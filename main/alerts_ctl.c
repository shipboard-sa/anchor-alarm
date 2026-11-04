/**
 * Purpose: Engine alerts on the box - the HTTP side; see alerts_ctl.h (ANQ-10).
 * Version: v4 (2026-10-21) - v4: crew RESET (Ship's Automation D-163): alerts_ctl_reset(id) -> POST /reset/<rule>
 *   {"by": "anchor-box"} with the box token, then poll at once; the outcome (done / the engine's 409 reason) shows 30 s.
 *   v3 (2026-10-09) - v3: no own task / HTTP client / reply buffer - alerts_ctl_service() runs on the shared
 *   engine link (engine_link.c, ANQ-33). v2 (2026-10-07) - v2: tap-to-ack (10-07, risk accepted): POST /ack with the box token, then an
 *   immediate re-poll; a failed ACK shows why for 30 s. v1 (2026-10-07): GET only; view copies live in PSRAM.
 * Scope: PRODUCTION - anchor-box firmware (live on the box at the nav station, installed by signed OTA).
 * Architecture: anchor-box firmware, control layer. Served by the "engine" task (engine_link.c, priority 1 - below
 *   hostwatch 2, clock 3, watch 4, ui 4); a tap sets a flag and kicks that task; a pending ACK -> POST /ack {"by": "anchor-box"} with "Authorization: Bearer <box
 *   token>" (4 s timeout), then poll at once; otherwise GET /alerts every ALERTS_POLL_S (2 s timeout). Link lost after
 *   2 failed polls in a row. Nothing here touches the anchor watch, DO0 or DO1. The token is never logged nor shown.
 * Invocation: alerts_ctl_service() called by engine_link.c; imported by app_main.c (alerts_ctl_start before
 *   engine_link_start), screens.c, ui_alert.c, ui_clock.c,
 *   ui_watch.c, boatclock.c. Token = CONFIG_ANCHORBOX_NIGHT_WATCH_TOKEN (the box token, sdkconfig.secrets).
 */
#include "alerts_ctl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "engine_link.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "alerts";

#define ERR_SHOW_US (30LL * 1000000LL)
#define WIFI (sizeof(CONFIG_ANCHORBOX_WIFI_SSID) > 1)
#define TOKEN (sizeof(CONFIG_ANCHORBOX_NIGHT_WATCH_TOKEN) > 1)

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static al_view_t *s_v;           /* PSRAM (1.5 KB - internal RAM is tight); guarded by s_mux; NULL until started */
static int64_t s_last_ok_us = -1, s_err_until_us;
static int64_t s_next_poll_us;   /* engine task only */
static int s_fails, s_http = -1, s_ack_http = -1;
static al_reply_t *s_tmp;        /* parse target (PSRAM), copied in under the lock */

static void send_ack(void)
{
    const char *body;
    int st = engine_link_request("/ack", "{\"by\": \"anchor-box\"}", true, 4000, &body);
    char err[48] = "";
    if (st == 401) snprintf(err, sizeof(err), "ACK not sent: token refused (401)");
    else if (st == 503) snprintf(err, sizeof(err), "ACK not sent: engine has no token (503)");
    else if (st == 0) snprintf(err, sizeof(err), "ACK not sent: engine not answering");
    else if (st != 200) snprintf(err, sizeof(err), "ACK not sent: engine said HTTP %d", st);
    taskENTER_CRITICAL(&s_mux);
    s_v->ack_pending = false;
    s_ack_http = st;
    strlcpy(s_v->err, err, sizeof(s_v->err));
    s_err_until_us = esp_timer_get_time() + ERR_SHOW_US;
    taskEXIT_CRITICAL(&s_mux);
    if (err[0]) ESP_LOGW(TAG, "%s", err);
    else ESP_LOGI(TAG, "ACK sent (tap on the box)");
}

/* v4: a confirmed RESET (D-163) -> POST /reset/<rule>; the engine re-checks the input and answers 409 + why */
static void send_reset(void)
{
    char id[24], path[48];
    taskENTER_CRITICAL(&s_mux);
    strlcpy(id, s_v->reset_id, sizeof(id));
    taskEXIT_CRITICAL(&s_mux);
    snprintf(path, sizeof(path), "/reset/%s", id);
    const char *body;
    int st = engine_link_request(path, "{\"by\": \"anchor-box\"}", true, 4000, &body);
    char err[96] = "";
    char why[80];
    if (st == 200) snprintf(err, sizeof(err), "RESET %s done", id);
    else if ((st == 409 || st == 403 || st == 404) && al_detail(body, why, sizeof(why)))
        snprintf(err, sizeof(err), "RESET refused: %s", why);
    else if (st == 401) snprintf(err, sizeof(err), "RESET not allowed for this box (401) - use the cockpit");
    else if (st == 503) snprintf(err, sizeof(err), "RESET not sent: engine has no token (503)");
    else if (st == 0) snprintf(err, sizeof(err), "RESET not sent: engine not answering");
    else snprintf(err, sizeof(err), "RESET not sent: engine said HTTP %d", st);
    taskENTER_CRITICAL(&s_mux);
    s_v->reset_pending = false;
    s_ack_http = st;
    strlcpy(s_v->err, err, sizeof(s_v->err));
    s_err_until_us = esp_timer_get_time() + ERR_SHOW_US;
    taskEXIT_CRITICAL(&s_mux);
    if (st == 200) ESP_LOGI(TAG, "%s (tap on the box)", err);
    else ESP_LOGW(TAG, "%s", err);
}

/* engine task (engine_link.c): a pending RESET or ACK (then poll at once, so the screen follows within a second),
 * else a due poll */
