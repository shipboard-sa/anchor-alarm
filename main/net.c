/**
 * Purpose: Wi-Fi + signed OTA updates for the anchor box (see net.h, SPEC 8).
 * Version: v5 (2026-10-06) - v5: HTTP server purges its least-recently-used connection when full, and an OTA upload
 *   that stalls 30 s is aborted instead of holding the (single-threaded) server for ever (ANQ-29, ANC-30). v4: net_add_post(), status text up to 1.5 KB (monitor lines, ANC-25). v3: TEST-ONLY /test/output behind CONFIG_ANCHORBOX_REMOTE_OUTPUT_TEST (never in a boat build), bigger status buffers. v2: status page shows the app's own lines (net_set_status). v1: Wi-Fi (non-fatal), mDNS, signed OTA, httpd stack 12 KB
 * Scope: PRODUCTION - anchor-box firmware, live on the box at the nav station since 2026-10-02 (installed by signed OTA).
 * Architecture: anchor-box firmware, network layer. Wi-Fi STA with auto-reconnect; mDNS name anchor-box.local;
 *   HTTP on port 80: GET / = status text, POST /ota = raw signed app image streamed into the idle OTA slot.
 *   esp_ota_end() rejects an image whose signature does not match the running app's signing key, so only
 *   images built with keys/anchorbox_signing_key.pem install (the previous OTA or anything else is refused).
 *   Rollback: a new image boots PENDING_VERIFY and is kept only when app_main calls net_mark_healthy().
 * Invocation: imported by app_main.c; net_start(gate) once after board_init().
 */
#include "net.h"

#include <stdlib.h>
#include <string.h>
#include "board.h"
#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#define HOSTNAME "anchor-box"

static const char *TAG = "net";
static net_ota_gate_t s_gate;
static bool s_busy;
static char s_ip[16] = "-";
static char s_extra[1536];
static httpd_handle_t s_srv;
static portMUX_TYPE s_extra_mux = portMUX_INITIALIZER_UNLOCKED;

void net_set_status(const char *text)
{
    portENTER_CRITICAL(&s_extra_mux);
    strlcpy(s_extra, text, sizeof(s_extra));
    portEXIT_CRITICAL(&s_extra_mux);
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && (id == WIFI_EVENT_STA_START || id == WIFI_EVENT_STA_DISCONNECTED)) {
        if (id == WIFI_EVENT_STA_DISCONNECTED) {
            static int n;
            wifi_event_sta_disconnected_t *e = data;
            strcpy(s_ip, "-");
            if (n++ % 10 == 0) ESP_LOGW(TAG, "Wi-Fi not connected, reason %d (201 = network not found, "
                                        "15/204 = wrong password) - retrying", e->reason);
        }
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        ESP_LOGI(TAG, "Wi-Fi up: %s (" HOSTNAME ".local)", s_ip);
    }
}

static esp_err_t status_get(httpd_req_t *req)
{
    const esp_app_desc_t *d = esp_app_get_description();
    const esp_partition_t *run = esp_ota_get_running_partition();
    /* PSRAM per request: internal RAM is kept for the display + Wi-Fi */
    const size_t bn = 2048;
    char *buf = heap_caps_malloc(bn, MALLOC_CAP_SPIRAM), *extra = heap_caps_malloc(sizeof(s_extra), MALLOC_CAP_SPIRAM);
    if (!buf || !extra) {
        free(buf);
        free(extra);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    }
    portENTER_CRITICAL(&s_extra_mux);
    memcpy(extra, s_extra, sizeof(s_extra));
    portEXIT_CRITICAL(&s_extra_mux);
    snprintf(buf, bn, "anchor-box\nversion %s\nbuilt %s %s\nrunning %s\nip %s\nuptime %lld s\n"
             "internal RAM free %u KB (lowest %u KB)\nupdate %s\n%s",
             d->version, d->date, d->time, run ? run->label : "?", s_ip,
             (long long)(esp_timer_get_time() / 1000000),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
             s_gate && !s_gate() ? "BLOCKED (watch armed / alarm)" : "allowed", extra);
    httpd_resp_set_type(req, "text/plain");
    esp_err_t err = httpd_resp_sendstr(req, buf);
    free(buf);
    free(extra);
    return err;
}

