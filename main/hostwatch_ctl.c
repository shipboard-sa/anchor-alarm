/**
 * Purpose: Runs the 'monitoring host down' watch on the box - see hostwatch_ctl.h (ANC-25).
 * Version: v2 (2026-10-21) - v2: the status page's router word carries its age ('router SILENT 3 h 05 min ago') -
 *   the check only runs while the Pi looks unhealthy, so a bare SILENT read like a live fault for hours (ANQ-32).
 *   v1 (2026-10-03)
 * Scope: PRODUCTION - anchor-box firmware (live on the box at the nav station, installed by signed OTA).
 * Architecture: anchor-box firmware, control layer. Task "hostwatch" (priority 2, below watch 4 / clock 3), 1 s
 *   loop: Cerbo check from n2k.c's last-frame times every second; Pi poll every PI_POLL_S:
 *     GET <health path> -> 200: healthy = every boolean "value" true AND newest "timestamp" fresh;
 *                          404 (SignalK answers, path not published - the previous stack until the cut-over):
 *     GET <fallback path> (position) -> 200 + fresh timestamp = "Pi + SignalK alive".
 *   Fresh = age by the box's own GPS UTC (n2k.c) when it has one, else "the timestamp changed within the limit".
 *   Router check (ICMP ping, 3 tries) only once the Pi has been unhealthy for PI_DOWN_AFTER_S. DO1 from
 *   hw_do1() every second (the expander refresh in boatclock.c rewrites it every 5 s too, ANC-16).
 *   Cerbo address table persisted in NVS "anchorbox"/"cerbo_tab" whenever it changes.
 *   The Pi poll is compiled in but OFF unless CONFIG_ANCHORBOX_PI_WATCH (off until the cut-over - ANC-26).
 *   TEST builds only (CONFIG_ANCHORBOX_HOSTWATCH_TEST): POST /test/hostwatch fakes the inputs for 10 min.
 * Invocation: imported by app_main.c (hostwatch_ctl_start after net_start), boatclock.c, ui_watch.c.
 */
#include "hostwatch_ctl.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "board.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hostwatch.h"
#include "lwip/ip_addr.h"
#include "n2k.h"
#include "net.h"
#include "nvs.h"
#include "ping/ping_sock.h"
#include "sdkconfig.h"

static const char *TAG = "hostwatch";

#define BODY_MAX 2048

#if CONFIG_ANCHORBOX_PI_WATCH
#define PI_WATCH_ON true
#else
#define PI_WATCH_ON false                 /* OFF until the cut-over (10-03, ANC-26) */
#endif

typedef enum { PI_NONE = 0, PI_OK, PI_FALSE, PI_STALE, PI_NO_PATH, PI_HTTP, PI_NO_ANSWER } pi_result_t;

static SemaphoreHandle_t s_mux;
static hw_pi_t s_pi;
static hw_cerbo_t s_cerbo;
static n2k_cerbo_t s_tab;                 /* last copy of n2k.c's Cerbo table, for the status page */
static pi_result_t s_pi_res;
static bool s_pi_fallback;                /* the last answer came from the fallback (position) path */
static int s_pi_http;                     /* last HTTP status, 0 = none */
static long long s_pi_age_ms = -1;        /* age of the last timestamp, -1 = unknown */
static int s_router = -1;                 /* last router check: -1 none, 0 silent, 1 answered */
static int64_t s_router_us = -1;          /* when that check ran (esp_timer us) - ANQ-32 */
static char s_last_event[96] = "-";
static char *s_body;

/* ------------------------------------------------------------------ TEST builds only */
#if CONFIG_ANCHORBOX_HOSTWATCH_TEST
#include "esp_http_server.h"
static int s_fake_pi, s_fake_router, s_fake_cerbo, s_fake_bus;   /* 0 = real; pi/router 1 = down, 2 = up */
static double s_fake_cerbo_t, s_fake_bus_t;
static int64_t s_fake_until_us;

