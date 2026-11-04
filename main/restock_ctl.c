/**
 * Purpose: Cold-box 'EXPECTED?' question - the HTTP side; see restock_ctl.h (ANC-45, Ship's Automation WF-502).
 * Version: v1 (2026-10-18)
 * Scope: PRODUCTION - anchor-box firmware (live on the box at the nav station, installed by signed OTA).
 * Architecture: anchor-box firmware, control layer; same pattern as hotwater_ctl.c. Served by the "engine" task
 *   (engine_link.c, priority 1): a pending answer -> POST {"box", "answer": yes | no, "by": "anchor-box"} with
 *   "Authorization: Bearer <token>" (4 s timeout); otherwise GET every CONFIG_ANCHORBOX_RESTOCK_POLL_S (2 s timeout).
 *   The reply (~0.45 KB, < 1.5 KB with four marks) fits the link's 2 KB buffer. The engine's reply (GET or POST) is
 *   the only state shown. A 409 (that box's WF-501 alarm is already up) carries the engine's reason - shown 30 s; a
 *   taken answer is confirmed for 5 s. Link lost after 2 failed polls in a row. Nothing here touches the anchor
 *   watch, DO0 or DO1. The token is never logged nor shown.
 * Invocation: restock_ctl_service() called by engine_link.c; imported by app_main.c, screens.c, boatclock.c.
 */
#include "restock_ctl.h"

#include <stdio.h>
#include <string.h>
#include "engine_link.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "restock";

#define MSG_SHOW_US (30LL * 1000000LL)
#define OK_SHOW_US (5LL * 1000000LL)

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static rs_view_t s_v = {                 /* guarded by s_mux */
    .wifi = sizeof(CONFIG_ANCHORBOX_WIFI_SSID) > 1,
    .token = sizeof(CONFIG_ANCHORBOX_NIGHT_WATCH_TOKEN) > 1,
    .get_http = -1,
    .pending_box = -1,
};
static int64_t s_msg_until_us;
static int64_t s_last_ok_us = -1;        /* last valid engine reply */
static int64_t s_next_poll_us;           /* engine task only */
static int s_fails;
static int s_post_http = -1;

