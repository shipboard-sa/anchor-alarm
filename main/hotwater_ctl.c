/**
 * Purpose: HOT WATER tap - the HTTP side; see hotwater_ctl.h (ANC-43, Ship's Automation D-135).
 * Version: v1 (2026-10-11)
 * Scope: PRODUCTION - anchor-box firmware (live on the box at the nav station, installed by signed OTA).
 * Architecture: anchor-box firmware, control layer; same pattern as fill_ctl.c. Served by the "engine" task
 *   (engine_link.c, priority 1): a pending tap -> POST {"on": x, "by": "anchor-box"} with "Authorization: Bearer
 *   <token>" (4 s timeout); otherwise GET every CONFIG_ANCHORBOX_HOT_WATER_POLL_S (2 s timeout). The engine's reply
 *   (GET or POST) is the only state shown. A 409 carries the engine's reason (bank below the floor) - shown for 30 s.
 *   Link lost after 2 failed polls in a row. Nothing here touches the anchor watch, DO0 or DO1. The token is never
 *   logged nor shown.
 * Invocation: hotwater_ctl_service() called by engine_link.c; imported by app_main.c, ui_watch.c, boatclock.c.
 */
#include "hotwater_ctl.h"

#include <stdio.h>
#include <string.h>
#include "engine_link.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "hotwater";

#define MSG_SHOW_US (30LL * 1000000LL)

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static hw_view_t s_v = {                 /* guarded by s_mux */
    .wifi = sizeof(CONFIG_ANCHORBOX_WIFI_SSID) > 1,
    .token = sizeof(CONFIG_ANCHORBOX_NIGHT_WATCH_TOKEN) > 1,
    .get_http = -1,
};
static int64_t s_msg_until_us;
static int64_t s_last_ok_us = -1;        /* last valid engine reply */
static int64_t s_next_poll_us;           /* engine task only */
static int s_fails;
static int s_post_http = -1;

int64_t hotwater_ctl_service(int64_t now)
{
    hw_reply_t r;
    bool post, on;
    taskENTER_CRITICAL(&s_mux);
    post = s_v.pending;
    on = s_v.pending_on;
    taskEXIT_CRITICAL(&s_mux);

    if (post) {
        char req[64];
        snprintf(req, sizeof(req), "{\"on\": %s, \"by\": \"anchor-box\"}", on ? "true" : "false");
        const char *body;
        int st = engine_link_request("/hot-water", req, true, 4000, &body);
        bool ok = st == 200 && hw_parse(body, &r);
        char msg[80] = "";
        if (!ok) {
            if (st == 409 && hw_detail(body, msg, sizeof(msg))) { /* the engine's own reason, e.g. bank below floor */ }
            else if (st == 401) snprintf(msg, sizeof(msg), "not changed: token refused (401)");
            else if (st == 404) snprintf(msg, sizeof(msg), "not changed: engine has no /hot-water yet (404)");
            else if (st == 503) snprintf(msg, sizeof(msg), "not changed: engine has no token (503)");
            else if (st == 0) snprintf(msg, sizeof(msg), "not changed: engine not answering");
            else if (st == 200) snprintf(msg, sizeof(msg), "not changed: bad reply from the engine");
            else snprintf(msg, sizeof(msg), "not changed: engine said HTTP %d", st);
        }
        int64_t t = esp_timer_get_time();
        taskENTER_CRITICAL(&s_mux);
        s_v.pending = false;
        s_post_http = st;
        if (ok) {
            s_v.eng = r;
            s_v.link = s_v.polled = true;
            s_v.msg[0] = 0;
            s_fails = 0;
            s_last_ok_us = t;
        } else {
            strlcpy(s_v.msg, msg, sizeof(s_v.msg));
            s_msg_until_us = t + MSG_SHOW_US;
        }
        taskEXIT_CRITICAL(&s_mux);
        if (ok) ESP_LOGI(TAG, "tap %s -> engine: %s", on ? "start" : "stop", r.on ? "on" : "off");
        else ESP_LOGW(TAG, "tap %s: %s", on ? "start" : "stop", msg);
        s_next_poll_us = ok ? t + CONFIG_ANCHORBOX_HOT_WATER_POLL_S * 1000000LL : t;
        return s_next_poll_us;
    }

    if (now < s_next_poll_us) return s_next_poll_us;
    const char *body;
    int st = engine_link_request("/hot-water", NULL, false, 2000, &body);
    bool ok = st == 200 && hw_parse(body, &r);
    int64_t t = esp_timer_get_time();
    s_next_poll_us = t + CONFIG_ANCHORBOX_HOT_WATER_POLL_S * 1000000LL;
    taskENTER_CRITICAL(&s_mux);
    s_v.polled = true;
    s_v.get_http = st;
    if (ok) {
        s_v.eng = r;
        s_v.link = true;
        s_fails = 0;
        s_last_ok_us = t;
    } else if (++s_fails >= 2) {
        s_v.link = false;
    }
    taskEXIT_CRITICAL(&s_mux);
    return s_next_poll_us;
}

