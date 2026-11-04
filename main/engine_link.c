/**
 * Purpose: The box's one link to the rules engine - see engine_link.h (ANQ-33 memory clean-up).
 * Version: v5 (2026-10-21) - v5: wkeep_ctl_service() in the loop (watchkeeper check-in, ANQ-27). v4 (2026-10-18) - v4: restock_ctl_service() in the loop (EXPECTED? question, ANC-45). v3 (2026-10-15) - v3: boatmode_ctl_service() in the loop (DOCK / LEAVE taps, ANC-44). v2 (2026-10-11) - v2: hotwater_ctl_service() in the loop (HOT WATER tap, ANC-43). v1 (2026-10-09)
 * Scope: PRODUCTION - anchor-box firmware (live on the box at the nav station, installed by signed OTA).
 * Architecture: anchor-box firmware, network layer. One esp_http_client handle for the box's lifetime: per request
 *   only URL / method / timeout / headers change; the socket is kept while the engine (uvicorn) keeps it alive, and
 *   any error closes it so the next request starts clean. Internal RAM per request: none new (handle + buffers made
 *   once); the TCP connection is reused instead of opened and left in TIME_WAIT every 2-10 s by three tasks.
 *   Nothing here touches the anchor watch, DO0 or DO1. The token is never logged nor shown.
 * Invocation: imported by app_main.c (engine_link_start) and nightwatch_ctl.c / alerts_ctl.c / fill_ctl.c.
 */
#include "engine_link.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "engine";

#define WIFI (sizeof(CONFIG_ANCHORBOX_WIFI_SSID) > 1)
#define START_DELAY_US (12LL * 1000000LL)       /* let Wi-Fi join first */
#define MAX_SLEEP_US (10LL * 1000000LL)

static TaskHandle_t s_task;
static esp_http_client_handle_t s_h;
static char *s_body;                            /* ENGINE_BODY_MAX, PSRAM */
static bool s_connected;                        /* a kept-alive socket may be open */
static char s_auth[8 + sizeof(CONFIG_ANCHORBOX_NIGHT_WATCH_TOKEN)];

static int once(const char *body, int len)
{
    int status = 0;
    s_body[0] = 0;
    if (esp_http_client_open(s_h, len) == ESP_OK && (len == 0 || esp_http_client_write(s_h, body, len) == len) &&
        esp_http_client_fetch_headers(s_h) >= 0) {
        status = esp_http_client_get_status_code(s_h);
        int got = 0, n;
        while (got < ENGINE_BODY_MAX - 1 && (n = esp_http_client_read(s_h, s_body + got, ENGINE_BODY_MAX - 1 - got)) > 0)
            got += n;
        s_body[got] = 0;
        int rest = 0;
        s_connected = esp_http_client_flush_response(s_h, &rest) == ESP_OK &&   /* drained: the socket is reusable */
                      esp_http_client_is_persistent_connection(s_h);
    } else {
        s_connected = false;
    }
    if (!s_connected) esp_http_client_close(s_h);   /* clean state for the next request */
    return status;
}

int engine_link_request(const char *path, const char *body, bool auth, int timeout_ms, const char **reply)
{
    *reply = "";
    if (!s_h || !s_body) return 0;
    char url[112];
    snprintf(url, sizeof(url), "http://%s:%d%s", CONFIG_ANCHORBOX_PI_HOST, CONFIG_ANCHORBOX_ENGINE_PORT, path);
    esp_http_client_set_url(s_h, url);
    esp_http_client_set_method(s_h, body ? HTTP_METHOD_POST : HTTP_METHOD_GET);
    esp_http_client_set_timeout_ms(s_h, timeout_ms);
    if (body) esp_http_client_set_header(s_h, "Content-Type", "application/json");
    else esp_http_client_delete_header(s_h, "Content-Type");
    if (auth) esp_http_client_set_header(s_h, "Authorization", s_auth);   /* copied by the client */
    else esp_http_client_delete_header(s_h, "Authorization");
    int len = body ? (int)strlen(body) : 0;
    bool reused = s_connected;
    int status = once(body, len);
    if (status == 0 && reused) status = once(body, len);   /* the engine dropped the idle socket: once more, fresh */
    *reply = s_body;
    return status;
}

static void engine_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(START_DELAY_US / 1000));
    for (;;) {
        int64_t now = esp_timer_get_time();
        int64_t next = now + MAX_SLEEP_US, t;
        /* in turn, each at most one request; a tap anywhere is served on the next pass at once */
        if ((t = nightwatch_ctl_service(now)) < next) next = t;
        if ((t = alerts_ctl_service(esp_timer_get_time())) < next) next = t;
        if ((t = fill_ctl_service(esp_timer_get_time())) < next) next = t;
        if ((t = hotwater_ctl_service(esp_timer_get_time())) < next) next = t;
        if ((t = boatmode_ctl_service(esp_timer_get_time())) < next) next = t;
        if ((t = restock_ctl_service(esp_timer_get_time())) < next) next = t;
        if ((t = wkeep_ctl_service(esp_timer_get_time())) < next) next = t;
        now = esp_timer_get_time();
        if (next > now) ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS((next - now) / 1000 + 1));   /* a tap wakes us */
    }
}

void engine_link_start(void)
{
    if (!WIFI) {
        ESP_LOGW(TAG, "no Wi-Fi on this box - no rules-engine link");
        return;
    }
    snprintf(s_auth, sizeof(s_auth), "Bearer %s", CONFIG_ANCHORBOX_NIGHT_WATCH_TOKEN);
    s_body = heap_caps_malloc(ENGINE_BODY_MAX, MALLOC_CAP_SPIRAM);   /* internal RAM is tight */
    char url[64];
    snprintf(url, sizeof(url), "http://%s:%d/", CONFIG_ANCHORBOX_PI_HOST, CONFIG_ANCHORBOX_ENGINE_PORT);
    esp_http_client_config_t c = {
        .url = url,
        .timeout_ms = 2000,
        .buffer_size = 512,
        .buffer_size_tx = 1024,
        .disable_auto_redirect = true,
    };
    s_h = esp_http_client_init(&c);
    if (!s_body || !s_h) {
        ESP_LOGE(TAG, "no memory for the engine link - night watch / alerts / fill / hot water show 'no link'");
        return;
    }
    /* stack in PSRAM (no flash writes on this task) */
    if (xTaskCreatePinnedToCoreWithCaps(engine_task, "engine", 6144, NULL, 1, &s_task, tskNO_AFFINITY,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "could not start the engine task");
        s_task = NULL;
        return;
    }
    ESP_LOGI(TAG, "one link to http://%s:%d (night watch, alerts, fill, hot water, mode, restock, watch), box token %s", CONFIG_ANCHORBOX_PI_HOST,
             CONFIG_ANCHORBOX_ENGINE_PORT, sizeof(CONFIG_ANCHORBOX_NIGHT_WATCH_TOKEN) > 1 ? "set" : "NOT SET");
}

void engine_link_kick(void)
{
    if (s_task) xTaskNotifyGive(s_task);
}
