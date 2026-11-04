/**
 * Purpose: Night-watch remote control - the HTTP side; see nightwatch_ctl.h (ANQ-30, ANC-27).
 * Version: v2 (2026-10-09) - v2: no own task / HTTP client - nightwatch_ctl_service() runs on the shared engine link
 *   (engine_link.c, ANQ-33). v1 (2026-10-06)
 * Scope: PRODUCTION - anchor-box firmware (live on the box at the nav station, installed by signed OTA).
 * Architecture: anchor-box firmware, control layer. Served by the "engine" task (engine_link.c, priority 1); a tap
 *   sets a flag and kicks that task; a pending tap -> POST {"armed": x, "by": "anchor-box"} with "Authorization: Bearer <token>" (4 s timeout);
 *   otherwise GET every NIGHT_WATCH_POLL_S (2 s timeout). The engine's reply (GET or POST) is the only state shown.
 *   Link lost after 2 failed polls in a row (one Wi-Fi blip does not grey the bar). A failed tap leaves the state
 *   alone and shows why for 30 s ("not changed: token refused (401)" ...). Nothing here touches the anchor watch,
 *   DO0 or DO1. The token is never logged nor shown.
 * Invocation: imported by app_main.c (nightwatch_ctl_start before engine_link_start), ui_clock.c, boatclock.c;
 *   nightwatch_ctl_service() is called by engine_link.c.
 */
#include "nightwatch_ctl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "engine_link.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "nightwatch";

#define ERR_SHOW_US (30LL * 1000000LL)

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static nw_view_t s_v = {                 /* guarded by s_mux; known from the build before the link starts */
    .wifi = sizeof(CONFIG_ANCHORBOX_WIFI_SSID) > 1,
    .token = sizeof(CONFIG_ANCHORBOX_NIGHT_WATCH_TOKEN) > 1,
};
static int64_t s_err_until_us;
static int64_t s_last_ok_us = -1;        /* last valid engine reply */
static int64_t s_next_poll_us;           /* engine task only */
static int s_fails;                      /* failed polls in a row */
static int s_get_http = -1, s_post_http = -1;   /* last HTTP status, 0 = no answer, -1 = none yet */