int64_t alerts_ctl_service(int64_t now)
{
    if (!s_v) return now + 60LL * 1000000LL;   /* not started (no Wi-Fi / no PSRAM) */
    bool ack, reset;
    taskENTER_CRITICAL(&s_mux);
    ack = s_v->ack_pending;
    reset = s_v->reset_pending;
    taskEXIT_CRITICAL(&s_mux);
    if (reset) send_reset();
    else if (ack) send_ack();
    else if (now < s_next_poll_us) return s_next_poll_us;

    char path[24];
    snprintf(path, sizeof(path), "/alerts?max=%d", AL_MAX);
    const char *body;
    int st = engine_link_request(path, NULL, false, 2000, &body);
    bool ok = st == 200 && al_parse(body, s_tmp);
    int64_t t = esp_timer_get_time();
    s_next_poll_us = t + CONFIG_ANCHORBOX_ALERTS_POLL_S * 1000000LL;
    taskENTER_CRITICAL(&s_mux);
    s_v->polled = true;
    s_http = st;
    if (ok) {
        s_v->r = *s_tmp;
        s_v->link = true;
        s_fails = 0;
        s_last_ok_us = t;
    } else if (++s_fails >= 2) {
        s_v->link = false;
    }
    taskEXIT_CRITICAL(&s_mux);
    if (!ok && s_fails == 2) ESP_LOGW(TAG, "engine not answering /alerts (HTTP %d)", st);
    return s_next_poll_us;
}

void alerts_ctl_start(void)
{
    if (!WIFI) {
        ESP_LOGW(TAG, "no Wi-Fi on this box - no engine alerts");
        return;
    }
    s_tmp = heap_caps_malloc(sizeof(al_reply_t), MALLOC_CAP_SPIRAM);
    al_view_t *v = heap_caps_calloc(1, sizeof(al_view_t), MALLOC_CAP_SPIRAM);
    if (!s_tmp || !v) {
        ESP_LOGE(TAG, "no PSRAM for the alert buffers - engine alerts off");
        return;
    }
    v->wifi = true;
    v->token = TOKEN;
    v->answer_age_s = -1;
    taskENTER_CRITICAL(&s_mux);
    s_v = v;
    taskEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "engine /alerts every %d s via the engine link, ack token %s", CONFIG_ANCHORBOX_ALERTS_POLL_S,
             TOKEN ? "set" : "NOT SET");
}

bool alerts_ctl_ack(void)
{
    bool go = false;
    taskENTER_CRITICAL(&s_mux);
    if (s_v && s_v->link && s_v->token && !s_v->ack_pending && s_v->r.unacked > 0) {
        s_v->ack_pending = true;
        s_v->err[0] = 0;
        go = true;
    }
    taskEXIT_CRITICAL(&s_mux);
    if (go) engine_link_kick();
    return go;
}

bool alerts_ctl_reset(const char *id)
{
    bool go = false;
    taskENTER_CRITICAL(&s_mux);
    if (s_v && al_reset_ok(s_v, id) && !s_v->ack_pending && !s_v->reset_pending) {
        s_v->reset_pending = true;
        strlcpy(s_v->reset_id, id, sizeof(s_v->reset_id));
        s_v->err[0] = 0;
        go = true;
    }
    taskEXIT_CRITICAL(&s_mux);
    if (go) engine_link_kick();
    return go;
}

void alerts_ctl_get(al_view_t *v)
{
    int64_t t = esp_timer_get_time();
    taskENTER_CRITICAL(&s_mux);
    if (s_v) {
        if (s_v->err[0] && t > s_err_until_us) s_v->err[0] = 0;
        *v = *s_v;
    } else {
        *v = (al_view_t){ .wifi = WIFI, .answer_age_s = -1 };   /* not started (no Wi-Fi / no PSRAM): quiet */
    }
    v->answer_age_s = s_last_ok_us < 0 ? -1 : (int)((t - s_last_ok_us) / 1000000);
    taskEXIT_CRITICAL(&s_mux);
}

void alerts_ctl_status(char *buf, size_t n)
{
    static al_view_t *v;             /* only the status-page builder (boatclock task) calls this; PSRAM */
    if (!v) v = heap_caps_malloc(sizeof(al_view_t), MALLOC_CAP_SPIRAM);
    if (!v) {
        snprintf(buf, n, "alerts: no memory for the status line");
        return;
    }
    alerts_ctl_get(v);
    int http, ack_http;
    taskENTER_CRITICAL(&s_mux);
    http = s_http;
    ack_http = s_ack_http;
    taskEXIT_CRITICAL(&s_mux);
    char age[24] = "never";
    if (v->answer_age_s >= 0) snprintf(age, sizeof(age), "%d s ago", v->answer_age_s);
    if (!v->wifi) snprintf(buf, n, "alerts: off (no Wi-Fi on this box)");
    else if (!v->link)
        snprintf(buf, n, "alerts: %s - engine %s:%d/alerts, last answer %s (HTTP %d)",
                 v->polled ? "NO LINK" : "not polled yet", CONFIG_ANCHORBOX_PI_HOST, CONFIG_ANCHORBOX_ENGINE_PORT,
                 age, http);
    else
        snprintf(buf, n, "alerts seq %lu: %d active, %d not acked%s%s%s - link ok, last answer %s",
                 (unsigned long)v->r.seq, v->r.total, v->r.unacked, v->r.sounding ? ", HORNS" : "",
                 v->r.n ? ", top " : "", v->r.n ? v->r.a[0].id : "", age);
    size_t k = strlen(buf);
    if (k < n)
        snprintf(buf + k, n - k, " | ack token %s%s", v->token ? "set" : "MISSING",
                 ack_http >= 0 ? (ack_http == 200 ? ", last tap ACK ok" : ", last tap ACK FAILED") : "");
}