int64_t restock_ctl_service(int64_t now)
{
    static rs_reply_t r;                 /* engine task only; static keeps it off the task stack */
    bool post, yes;
    int box;
    char label[sizeof(r.b[0].label)] = "?";
    taskENTER_CRITICAL(&s_mux);
    post = s_v.pending;
    yes = s_v.pending_yes;
    box = s_v.pending_box;
    if (post && box >= 0 && box < RS_BOXES) strlcpy(label, s_v.eng.b[box].label, sizeof(label));
    taskEXIT_CRITICAL(&s_mux);

    if (post) {
        char req[96];
        rs_post_body(box, yes, req, sizeof(req));
        const char *body;
        int st = engine_link_request("/restock", req, true, 4000, &body);
        bool ok = st == 200 && rs_parse(body, &r);
        char msg[128] = "";
        if (ok) {
            if (yes) snprintf(msg, sizeof(msg), "YES taken - the %s alarm waits %g h", label, r.hold_h);
            else snprintf(msg, sizeof(msg), "NO taken - the %s alarm is on now", label);
        } else if ((st == 409 || st == 400) && rs_detail(body, msg, sizeof(msg))) {
            /* the engine's own reason, e.g. that box's alarm is already up */
        } else if (st == 401) snprintf(msg, sizeof(msg), "not answered: token refused (401) - use the cockpit page");
        else if (st == 404) snprintf(msg, sizeof(msg), "not answered: engine has no /restock yet (404)");
        else if (st == 503) snprintf(msg, sizeof(msg), "not answered: engine has no token (503)");
        else if (st == 0) snprintf(msg, sizeof(msg), "not answered: engine not answering - try again");
        else if (st == 200) snprintf(msg, sizeof(msg), "not answered: bad reply from the engine");
        else snprintf(msg, sizeof(msg), "not answered: engine said HTTP %d", st);
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
        if (ok) ESP_LOGI(TAG, "%s %s -> taken", RS_KEYS[box], yes ? "YES" : "NO");
        else ESP_LOGW(TAG, "%s %s: %s", box >= 0 ? RS_KEYS[box] : "?", yes ? "YES" : "NO", msg);
        s_next_poll_us = ok ? t + CONFIG_ANCHORBOX_RESTOCK_POLL_S * 1000000LL : t;
        return s_next_poll_us;
    }

    if (now < s_next_poll_us) return s_next_poll_us;
    const char *body;
    int st = engine_link_request("/restock", NULL, false, 2000, &body);
    bool ok = st == 200 && rs_parse(body, &r);
    int64_t t = esp_timer_get_time();
    s_next_poll_us = t + CONFIG_ANCHORBOX_RESTOCK_POLL_S * 1000000LL;
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

void restock_ctl_start(void)
{
    if (!s_v.wifi) ESP_LOGW(TAG, "no Wi-Fi on this box - no EXPECTED? question");
    else ESP_LOGI(TAG, "engine /restock every %d s via the engine link, token %s",
                  CONFIG_ANCHORBOX_RESTOCK_POLL_S, s_v.token ? "set" : "NOT SET");
}

void restock_ctl_get(rs_view_t *v)
{
    int64_t t = esp_timer_get_time();
    taskENTER_CRITICAL(&s_mux);
    if (s_v.msg[0] && t > s_msg_until_us) s_v.msg[0] = 0;
    *v = s_v;
    taskEXIT_CRITICAL(&s_mux);
}

bool restock_ctl_answer(bool yes)
{
    bool go = false;
    int box;
    taskENTER_CRITICAL(&s_mux);
    if (rs_tap(&s_v, &box)) {
        s_v.pending = true;
        s_v.pending_yes = yes;
        s_v.pending_box = box;
        s_v.msg[0] = 0;
        go = true;
    }
    taskEXIT_CRITICAL(&s_mux);
    if (go) engine_link_kick();
    return go;
}

void restock_ctl_status(char *buf, size_t n)
{
    rs_view_t v;
    restock_ctl_get(&v);
    int ph, age;
    taskENTER_CRITICAL(&s_mux);
    ph = s_post_http;
    age = s_last_ok_us < 0 ? -1 : (int)((esp_timer_get_time() - s_last_ok_us) / 1000000);
    taskEXIT_CRITICAL(&s_mux);
    if (!v.wifi) {
        snprintf(buf, n, "restock: no link (no Wi-Fi on this box)");
    } else if (!v.link) {
        snprintf(buf, n, "restock: %s - engine %s:%d (HTTP %d), token %s", v.polled ? "NO LINK" : "not polled yet",
                 CONFIG_ANCHORBOX_PI_HOST, CONFIG_ANCHORBOX_ENGINE_PORT, v.get_http, v.token ? "set" : "MISSING");
    } else {
        int count, b = rs_asking(&v, &count);
        char marks[64] = "";
        for (int i = 0; i < RS_BOXES; i++) {
            if (v.eng.b[i].mark == RS_MARK_NONE) continue;
            char t[24];
            snprintf(t, sizeof(t), "%s%s %s", marks[0] ? ", " : "", RS_KEYS[i],
                     v.eng.b[i].mark == RS_MARK_HOLD ? "hold" : "NO");
            strlcat(marks, t, sizeof(marks));
        }
        if (b >= 0) snprintf(buf, n, "restock: ASKING %s (%d asking)", v.eng.b[b].label, count);
        else snprintf(buf, n, "restock: none asking");
        size_t k = strlen(buf);
        snprintf(buf + k, n > k ? n - k : 0, ", marks %s, hold %g h - link ok, %d s ago, token %s%s%s%s",
                 marks[0] ? marks : "none", v.eng.hold_h, age, v.token ? "set" : "MISSING",
                 v.pending ? ", SENDING" : "", v.msg[0] ? " | " : "", v.msg);
    }
    if (ph >= 0) {
        char t[32];
        snprintf(t, sizeof(t), " | last answer HTTP %d", ph);
        strlcat(buf, t, n);
    }
}