static esp_err_t fake_post(httpd_req_t *req)
{
    char q[96] = "", v[12];
    httpd_req_get_url_query_str(req, q, sizeof(q));
    double now = esp_timer_get_time() / 1e6;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    if (httpd_query_key_value(q, "pi", v, sizeof(v)) == ESP_OK)
        s_fake_pi = !strcmp(v, "down") ? 1 : !strcmp(v, "ok") ? 2 : 0;
    if (httpd_query_key_value(q, "router", v, sizeof(v)) == ESP_OK)
        s_fake_router = !strcmp(v, "down") ? 1 : !strcmp(v, "up") ? 2 : 0;
    if (httpd_query_key_value(q, "cerbo", v, sizeof(v)) == ESP_OK) {
        s_fake_cerbo = !strcmp(v, "silent");
        s_fake_cerbo_t = now;
    }
    if (httpd_query_key_value(q, "bus", v, sizeof(v)) == ESP_OK) {
        s_fake_bus = !strcmp(v, "silent");
        s_fake_bus_t = now;
    }
    s_fake_until_us = esp_timer_get_time() + 600LL * 1000000LL;   /* fakes end on their own after 10 min */
    char msg[96];
    snprintf(msg, sizeof(msg), "fake pi %d router %d cerbo %d bus %d for 10 min\n", s_fake_pi, s_fake_router,
             s_fake_cerbo, s_fake_bus);
    xSemaphoreGive(s_mux);
    ESP_LOGW(TAG, "TEST %s", msg);
    return httpd_resp_sendstr(req, msg);
}

static void fake_expire(void)
{
    if (s_fake_until_us && esp_timer_get_time() > s_fake_until_us) {
        s_fake_pi = s_fake_router = s_fake_cerbo = s_fake_bus = 0;
        s_fake_until_us = 0;
        ESP_LOGW(TAG, "TEST fakes expired");
    }
}
#endif

/* ------------------------------------------------------------------ Pi */