/* engine task (engine_link.c): a pending tap, else a due poll */
int64_t nightwatch_ctl_service(int64_t now)
{
    static nw_reply_t r;
    bool post, arm;
    taskENTER_CRITICAL(&s_mux);
    post = s_v.pending;
    arm = s_v.pending_arm;
    taskEXIT_CRITICAL(&s_mux);

    if (post) {
        const char *body;
        int st = engine_link_request("/night-watch", arm ? "{\"armed\": true, \"by\": \"anchor-box\"}"
                                                         : "{\"armed\": false, \"by\": \"anchor-box\"}",
                                     true, 4000, &body);
        bool ok = st == 200 && nw_parse(body, &r);
        char err[48] = "";
        if (!ok) {
            if (st == 401) snprintf(err, sizeof(err), "not changed: token refused (401)");
            else if (st == 503) snprintf(err, sizeof(err), "not changed: engine has no token (503)");
            else if (st == 0) snprintf(err, sizeof(err), "not changed: engine not answering");
            else if (st == 200) snprintf(err, sizeof(err), "not changed: bad reply from the engine");
            else snprintf(err, sizeof(err), "not changed: engine said HTTP %d", st);
        }
        int64_t t = esp_timer_get_time();
        taskENTER_CRITICAL(&s_mux);
        s_v.pending = false;
        s_post_http = st;
        if (ok) {
            s_v.eng = r;
            s_v.link = s_v.polled = true;
            s_v.err[0] = 0;
            s_fails = 0;
            s_last_ok_us = t;
        } else {
            strlcpy(s_v.err, err, sizeof(s_v.err));
            s_err_until_us = t + ERR_SHOW_US;
        }
        taskEXIT_CRITICAL(&s_mux);
        if (ok) ESP_LOGI(TAG, "tap -> engine says %s", r.armed ? "ARMED" : "OFF");
        else ESP_LOGW(TAG, "tap (%s) %s", arm ? "arm" : "off", err);
        s_next_poll_us = ok ? t + CONFIG_ANCHORBOX_NIGHT_WATCH_POLL_S * 1000000LL : t;   /* failed: look again now */
        return s_next_poll_us;
    }

    if (now < s_next_poll_us) return s_next_poll_us;
    const char *body;
    int st = engine_link_request("/night-watch", NULL, false, 2000, &body);
    bool ok = st == 200 && nw_parse(body, &r);
    int64_t t = esp_timer_get_time();
    s_next_poll_us = t + CONFIG_ANCHORBOX_NIGHT_WATCH_POLL_S * 1000000LL;
    taskENTER_CRITICAL(&s_mux);
    s_v.polled = true;
    s_get_http = st;
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

void nightwatch_ctl_start(void)
{
    if (!s_v.wifi) ESP_LOGW(TAG, "no Wi-Fi on this box - night watch shows 'no link'");
    else ESP_LOGI(TAG, "engine /night-watch every %d s via the engine link, token %s",
                  CONFIG_ANCHORBOX_NIGHT_WATCH_POLL_S, s_v.token ? "set" : "NOT SET");
}

void nightwatch_ctl_get(nw_view_t *v)
{
    int64_t t = esp_timer_get_time();
    taskENTER_CRITICAL(&s_mux);
    if (s_v.err[0] && t > s_err_until_us) s_v.err[0] = 0;
    *v = s_v;
    taskEXIT_CRITICAL(&s_mux);
}

bool nightwatch_ctl_toggle(void)
{
    bool go = false;
    taskENTER_CRITICAL(&s_mux);
    if (s_v.link && s_v.token && !s_v.pending) {
        s_v.pending = true;
        s_v.pending_arm = !s_v.eng.armed;   /* the opposite of what the ENGINE last said */
        s_v.err[0] = 0;
        go = true;
    }
    taskEXIT_CRITICAL(&s_mux);
    if (go) engine_link_kick();
    return go;
}

void nightwatch_ctl_status(char *buf, size_t n)
{
    nw_view_t v;
    nightwatch_ctl_get(&v);
    int64_t last;
    int gh, ph;
    taskENTER_CRITICAL(&s_mux);
    last = s_last_ok_us;
    gh = s_get_http;
    ph = s_post_http;
    taskEXIT_CRITICAL(&s_mux);
    char age[32] = "never";
    if (last >= 0) snprintf(age, sizeof(age), "%lld s ago", (long long)((esp_timer_get_time() - last) / 1000000));
    if (!v.wifi) {
        snprintf(buf, n, "night watch: no link (no Wi-Fi on this box)");
    } else if (!v.link) {
        snprintf(buf, n, "night watch: %s - engine %s:%d, last answer %s (HTTP %d), token %s",
                 v.polled ? "NO LINK" : "not polled yet", CONFIG_ANCHORBOX_PI_HOST, CONFIG_ANCHORBOX_ENGINE_PORT, age,
                 gh, v.token ? "set" : "MISSING");
    } else {
        snprintf(buf, n, "night watch %s%s%s (by %s, since %s) - link ok, last answer %s, token %s%s%s",
                 v.eng.armed ? "ARMED" : "OFF", v.eng.nudge ? " NUDGE" : "", v.eng.night ? " night" : "",
                 v.eng.by[0] ? v.eng.by : "-", v.eng.since[0] ? v.eng.since : "-", age, v.token ? "set" : "MISSING",
                 v.pending ? ", SENDING" : "", v.err[0] ? " | " : "");
        if (v.err[0]) strlcat(buf, v.err, n);
    }
    if (ph >= 0) {
        char t[40];
        snprintf(t, sizeof(t), " | last tap HTTP %d", ph);
        strlcat(buf, t, n);
    }
}