static void restart_cb(void *arg) { esp_restart(); }

#if CONFIG_ANCHORBOX_REMOTE_OUTPUT_TEST
/* TEST ONLY (sdkconfig.outtest, owner's yes 10-02): pulse one output for <= 15 s, then force it OFF. */
static esp_timer_handle_t s_pulse_timer;
static void pulse_off_cb(void *arg)
{
    board_output(BOARD_DO0, false);
    board_output(BOARD_DO1, false);
    ESP_LOGW(TAG, "TEST pulse ended - outputs OFF");
}

static esp_err_t output_test_post(httpd_req_t *req)
{
    char q[64] = "", v[8];
    httpd_req_get_url_query_str(req, q, sizeof(q));
    int which = 0, secs = 10;
    if (httpd_query_key_value(q, "do", v, sizeof(v)) == ESP_OK) which = atoi(v);
    if (httpd_query_key_value(q, "s", v, sizeof(v)) == ESP_OK) secs = atoi(v);
    if (which < 0 || which > 1 || secs < 1 || secs > 15)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "do=0|1, s=1..15");
    if (!s_pulse_timer) {
        esp_timer_create_args_t a = { .callback = pulse_off_cb, .name = "pulse_off" };
        esp_timer_create(&a, &s_pulse_timer);
    }
    esp_timer_stop(s_pulse_timer);
    board_output(which ? BOARD_DO1 : BOARD_DO0, true);
    esp_timer_start_once(s_pulse_timer, (uint64_t)secs * 1000000ULL);
    ESP_LOGW(TAG, "TEST pulse DO%d ON for %d s", which, secs);
    char msg[48];
    snprintf(msg, sizeof(msg), "DO%d ON for %d s\n", which, secs);
    return httpd_resp_sendstr(req, msg);
}
#endif

static esp_err_t refuse(httpd_req_t *req, const char *msg)
{
    httpd_resp_set_status(req, "409 Conflict");
    return httpd_resp_sendstr(req, msg);
}

static esp_err_t ota_post(httpd_req_t *req)
{
    if (s_busy) return refuse(req, "update already running\n");
    if (s_gate && !s_gate()) return refuse(req, "watch armed or alarm showing\n");
    const esp_partition_t *dst = esp_ota_get_next_update_partition(NULL);
    if (!dst || req->content_len <= 0 || req->content_len > dst->size)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad image size");

    s_busy = true;
    board_output(BOARD_DO0, false);   /* outputs OFF throughout (SPEC 8.4) */
    board_output(BOARD_DO1, false);
    ESP_LOGI(TAG, "OTA: %d bytes -> %s", req->content_len, dst->label);

    esp_ota_handle_t h;
    esp_err_t err = esp_ota_begin(dst, OTA_WITH_SEQUENTIAL_WRITES, &h);
    static char chunk[4096];
    int left = req->content_len;
    int stalls = 0;   /* recv_wait_timeout = 10 s each: 3 in a row = the sender is gone (ANQ-29) */
    while (err == ESP_OK && left > 0) {
        int n = httpd_req_recv(req, chunk, left < (int)sizeof(chunk) ? left : (int)sizeof(chunk));
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++stalls < 3) continue;
            err = ESP_ERR_TIMEOUT;
            break;
        }
        stalls = 0;
        if (n <= 0) { err = ESP_FAIL; break; }
        err = esp_ota_write(h, chunk, n);
        left -= n;
    }
    if (err == ESP_OK) {
        err = esp_ota_end(h);             /* checks the image AND its signature */
    } else {
        esp_ota_abort(h);
    }
    if (err == ESP_OK) err = esp_ota_set_boot_partition(dst);
    s_busy = false;

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "OTA refused: %s", esp_err_to_name(err));
        char msg[96];
        snprintf(msg, sizeof(msg), "update refused: %s", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, msg);
    }
    ESP_LOGI(TAG, "OTA OK - restarting into %s", dst->label);
    httpd_resp_sendstr(req, "OK - signature verified, restarting\n");
    esp_timer_handle_t t;
    esp_timer_create_args_t a = { .callback = restart_cb, .name = "ota_restart" };
    esp_timer_create(&a, &t);
    esp_timer_start_once(t, 1000000);
    return ESP_OK;
}