/* GET http://PI_HOST:PI_PORT<path> into s_body. Returns the HTTP status, or 0 = no answer. */
static int http_get(const char *path)
{
    char url[192];
    snprintf(url, sizeof(url), "http://%s:%d%s", CONFIG_ANCHORBOX_PI_HOST, CONFIG_ANCHORBOX_PI_PORT, path);
    esp_http_client_config_t c = {
        .url = url,
        .timeout_ms = 3000,
        .buffer_size = 1024,
        .buffer_size_tx = 512,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t h = esp_http_client_init(&c);
    if (!h) return 0;
    int status = 0;
    s_body[0] = 0;
    if (esp_http_client_open(h, 0) == ESP_OK && esp_http_client_fetch_headers(h) >= 0) {
        status = esp_http_client_get_status_code(h);
        int got = 0, n;
        while (got < BODY_MAX - 1 && (n = esp_http_client_read(h, s_body + got, BODY_MAX - 1 - got)) > 0) got += n;
        s_body[got] = 0;
    }
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    return status;
}

typedef struct { long long ts; int64_t changed_us; bool have; } ts_track_t;
static ts_track_t s_trk_health, s_trk_fallback;

/* is a SignalK timestamp fresh? By the box's GPS UTC when it has one, else by "it changed recently". */
static bool fresh(long long ts_ms, int limit_s, ts_track_t *trk)
{
    int64_t now_us = esp_timer_get_time();
    if (!trk->have || trk->ts != ts_ms) {
        trk->have = true;
        trk->ts = ts_ms;
        trk->changed_us = now_us;
    }
    n2k_state_t st;
    n2k_get(&st);
    if (st.sync.valid) {
        long long age = bt_sync_now_ms(&st.sync, now_us / 1000) - ts_ms;
        s_pi_age_ms = age;
        return age < limit_s * 1000LL && age > -limit_s * 1000LL;   /* a clock far in the future is not fresh */
    }
    s_pi_age_ms = (now_us - trk->changed_us) / 1000;
    return now_us - trk->changed_us < limit_s * 1000000LL;
}

static pi_result_t poll_pi(int *http, bool *fallback)
{
    *fallback = false;
    hw_sk_reply_t r;
    int st = *http = http_get(CONFIG_ANCHORBOX_PI_HEALTH_PATH);
    if (st == 200) {
        hw_parse_signalk(s_body, &r);
        if (!r.have_bool || !r.all_true) return PI_FALSE;
        if (!r.have_ts || !fresh(r.ts_ms, CONFIG_ANCHORBOX_PI_HEALTH_FRESH_S, &s_trk_health)) return PI_STALE;
        return PI_OK;
    }
    if (st != 404) return st ? PI_HTTP : PI_NO_ANSWER;
    /* SignalK answers but does not publish the health path (yet): is it alive with a fresh position? */
    *fallback = true;
    st = *http = http_get(CONFIG_ANCHORBOX_PI_FALLBACK_PATH);
    if (st == 404) return PI_NO_PATH;
    if (st != 200) return st ? PI_HTTP : PI_NO_ANSWER;
    hw_parse_signalk(s_body, &r);
    if (!r.have_ts || !fresh(r.ts_ms, CONFIG_ANCHORBOX_PI_FALLBACK_FRESH_S, &s_trk_fallback)) return PI_STALE;
    return PI_OK;
}

/* ------------------------------------------------------------------ router */

static SemaphoreHandle_t s_ping_done;
static volatile int s_ping_ok;
static void ping_ok_cb(esp_ping_handle_t h, void *a) { s_ping_ok++; }
static void ping_end_cb(esp_ping_handle_t h, void *a) { xSemaphoreGive(s_ping_done); }

static int router_answers(void)
{
    ip_addr_t addr;
    if (!ipaddr_aton(CONFIG_ANCHORBOX_ROUTER_HOST, &addr)) return 0;
    esp_ping_config_t pc = ESP_PING_DEFAULT_CONFIG();
    pc.target_addr = addr;
    pc.count = 3;
    pc.interval_ms = 300;
    pc.timeout_ms = 1000;
    esp_ping_callbacks_t cb = { .on_ping_success = ping_ok_cb, .on_ping_end = ping_end_cb };
    esp_ping_handle_t h;
    s_ping_ok = 0;
    xSemaphoreTake(s_ping_done, 0);
    if (esp_ping_new_session(&pc, &cb, &h) != ESP_OK) return 0;
    esp_ping_start(h);
    bool ended = xSemaphoreTake(s_ping_done, pdMS_TO_TICKS(6000)) == pdTRUE;
    esp_ping_stop(h);
    esp_ping_delete_session(h);
    return ended && s_ping_ok > 0;
}

/* ------------------------------------------------------------------ Cerbo table in NVS */

typedef struct { uint8_t n; uint8_t sa[N2K_CERBO_MAX]; uint64_t name[N2K_CERBO_MAX]; } cerbo_blob_t;

static void tab_load(void)
{
    nvs_handle_t h;
    cerbo_blob_t b;
    size_t len = sizeof(b);
    if (nvs_open("anchorbox", NVS_READONLY, &h) != ESP_OK) return;
    if (nvs_get_blob(h, "cerbo_tab", &b, &len) == ESP_OK && len == sizeof(b) && b.n <= N2K_CERBO_MAX) {
        n2k_cerbo_seed(b.sa, b.name, b.n);
        ESP_LOGI(TAG, "Cerbo table restored: %u addresses", b.n);
    }
    nvs_close(h);
}

static void tab_save(const n2k_cerbo_t *c)
{
    cerbo_blob_t b = { .n = (uint8_t)c->n };
    memcpy(b.sa, c->sa, sizeof(b.sa));
    memcpy(b.name, c->name, sizeof(b.name));
    nvs_handle_t h;
    if (nvs_open("anchorbox", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "cerbo_tab", &b, sizeof(b));
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Cerbo table saved: %d addresses", c->n);
}

/* ------------------------------------------------------------------ loop */

static void note(hw_event_t e)
{
    if (e == HW_EV_NONE) return;
    static const char *TXT[] = { "", "Pi DOWN - DO1 on", "network down (router silent) - not the Pi, no lamp",
                                 "network back", "Pi healthy 30 s - DO1 released", "Cerbo SILENT - DO1 on",
                                 "N2K bus silent - not the Cerbo, no lamp", "N2K bus back",
                                 "Cerbo heard 30 s - DO1 released" };
    ESP_LOGW(TAG, "%s", TXT[e]);
    strlcpy(s_last_event, TXT[e], sizeof(s_last_event));
}

static void hostwatch_task(void *arg)
{
    int64_t next_poll_us = esp_timer_get_time() + 15LL * 1000000LL;   /* let Wi-Fi join first */
    for (;;) {
        int64_t now_us = esp_timer_get_time();
        double t = now_us / 1e6;

        if (now_us >= next_poll_us) {           /* the Pi: blocking HTTP / ping - only this task waits */
            next_poll_us = now_us + CONFIG_ANCHORBOX_PI_POLL_S * 1000000LL;
            bool enabled = PI_WATCH_ON;
#if CONFIG_ANCHORBOX_HOSTWATCH_TEST
            xSemaphoreTake(s_mux, portMAX_DELAY);
            fake_expire();
            enabled |= s_fake_pi != 0;           /* a faked Pi drives the logic even with the poll off */
            xSemaphoreGive(s_mux);
#endif
            if (!enabled) goto cerbo;
            int http = 0;
            bool fb = false;
            pi_result_t res = PI_NONE;
            if (PI_WATCH_ON) res = poll_pi(&http, &fb);
            bool ok = res == PI_OK;
            xSemaphoreTake(s_mux, portMAX_DELAY);
#if CONFIG_ANCHORBOX_HOSTWATCH_TEST
            if (s_fake_pi) ok = s_fake_pi == 2;
#endif
            bool want = !ok && hw_pi_wants_router(&s_pi, esp_timer_get_time() / 1e6);
            xSemaphoreGive(s_mux);
            int router = -1;
            if (want) {
                router = router_answers();
#if CONFIG_ANCHORBOX_HOSTWATCH_TEST
                if (s_fake_router) router = s_fake_router == 2;
#endif
            }
            xSemaphoreTake(s_mux, portMAX_DELAY);
            s_pi_res = res;
            s_pi_http = http;
            s_pi_fallback = fb;
            if (router >= 0) {
                s_router = router;
                s_router_us = esp_timer_get_time();
            }
            note(hw_pi_update(&s_pi, esp_timer_get_time() / 1e6, ok, router));
            xSemaphoreGive(s_mux);
        }

    cerbo:;
        n2k_cerbo_t c;                          /* the Cerbo: every second, no I/O */
        n2k_cerbo_get(&c, true);
        if (c.dirty) tab_save(&c);
        double lc = c.last_cerbo_us < 0 ? -1 : c.last_cerbo_us / 1e6;
        double lo = c.last_other_us < 0 ? -1 : c.last_other_us / 1e6;
        bool known = c.n > 0;
        xSemaphoreTake(s_mux, portMAX_DELAY);
#if CONFIG_ANCHORBOX_HOSTWATCH_TEST
        fake_expire();
        if (s_fake_cerbo) {
            known = true;
            lc = s_fake_cerbo_t;
        }
        if (s_fake_bus) lo = s_fake_bus_t;
#endif
        s_tab = c;
        note(hw_cerbo_update(&s_cerbo, t, known, lc, lo));
        bool do1 = hw_do1(&s_pi, &s_cerbo);
        xSemaphoreGive(s_mux);
        board_output(BOARD_DO1, do1);           /* panel AL-13 'Automation' - inform only */

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void hostwatch_ctl_start(void)
{
    hw_config_t cfg = {
        .pi_down_after_s = CONFIG_ANCHORBOX_PI_DOWN_AFTER_S,
        .cerbo_silent_s = CONFIG_ANCHORBOX_CERBO_SILENT_S,
        .release_s = CONFIG_ANCHORBOX_HOSTWATCH_RELEASE_S,
        .bus_quiet_s = 10,
    };
    hw_pi_init(&s_pi, &cfg);
    hw_cerbo_init(&s_cerbo, &cfg);
    s_mux = xSemaphoreCreateMutex();
    s_ping_done = xSemaphoreCreateBinary();
    s_body = heap_caps_malloc(BODY_MAX, MALLOC_CAP_SPIRAM);   /* internal RAM is tight */
    if (!s_body) s_body = malloc(BODY_MAX);
    tab_load();
#if CONFIG_ANCHORBOX_HOSTWATCH_TEST
    if (net_add_post("/test/hostwatch", fake_post) == ESP_OK) ESP_LOGW(TAG, "TEST BUILD: /test/hostwatch enabled");
#endif
    xTaskCreate(hostwatch_task, "hostwatch", 6144, NULL, 2, NULL);
    ESP_LOGI(TAG, "watching Pi %s:%d (every %d s) and the Cerbo on N2K (silent %d s) -> DO1",
             CONFIG_ANCHORBOX_PI_HOST, CONFIG_ANCHORBOX_PI_PORT, CONFIG_ANCHORBOX_PI_POLL_S,
             CONFIG_ANCHORBOX_CERBO_SILENT_S);
}

void hostwatch_ctl_notice(char *buf, size_t n)
{
    buf[0] = 0;
    if (!s_mux) return;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    const char *a = hw_pi_text(&s_pi), *b = hw_cerbo_text(&s_cerbo);
    if (a && b) snprintf(buf, n, "%s\n%s", a, b);
    else if (a || b) snprintf(buf, n, "%s", a ? a : b);
    xSemaphoreGive(s_mux);
}

/* append to buf, never past n (snprintf's return can exceed what fitted) */
static void app(char *buf, size_t n, size_t *k, const char *fmt, ...)
{
    if (*k >= n) return;
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(buf + *k, n - *k, fmt, ap);
    va_end(ap);
    if (w > 0) *k = *k + (size_t)w < n ? *k + (size_t)w : n;
}

void hostwatch_ctl_status(char *buf, size_t n)
{
    static const char *RES[] = { "not polled yet", "healthy", "health value false", "stale timestamp",
                                 "SignalK answers, neither path published", "HTTP error", "no answer" };
    buf[0] = 0;
    if (!s_mux) return;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    size_t k = 0;
    app(buf, n, &k, "monitor DO1 %s | last: %s\n", hw_do1(&s_pi, &s_cerbo) ? "ON" : "off", s_last_event);
    if (PI_WATCH_ON || s_pi.state != HW_UNKNOWN)
        app(buf, n, &k, "pi %s - %s", hw_state_name(s_pi.state), RES[s_pi_res]);
    else
        app(buf, n, &k, "pi watch OFF until the cut-over (ANC-26)");
    if (s_pi_http) {
        app(buf, n, &k, " (HTTP %d, %s path", s_pi_http, s_pi_fallback ? "fallback position" : "health");
        if (s_pi_age_ms >= 0) app(buf, n, &k, ", %lld s old", s_pi_age_ms / 1000);
        app(buf, n, &k, ")");
    }
    if (s_router >= 0) {                  /* ANQ-32: the LAST check, which only runs while the Pi looks unhealthy */
        int ago = s_router_us < 0 ? 0 : (int)((esp_timer_get_time() - s_router_us) / 1000000);
        if (ago < 60) app(buf, n, &k, ", router %s %d s ago", s_router ? "answered" : "SILENT", ago);
        else if (ago < 3600) app(buf, n, &k, ", router %s %d min ago", s_router ? "answered" : "SILENT", ago / 60);
        else app(buf, n, &k, ", router %s %d h %02d min ago", s_router ? "answered" : "SILENT", ago / 3600,
                 (ago / 60) % 60);
    }
    app(buf, n, &k, "\ncerbo %s - ", hw_state_name(s_cerbo.state));
    if (s_tab.n == 0) {
        app(buf, n, &k, "no Victron address claim heard yet");
    } else {
        app(buf, n, &k, "%d addr", s_tab.n);
        for (int i = 0; i < s_tab.n; i++) app(buf, n, &k, " %u", s_tab.sa[i]);
        if (s_tab.last_cerbo_us >= 0)
            app(buf, n, &k, ", last frame %lld s ago (src %u)",
                (long long)((esp_timer_get_time() - s_tab.last_cerbo_us) / 1000000), s_tab.last_sa);
    }
#if CONFIG_ANCHORBOX_HOSTWATCH_TEST
    if (s_fake_until_us)
        app(buf, n, &k, "\nTEST FAKES ACTIVE: pi %d router %d cerbo %d bus %d", s_fake_pi, s_fake_router, s_fake_cerbo,
            s_fake_bus);
#endif
    xSemaphoreGive(s_mux);
}
