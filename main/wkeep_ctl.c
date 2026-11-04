/**
 * Purpose: Watchkeeper check-in on the box - the HTTP side; see wkeep_ctl.h (ANQ-27, Ship's Automation WF-614).
 * Version: v1 (2026-10-21)
 * Scope: PRODUCTION - anchor-box firmware (live on the box at the nav station, installed by signed OTA).
 * Architecture: anchor-box firmware, control layer. Served by the "engine" task (engine_link.c, priority 1); a tap
 *   sets a flag and kicks that task; a pending I'M UP -> POST /watch {"action": "up", "by": "anchor-box"} with
 *   "Authorization: Bearer <box token>" (4 s timeout) - the engine tags it api:anchor-box and lets this token tap
 *   I'M UP only; otherwise GET /watch every CONFIG_ANCHORBOX_WATCHKEEPER_POLL_S (2 s timeout). Link lost after 2
 *   failed polls in a row. Nothing here touches the anchor watch, DO0 or DO1. The token is never logged nor shown.
 * Invocation: wkeep_ctl_service() called by engine_link.c; imported by app_main.c, screens.c, boatclock.c.
 */
#include "wkeep_ctl.h"

#include <stdio.h>
#include <string.h>
#include "engine_link.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "wkeep";

#define MSG_SHOW_US (30LL * 1000000LL)
#define OK_SHOW_US (4LL * 1000000LL)

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static wk_view_t s_v = {                 /* guarded by s_mux */
    .wifi = sizeof(CONFIG_ANCHORBOX_WIFI_SSID) > 1,
    .token = sizeof(CONFIG_ANCHORBOX_NIGHT_WATCH_TOKEN) > 1,
    .get_http = -1,
};
static int64_t s_msg_until_us;
static int64_t s_last_ok_us = -1;        /* last valid engine reply */
static int64_t s_next_poll_us;           /* engine task only */
static int s_fails;
static int s_post_http = -1;

int64_t wkeep_ctl_service(int64_t now)
{
    static wk_reply_t r;                 /* engine task only */
    bool post;
    taskENTER_CRITICAL(&s_mux);
    post = s_v.pending;
    taskEXIT_CRITICAL(&s_mux);

    if (post) {
        char req[64];
        wk_post_body(req, sizeof(req));
        const char *body;
        int st = engine_link_request("/watch", req, true, 4000, &body);
        bool ok = st == 200 && wk_parse(body, &r);
        char msg[96] = "";
        if (ok) snprintf(msg, sizeof(msg), "I'M UP taken - next check-in in %d min",
                         r.next_prompt_s > 0 ? (r.next_prompt_s + 59) / 60 : 15);
        else if ((st == 409 || st == 403 || st == 422) && wk_detail(body, msg, sizeof(msg))) {
            /* the engine's own reason, e.g. 'no watch timer running' */
        } else if (st == 401) snprintf(msg, sizeof(msg), "not sent: token refused (401) - use the cockpit tablet");
        else if (st == 404 || st == 405) snprintf(msg, sizeof(msg), "not sent: engine has no /watch yet (%d)", st);
        else if (st == 503) snprintf(msg, sizeof(msg), "not sent: engine has no token (503)");
        else if (st == 0) snprintf(msg, sizeof(msg), "not sent: engine not answering - tap again");
        else if (st == 200) snprintf(msg, sizeof(msg), "not sent: bad reply from the engine");
        else snprintf(msg, sizeof(msg), "not sent: engine said HTTP %d", st);
        int64_t t = esp_timer_get_time();
        taskENTER_CRITICAL(&s_mux);
        s_v.pending = false;
        s_post_http = st;
        if (ok) {
            s_v.eng = r;
            s_v.link = s_v.polled = true;
            s_fails = 0;
            s_last_ok_us = t;
        }
        s_v.msg_ok = ok;
        strlcpy(s_v.msg, msg, sizeof(s_v.msg));
        s_msg_until_us = t + (ok ? OK_SHOW_US : MSG_SHOW_US);
        taskEXIT_CRITICAL(&s_mux);
        if (ok) ESP_LOGI(TAG, "I'M UP taken");
        else ESP_LOGW(TAG, "I'M UP: %s", msg);
        s_next_poll_us = t + CONFIG_ANCHORBOX_WATCHKEEPER_POLL_S * 1000000LL;
        return s_next_poll_us;
    }

    if (now < s_next_poll_us) return s_next_poll_us;
    const char *body;
    int st = engine_link_request("/watch", NULL, false, 2000, &body);
    bool ok = st == 200 && wk_parse(body, &r);
    int64_t t = esp_timer_get_time();
    s_next_poll_us = t + CONFIG_ANCHORBOX_WATCHKEEPER_POLL_S * 1000000LL;
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

void wkeep_ctl_start(void)
{
    if (!s_v.wifi) ESP_LOGW(TAG, "no Wi-Fi on this box - no watch check-in");
    else ESP_LOGI(TAG, "engine /watch every %d s via the engine link, token %s",
                  CONFIG_ANCHORBOX_WATCHKEEPER_POLL_S, s_v.token ? "set" : "NOT SET");
}

void wkeep_ctl_get(wk_view_t *v)
{
    int64_t t = esp_timer_get_time();
    taskENTER_CRITICAL(&s_mux);
    if (s_v.msg[0] && t > s_msg_until_us) s_v.msg[0] = 0;
    *v = s_v;
    taskEXIT_CRITICAL(&s_mux);
}

bool wkeep_ctl_up(void)
{
    bool go = false;
    taskENTER_CRITICAL(&s_mux);
    if (wk_tap(&s_v)) {
        s_v.pending = true;
        s_v.msg[0] = 0;
        go = true;
    }
    taskEXIT_CRITICAL(&s_mux);
    if (go) engine_link_kick();
    return go;
}

void wkeep_ctl_status(char *buf, size_t n)
{
    wk_view_t v;
    wkeep_ctl_get(&v);
    int ph, age;
    taskENTER_CRITICAL(&s_mux);
    ph = s_post_http;
    age = s_last_ok_us < 0 ? -1 : (int)((esp_timer_get_time() - s_last_ok_us) / 1000000);
    taskEXIT_CRITICAL(&s_mux);
    if (!v.wifi) {
        snprintf(buf, n, "watchkeeper: no link (no Wi-Fi on this box)");
    } else if (!v.link) {
        snprintf(buf, n, "watchkeeper: %s - engine %s:%d (HTTP %d), token %s", v.polled ? "NO LINK" : "not polled yet",
                 CONFIG_ANCHORBOX_PI_HOST, CONFIG_ANCHORBOX_ENGINE_PORT, v.get_http, v.token ? "set" : "MISSING");
    } else if (!v.eng.on) {
        snprintf(buf, n, "watchkeeper: off (%s) - link ok, %d s ago, token %s", v.eng.mode[0] ? v.eng.mode : "?", age,
                 v.token ? "set" : "MISSING");
    } else {
        snprintf(buf, n, "watchkeeper: ON stage %d, %d min since I'm up, next check-in %d s - link ok, %d s ago, token %s",
                 v.eng.stage, v.eng.since_up_s / 60, v.eng.next_prompt_s, age, v.token ? "set" : "MISSING");
    }
    size_t k = strlen(buf);
    if (k < n && (v.pending || v.msg[0]))
        snprintf(buf + k, n - k, "%s%s%s", v.pending ? ", SENDING" : "", v.msg[0] ? " | " : "", v.msg);
    if (ph >= 0) {
        char t[32];
        snprintf(t, sizeof(t), " | last tap HTTP %d", ph);
        strlcat(buf, t, n);
    }
}
