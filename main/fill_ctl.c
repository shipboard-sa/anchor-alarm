/**
 * Purpose: Tank-fill taps - the HTTP side; see fill_ctl.h (ANC-41, Ship's Automation D-129).
 * Version: v2 (2026-10-09) - v2: no own task / HTTP client / reply buffer - fill_ctl_service() runs on the shared
 *   engine link (engine_link.c, ANQ-33). v1 (2026-10-09)
 * Scope: PRODUCTION - anchor-box firmware (live on the box at the nav station, installed by signed OTA).
 * Architecture: anchor-box firmware, control layer; same pattern as nightwatch_ctl.c. Served by the "engine" task
 *   (engine_link.c, priority 1); a tap or the page opening sets a flag and kicks that task; a pending tap -> POST {"tank": k, "on": x, "by": "anchor-box"} with
 *   "Authorization: Bearer <token>" (4 s timeout); otherwise GET every CONFIG_ANCHORBOX_FILL_POLL_S, every 2 s while
 *   the FILL page is open (2 s timeout). The engine's reply (GET or POST) is the only state shown. Link lost after
 *   2 failed polls in a row. A failed tap leaves the state alone and shows why for 30 s. Nothing here touches the
 *   anchor watch, DO0 or DO1. The token is never logged nor shown.
 * Invocation: fill_ctl_service() called by engine_link.c; imported by app_main.c (fill_ctl_start before
 *   engine_link_start), ui_fill.c, ui_watch.c, screens.c, boatclock.c.
 */
#include "fill_ctl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "engine_link.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "fill";

#define ERR_SHOW_US (30LL * 1000000LL)
#define FAST_POLL_S 2

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static fl_view_t s_v = {                 /* guarded by s_mux; known from the build before the link starts */
    .wifi = sizeof(CONFIG_ANCHORBOX_WIFI_SSID) > 1,
    .token = sizeof(CONFIG_ANCHORBOX_NIGHT_WATCH_TOKEN) > 1,
    .get_http = -1,
    .pending_tank = -1,
};
static bool s_fast, s_poll_now;          /* guarded by s_mux */
static int64_t s_err_until_us;
static int64_t s_last_ok_us = -1;        /* last valid engine reply */
static int64_t s_next_poll_us;           /* engine task only */
static int s_fails;                      /* failed polls in a row */
static int s_post_http = -1;             /* last tap's HTTP status, 0 = no answer, -1 = none yet */

static int64_t poll_us(void)
{
    bool fast;
    taskENTER_CRITICAL(&s_mux);
    fast = s_fast;
    taskEXIT_CRITICAL(&s_mux);
    return (fast ? FAST_POLL_S : CONFIG_ANCHORBOX_FILL_POLL_S) * 1000000LL;
}

