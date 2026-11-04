/**
 * Purpose: DOCK / LEAVE taps - the HTTP side; see boatmode_ctl.h (ANC-44, Ship's Automation D-151).
 * Version: v1 (2026-10-15)
 * Scope: PRODUCTION - anchor-box firmware (live on the box at the nav station, installed by signed OTA).
 * Architecture: anchor-box firmware, control layer; same pattern as hotwater_ctl.c. Served by the "engine" task
 *   (engine_link.c, priority 1): a pending tap -> POST {"mode": "DOCK" | "MOTOR", "by": "anchor-box"} with
 *   "Authorization: Bearer <token>" (4 s timeout), then an immediate GET (the POST answers only {mode, manual});
 *   otherwise GET every CONFIG_ANCHORBOX_MODE_POLL_S (2 s timeout). The engine's GET reply is the only state shown.
 *   A 403 carries the engine's reason (no crew phone aboard) - shown for 30 s. Link lost after 2 failed polls in a
 *   row. Nothing here touches the anchor watch, DO0 or DO1. The token is never logged nor shown.
 * Invocation: boatmode_ctl_service() called by engine_link.c; imported by app_main.c, ui_watch.c, boatclock.c.
 */
#include "boatmode_ctl.h"

#include <stdio.h>
#include <string.h>
#include "engine_link.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "boatmode";

#define MSG_SHOW_US (30LL * 1000000LL)

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bm_view_t s_v = {                 /* guarded by s_mux */
    .wifi = sizeof(CONFIG_ANCHORBOX_WIFI_SSID) > 1,
    .token = sizeof(CONFIG_ANCHORBOX_NIGHT_WATCH_TOKEN) > 1,
    .get_http = -1,
};
static int64_t s_msg_until_us;
static int64_t s_last_ok_us = -1;        /* last valid engine reply */
static int64_t s_next_poll_us;           /* engine task only */
static int s_fails;
static int s_post_http = -1;

int64_t boatmode_ctl_service(int64_t now)
{
    bool post;
    bm_mode_t m;
    taskENTER_CRITICAL(&s_mux);
    post = s_v.pending;
    m = s_v.pending_mode;
    taskEXIT_CRITICAL(&s_mux);

    if (post) {
        char req[64];
        bm_post_body(m, req, sizeof(req));
        const char *body;
        int st = engine_link_request("/mode", req, true, 4000, &body);
        char msg[96] = "";
        if (st != 200) {
            if (st == 403 && bm_detail(body, msg, sizeof(msg))) { /* the engine's own reason (no crew phone aboard) */ }
            else if (st == 401) snprintf(msg, sizeof(msg), "token refused (401)");
            else if (st == 404 || st == 405) snprintf(msg, sizeof(msg), "engine too old for box mode taps (HTTP %d)", st);
            else if (st == 503) snprintf(msg, sizeof(msg), "engine has no token (503)");
            else if (st == 0) snprintf(msg, sizeof(msg), "engine not answering");
            else snprintf(msg, sizeof(msg), "engine said HTTP %d", st);
        }
        int64_t t = esp_timer_get_time();
        taskENTER_CRITICAL(&s_mux);
        s_v.pending = false;
        s_post_http = st;
        if (st == 200) {
            s_v.msg[0] = 0;
        } else {
            strlcpy(s_v.msg, msg, sizeof(s_v.msg));
            s_msg_until_us = t + MSG_SHOW_US;
        }
        taskEXIT_CRITICAL(&s_mux);
        if (st == 200) ESP_LOGI(TAG, "tap %s -> engine ok", bm_name(m));
        else ESP_LOGW(TAG, "tap %s: %s", bm_name(m), msg);
        s_next_poll_us = t;                     /* read the engine's view back at once */
        return s_next_poll_us;
    }

    if (now < s_next_poll_us) return s_next_poll_us;
    bm_reply_t r;
    const char *body;
    int st = engine_link_request("/mode", NULL, false, 2000, &body);
    bool ok = st == 200 && bm_parse(body, &r);
    int64_t t = esp_timer_get_time();
    s_next_poll_us = t + CONFIG_ANCHORBOX_MODE_POLL_S * 1000000LL;
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

void boatmode_ctl_start(void)
{
    if (!s_v.wifi) ESP_LOGW(TAG, "no Wi-Fi on this box - no DOCK / LEAVE taps");
    else ESP_LOGI(TAG, "engine /mode every %d s via the engine link, token %s",
                  CONFIG_ANCHORBOX_MODE_POLL_S, s_v.token ? "set" : "NOT SET");
}

void boatmode_ctl_get(bm_view_t *v)
{
    int64_t t = esp_timer_get_time();
    taskENTER_CRITICAL(&s_mux);
    if (s_v.msg[0] && t > s_msg_until_us) s_v.msg[0] = 0;
    *v = s_v;
    taskEXIT_CRITICAL(&s_mux);
}

bool boatmode_ctl_tap(bm_mode_t m)
{
    bool go = false;
    taskENTER_CRITICAL(&s_mux);
    if ((m == BM_DOCK && bm_dock_ok(&s_v)) || (m == BM_MOTOR && bm_leave_ok(&s_v))) {
        s_v.pending = true;
        s_v.pending_mode = m;
        s_v.msg[0] = 0;
        go = true;
    }
    taskEXIT_CRITICAL(&s_mux);
    if (go) engine_link_kick();
    return go;
}

void boatmode_ctl_status(char *buf, size_t n)
{
    bm_view_t v;
    boatmode_ctl_get(&v);
    int ph, age;
    taskENTER_CRITICAL(&s_mux);
    ph = s_post_http;
    age = s_last_ok_us < 0 ? -1 : (int)((esp_timer_get_time() - s_last_ok_us) / 1000000);
    taskEXIT_CRITICAL(&s_mux);
    if (!v.wifi) {
        snprintf(buf, n, "boat mode: no link (no Wi-Fi on this box)");
    } else if (!v.link) {
        snprintf(buf, n, "boat mode: %s - engine %s:%d (HTTP %d), token %s", v.polled ? "NO LINK" : "not polled yet",
                 CONFIG_ANCHORBOX_PI_HOST, CONFIG_ANCHORBOX_ENGINE_PORT, v.get_http, v.token ? "set" : "MISSING");
    } else {
        snprintf(buf, n, "boat mode: %s%s, %d crew phone%s aboard, leave %s - link ok, %d s ago, token %s%s%s%s",
                 v.eng.mode == BM_NONE ? "none" : bm_name(v.eng.mode), v.eng.secured ? " (secured)" : "", v.eng.crew,
                 v.eng.crew == 1 ? "" : "s", v.eng.leave_ok ? "ok" : "NOT ok", age, v.token ? "set" : "MISSING",
                 v.pending ? ", SENDING" : "", v.msg[0] ? " | " : "", v.msg);
    }
    if (ph >= 0) {
        char t[32];
        snprintf(t, sizeof(t), " | last tap HTTP %d", ph);
        strlcat(buf, t, n);
    }
}