void hotwater_ctl_start(void)
{
    if (!s_v.wifi) ESP_LOGW(TAG, "no Wi-Fi on this box - HOT WATER shows 'no link'");
    else ESP_LOGI(TAG, "engine /hot-water every %d s via the engine link, token %s",
                  CONFIG_ANCHORBOX_HOT_WATER_POLL_S, s_v.token ? "set" : "NOT SET");
}

void hotwater_ctl_get(hw_view_t *v)
{
    int64_t t = esp_timer_get_time();
    taskENTER_CRITICAL(&s_mux);
    if (s_v.msg[0] && t > s_msg_until_us) s_v.msg[0] = 0;
    s_v.reply_age_s = s_last_ok_us < 0 ? 0 : (int)((t - s_last_ok_us) / 1000000);
    *v = s_v;
    taskEXIT_CRITICAL(&s_mux);
}

bool hotwater_ctl_tap(void)
{
    bool go = false, on;
    taskENTER_CRITICAL(&s_mux);
    if (hw_tap(&s_v, &on)) {
        s_v.pending = true;
        s_v.pending_on = on;
        s_v.msg[0] = 0;
        go = true;
    }
    taskEXIT_CRITICAL(&s_mux);
    if (go) engine_link_kick();
    return go;
}

void hotwater_ctl_status(char *buf, size_t n)
{
    hw_view_t v;
    hotwater_ctl_get(&v);
    int ph;
    taskENTER_CRITICAL(&s_mux);
    ph = s_post_http;
    taskEXIT_CRITICAL(&s_mux);
    static const char *const ST[] = { "off", "HEATING", "on (no draw yet)", "HELD (lockout / inverter shed)" };
    static const char *const LAST[] = { "-", "hot", "no draw", "time limit", "stopped" };
    if (!v.wifi) {
        snprintf(buf, n, "hot water: no link (no Wi-Fi on this box)");
    } else if (!v.link) {
        snprintf(buf, n, "hot water: %s - engine %s:%d (HTTP %d), token %s", v.polled ? "NO LINK" : "not polled yet",
                 CONFIG_ANCHORBOX_PI_HOST, CONFIG_ANCHORBOX_ENGINE_PORT, v.get_http, v.token ? "set" : "MISSING");
    } else {
        char amps[16] = "?", soc[16] = "?";
        if (v.eng.has_amps) snprintf(amps, sizeof(amps), "%.1f A", v.eng.amps);
        if (v.eng.has_soc) snprintf(soc, sizeof(soc), "%.0f %%", v.eng.soc);
        snprintf(buf, n, "hot water: %s, heater %s, bank %s (floor %.0f %%), last run %s - link ok, %d s ago, "
                 "token %s%s%s%s", ST[v.eng.state], amps, soc, v.eng.soc_floor,
                 LAST[v.eng.last], v.reply_age_s, v.token ? "set" : "MISSING", v.pending ? ", SENDING" : "",
                 v.msg[0] ? " | " : "", v.msg);
        if (v.eng.on) {
            char t[24];
            snprintf(t, sizeof(t), " | %d s left", v.eng.remaining_s);
            strlcat(buf, t, n);
        }
    }
    if (ph >= 0) {
        char t[32];
        snprintf(t, sizeof(t), " | last tap HTTP %d", ph);
        strlcat(buf, t, n);
    }
}