/* engine task (engine_link.c): a pending tap, else a due poll (or one asked for by the page opening) */
int64_t fill_ctl_service(int64_t now)
{
    static fl_reply_t r;                 /* ~300 bytes: off the task stack */
    bool post, on, poll_now;
    int tank;
    taskENTER_CRITICAL(&s_mux);
    post = s_v.pending;
    tank = s_v.pending_tank;
    on = s_v.pending_on;
    poll_now = s_poll_now;
    s_poll_now = false;
    taskEXIT_CRITICAL(&s_mux);

    if (post && tank >= 0 && tank < FL_TANKS) {
        char req[80];
        snprintf(req, sizeof(req), "{\"tank\": \"%s\", \"on\": %s, \"by\": \"anchor-box\"}", FL_KEYS[tank],
                 on ? "true" : "false");
        const char *body;
        int st = engine_link_request("/fill", req, true, 4000, &body);
        bool ok = st == 200 && fl_parse(body, &r);
        char err[48] = "";
        if (!ok) {
            if (st == 401) snprintf(err, sizeof(err), "not changed: token refused (401)");
            else if (st == 400) snprintf(err, sizeof(err), "not changed: engine doesn't know that tank");
            else if (st == 404) snprintf(err, sizeof(err), "not changed: engine has no /fill yet (404)");
            else if (st == 503) snprintf(err, sizeof(err), "not changed: engine has no token (503)");
            else if (st == 0) snprintf(err, sizeof(err), "not changed: engine not answering");
            else if (st == 200) snprintf(err, sizeof(err), "not changed: bad reply from the engine");
            else snprintf(err, sizeof(err), "not changed: engine said HTTP %d", st);
        }
        int64_t t = esp_timer_get_time();
        taskENTER_CRITICAL(&s_mux);
        s_v.pending = false;
        s_v.pending_tank = -1;
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
        if (ok) ESP_LOGI(TAG, "tap %s %s -> engine: %s", on ? "FILLING" : "STOP", FL_KEYS[tank],
                         r.t[tank].src != FL_SRC_NONE ? "session on" : "no session");
        else ESP_LOGW(TAG, "tap %s %s %s", on ? "FILLING" : "STOP", FL_KEYS[tank], err);
        s_next_poll_us = ok ? t + poll_us() : t;   /* failed: look again now */
        return s_next_poll_us;
    }

    if (!poll_now && now < s_next_poll_us) return s_next_poll_us;
    const char *body;
    int st = engine_link_request("/fill", NULL, false, 2000, &body);
    bool ok = st == 200 && fl_parse(body, &r);
    int64_t t = esp_timer_get_time();
    s_next_poll_us = t + poll_us();
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

void fill_ctl_start(void)
{
    if (!s_v.wifi) ESP_LOGW(TAG, "no Wi-Fi on this box - the FILL page shows 'no link'");
    else ESP_LOGI(TAG, "engine /fill every %d s (%d s with the page open) via the engine link, token %s",
                  CONFIG_ANCHORBOX_FILL_POLL_S, FAST_POLL_S, s_v.token ? "set" : "NOT SET");
}

void fill_ctl_get(fl_view_t *v)
{
    int64_t t = esp_timer_get_time();
    taskENTER_CRITICAL(&s_mux);
    if (s_v.err[0] && t > s_err_until_us) s_v.err[0] = 0;
    *v = s_v;
    taskEXIT_CRITICAL(&s_mux);
}

bool fill_ctl_tap(int i)
{
    bool go = false, on;
    taskENTER_CRITICAL(&s_mux);
    if (fl_tap(&s_v, i, &on)) {
        s_v.pending = true;
        s_v.pending_tank = i;
        s_v.pending_on = on;
        s_v.err[0] = 0;
        go = true;
    }
    taskEXIT_CRITICAL(&s_mux);
    if (go) engine_link_kick();
    return go;
}

void fill_ctl_fast(bool on)
{
    taskENTER_CRITICAL(&s_mux);
    bool was = s_fast;
    s_fast = on;
    if (on && !was) s_poll_now = true;   /* the page just opened: ask now */
    taskEXIT_CRITICAL(&s_mux);
    if (on && !was) engine_link_kick();
}

void fill_ctl_status(char *buf, size_t n)
{
    fl_view_t v;
    fill_ctl_get(&v);
    int64_t last;
    int ph;
    taskENTER_CRITICAL(&s_mux);
    last = s_last_ok_us;
    ph = s_post_http;
    taskEXIT_CRITICAL(&s_mux);
    char age[32] = "never";
    if (last >= 0) snprintf(age, sizeof(age), "%lld s ago", (long long)((esp_timer_get_time() - last) / 1000000));
    if (!v.wifi) {
        snprintf(buf, n, "fill: no link (no Wi-Fi on this box)");
    } else if (!v.link) {
        snprintf(buf, n, "fill: %s - engine %s:%d, last answer %s (HTTP %d), token %s",
                 v.polled ? "NO LINK" : "not polled yet", CONFIG_ANCHORBOX_PI_HOST, CONFIG_ANCHORBOX_ENGINE_PORT, age,
                 v.get_http, v.token ? "set" : "MISSING");
    } else {
        int k = snprintf(buf, n, "fill:");
        int sessions = 0;
        for (int i = 0; i < FL_TANKS && k > 0 && (size_t)k < n; i++) {
            const fl_tank_t *t = &v.eng.t[i];
            if (!t->present || t->src == FL_SRC_NONE) continue;
            sessions++;
            k += snprintf(buf + k, n - k, " %s %s%s (%s)", FL_KEYS[i],
                          t->src == FL_SRC_TAP ? "tap" : "watermaker", t->active ? "" : " waiting",
                          t->by[0] ? t->by : "-");
        }
        if (!sessions && k > 0 && (size_t)k < n) k += snprintf(buf + k, n - k, " no sessions");
        if (k > 0 && (size_t)k < n)
            snprintf(buf + k, n - k, ", watermaker %s - link ok, last answer %s, token %s%s%s",
                     v.eng.wm_running ? "RUNNING" : "off", age, v.token ? "set" : "MISSING",
                     v.pending ? ", SENDING" : "", v.err[0] ? " | " : "");
        if (v.err[0]) strlcat(buf, v.err, n);
    }
    if (ph >= 0) {
        char t[40];
        snprintf(t, sizeof(t), " | last tap HTTP %d", ph);
        strlcat(buf, t, n);
    }
}