esp_err_t net_start(net_ota_gate_t gate)
{
    s_gate = gate;
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;
    if (strlen(CONFIG_ANCHORBOX_WIFI_SSID) == 0) {
        ESP_LOGW(TAG, "no Wi-Fi network set (sdkconfig.secrets) - Wi-Fi off, updates by USB only");
        return ESP_OK;
    }
    if (err == ESP_OK) err = esp_netif_init();
    if (err == ESP_OK) err = esp_event_loop_create_default();
    if (err == ESP_OK && !esp_netif_create_default_wifi_sta()) err = ESP_FAIL;
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    if (err == ESP_OK) err = esp_wifi_init(&wcfg);
    if (err == ESP_OK) err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL);
    if (err == ESP_OK) err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL);
    wifi_config_t sta = {0};
    strncpy((char *)sta.sta.ssid, CONFIG_ANCHORBOX_WIFI_SSID, sizeof(sta.sta.ssid) - 1);
    strncpy((char *)sta.sta.password, CONFIG_ANCHORBOX_WIFI_PASSWORD, sizeof(sta.sta.password) - 1);
    if (err == ESP_OK) err = esp_wifi_set_storage(WIFI_STORAGE_RAM);   /* login lives in the image, not NVS */
    if (err == ESP_OK) err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_STA, &sta);
    if (err == ESP_OK) err = esp_wifi_start();
    if (err != ESP_OK) {   /* Wi-Fi must never take the clock / watch down: log and carry on without it */
        ESP_LOGE(TAG, "Wi-Fi start failed (%s) - running without Wi-Fi, updates by USB only", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "joining Wi-Fi '%s'", CONFIG_ANCHORBOX_WIFI_SSID);

    ESP_LOGI(TAG, "internal RAM free %u KB", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(HOSTNAME);
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    }

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.recv_wait_timeout = 10;
    hc.lru_purge_enable = true;   /* all 7 connections held (idle keep-alive) -> drop the oldest, never go dark (ANQ-29) */
    hc.stack_size = 12288;   /* RSA-3072 signature check runs on this task; 4 KB default overflowed (10-02) */
    httpd_handle_t srv;
    err = httpd_start(&srv, &hc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server failed (%s) - no OTA", esp_err_to_name(err));
        return err;
    }
    httpd_uri_t get = { .uri = "/", .method = HTTP_GET, .handler = status_get };
    httpd_uri_t post = { .uri = "/ota", .method = HTTP_POST, .handler = ota_post };
    httpd_register_uri_handler(srv, &get);
    httpd_register_uri_handler(srv, &post);
    s_srv = srv;
#if CONFIG_ANCHORBOX_REMOTE_OUTPUT_TEST
    httpd_uri_t tst = { .uri = "/test/output", .method = HTTP_POST, .handler = output_test_post };
    httpd_register_uri_handler(srv, &tst);
    ESP_LOGW(TAG, "TEST BUILD: /test/output enabled");
#endif
    return ESP_OK;
}

esp_err_t net_add_post(const char *uri, esp_err_t (*handler)(httpd_req_t *))
{
    if (!s_srv) return ESP_ERR_INVALID_STATE;
    httpd_uri_t u = { .uri = uri, .method = HTTP_POST, .handler = handler };
    return httpd_register_uri_handler(s_srv, &u);
}

void net_mark_healthy(void)
{
    esp_ota_img_states_t st;
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "new firmware marked good (rollback cancelled)");
    }
}
