/**
 * Purpose: NMEA 2000 listener for the anchor box - LISTEN-ONLY (see n2k.h): System Time 126992 + Position Rapid
 *   129025 decode, per-PGN / per-source frame counts for the bus check, the Cerbo's addresses from overheard
 *   Address Claims 60928 + last-frame times (ANC-25).
 * Version: v3 (2026-10-03) - v3: Cerbo address table (ANC-25). v2: feeds every 126992 into bt_sync_feed()
 * Scope: PRODUCTION - anchor-box firmware, live on the box at the nav station since 2026-10-02 (installed by signed OTA).
 * Architecture: anchor-box firmware, input layer. RX ISR callback copies frames into a small ring; n2k_task
 *   decodes them. Pattern after ESP-IDF's twai_listen_only example (Apache-2.0). 29-bit ID -> PGN per
 *   ISO 11783 / canboat; 126992 = SID, source (low nibble), days since 1970 (u16), time of day 0.0001 s (u32);
 *   129025 = lat, lon (i32, 1e-7 deg). 0xFFFF / 0xFFFFFFFF / 0x7FFFFFFF = not available. 60928 = the 64-bit
 *   NAME, little-endian; manufacturer code = bits 21-31 (ISO 11783-5 / canboat; Victron Energy = 358).
 *   The box never REQUESTS a claim (listen-only, ANC-03): it learns when another node asks or the Cerbo
 *   (re)starts, and keeps the table in NVS (hostwatch_ctl.c) across its own restarts.
 * Invocation: imported by app_main.c; n2k_start() once after board_init().
 */
#include "n2k.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define N2K_TX_GPIO 15
#define N2K_RX_GPIO 16
#define N2K_BITRATE 250000
#define RING        256
#define MAX_SEEN    48

static const char *TAG = "n2k";

typedef struct { uint32_t id; uint8_t len; uint8_t d[8]; } rx_t;

static twai_node_handle_t s_node;
static rx_t s_ring[RING];
static volatile uint32_t s_w, s_r, s_dropped;
static uint8_t s_isr_buf[8];
static TaskHandle_t s_task;
static SemaphoreHandle_t s_mux;
static n2k_state_t s_state;

static struct { uint32_t pgn; uint8_t sa; uint32_t n; } s_seen[MAX_SEEN];
static int s_nseen;
static uint32_t s_total;
static volatile uint32_t s_err[5];
static n2k_cerbo_t s_cerbo = { .last_cerbo_us = -1, .last_other_us = -1 };
static bool s_is_cerbo[256];

/* caller holds s_mux */
static void cerbo_rebuild(void)
{
    memset(s_is_cerbo, 0, sizeof(s_is_cerbo));
    for (int i = 0; i < s_cerbo.n; i++) s_is_cerbo[s_cerbo.sa[i]] = true;
}

/* caller holds s_mux. An Address Claim: a Victron NAME is added / moved; any other NAME at an address we held
 * takes that address away from the Cerbo. */
static void cerbo_claim(uint8_t sa, uint64_t name)
{
    if (sa >= 252) return;                                      /* 254 = cannot claim, 255 = global */
    bool victron = ((name >> 21) & 0x7FF) == CONFIG_ANCHORBOX_CERBO_MFR;
    bool changed = false;
    for (int i = 0; i < s_cerbo.n; i++) {
        if (s_cerbo.sa[i] == sa && s_cerbo.name[i] != name) {   /* someone else has this address now */
            s_cerbo.sa[i] = s_cerbo.sa[--s_cerbo.n];
            s_cerbo.name[i] = s_cerbo.name[s_cerbo.n];
            i--;
            changed = true;
        }
    }
    if (victron) {
        int i = 0;
        while (i < s_cerbo.n && s_cerbo.name[i] != name) i++;
        if (i < s_cerbo.n && s_cerbo.sa[i] != sa) {
            ESP_LOGW(TAG, "Cerbo device %016llx moved %u -> %u", (unsigned long long)name, s_cerbo.sa[i], sa);
            s_cerbo.sa[i] = sa;
            changed = true;
        } else if (i == s_cerbo.n && s_cerbo.n < N2K_CERBO_MAX) {
            ESP_LOGI(TAG, "Cerbo device %016llx at %u", (unsigned long long)name, sa);
            s_cerbo.sa[s_cerbo.n] = sa;
            s_cerbo.name[s_cerbo.n++] = name;
            changed = true;
        }
    }
    if (changed) {
        s_cerbo.dirty = true;
        cerbo_rebuild();
    }
}   /* arb_lost, bit, form, stuff, ack - from the on_error callback */

static bool err_cb(twai_node_handle_t h, const twai_error_event_data_t *e, void *ctx)
{
    if (e->err_flags.arb_lost) s_err[0]++;
    if (e->err_flags.bit_err) s_err[1]++;
    if (e->err_flags.form_err) s_err[2]++;
    if (e->err_flags.stuff_err) s_err[3]++;
    if (e->err_flags.ack_err) s_err[4]++;
    return false;
}

static bool rx_cb(twai_node_handle_t h, const twai_rx_done_event_data_t *e, void *ctx)
{
    twai_frame_t f = { .buffer = s_isr_buf, .buffer_len = sizeof(s_isr_buf) };
    if (twai_node_receive_from_isr(h, &f) != ESP_OK || !f.header.ide) return false;   /* N2K = 29-bit only */
    if (s_w - s_r >= RING) { s_dropped++; return false; }
    rx_t *r = &s_ring[s_w % RING];
    r->id = f.header.id;
    r->len = f.header.dlc > 8 ? 8 : f.header.dlc;
    memcpy(r->d, s_isr_buf, r->len);
    s_w++;
    BaseType_t woke = pdFALSE;
    vTaskNotifyGiveFromISR(s_task, &woke);
    return woke == pdTRUE;
}

static uint32_t pgn_of(uint32_t id)
{
    uint32_t dp = (id >> 24) & 1, pf = (id >> 16) & 0xFF, ps = (id >> 8) & 0xFF;
    return (dp << 16) | (pf << 8) | (pf >= 240 ? ps : 0);
}

static uint16_t u16le(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t u32le(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

static void count(uint32_t pgn, uint8_t sa)
{
    s_total++;
    for (int i = 0; i < s_nseen; i++)
        if (s_seen[i].pgn == pgn && s_seen[i].sa == sa) { s_seen[i].n++; return; }
    if (s_nseen < MAX_SEEN) s_seen[s_nseen++] = (typeof(s_seen[0])){ pgn, sa, 1 };
}

static void decode(const rx_t *r)
{
    uint32_t pgn = pgn_of(r->id);
    uint8_t sa = r->id & 0xFF;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    count(pgn, sa);
    int64_t now = esp_timer_get_time();
    if (pgn == 60928 && r->len >= 8) cerbo_claim(sa, (uint64_t)u32le(&r->d[0]) | ((uint64_t)u32le(&r->d[4]) << 32));
    if (s_is_cerbo[sa]) {
        s_cerbo.last_cerbo_us = now;
        s_cerbo.last_sa = sa;
    } else {
        s_cerbo.last_other_us = now;
    }
    if (pgn == 126992 && r->len >= 8) {
        uint16_t days = u16le(&r->d[2]);
        uint32_t tod = u32le(&r->d[4]);
        if (days != 0xFFFF && tod != 0xFFFFFFFFu && tod < 864000000u) {
            s_state.have_time = true;
            s_state.utc_ms = (int64_t)days * 86400000LL + tod / 10;
            s_state.utc_at_us = esp_timer_get_time();
            s_state.time_src = sa;
            s_state.time_kind = r->d[1] & 0x0F;
            bt_sync_feed(&s_state.sync, s_state.utc_ms, s_state.utc_at_us / 1000, s_state.time_kind);
        }
    } else if (pgn == 129025 && r->len >= 8) {
        int32_t la = (int32_t)u32le(&r->d[0]), lo = (int32_t)u32le(&r->d[4]);
        if (la != 0x7FFFFFFF && lo != 0x7FFFFFFF) {
            s_state.have_pos = true;
            s_state.lat = la * 1e-7;
            s_state.lon = lo * 1e-7;
            s_state.pos_at_us = esp_timer_get_time();
            s_state.pos_src = sa;
        }
    }
    xSemaphoreGive(s_mux);
}

static void n2k_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        while (s_r != s_w) {
            decode(&s_ring[s_r % RING]);
            s_r++;
        }
    }
}

esp_err_t n2k_start(void)
{
    s_mux = xSemaphoreCreateMutex();
    bt_sync_init(&s_state.sync);
    xTaskCreate(n2k_task, "n2k", 4096, NULL, 5, &s_task);
    twai_onchip_node_config_t cfg = {
        .io_cfg = { .tx = N2K_TX_GPIO, .rx = N2K_RX_GPIO, .quanta_clk_out = -1, .bus_off_indicator = -1 },
        .bit_timing = { .bitrate = N2K_BITRATE, .sp_permill = 875 },   /* 87.5 % sample point, usual for 250k CAN */
        .tx_queue_depth = 1,
        .flags.enable_listen_only = true,   /* never transmits, never ACKs - cannot disturb the bus */
    };
    esp_err_t err = twai_new_node_onchip(&cfg, &s_node);
    if (err != ESP_OK) return err;
    twai_event_callbacks_t cbs = { .on_rx_done = rx_cb, .on_error = err_cb };
    err = twai_node_register_event_callbacks(s_node, &cbs, NULL);
    if (err != ESP_OK) return err;
    err = twai_node_enable(s_node);
    ESP_LOGI(TAG, "listen-only on GPIO %d/%d at %d bit/s: %s", N2K_TX_GPIO, N2K_RX_GPIO, N2K_BITRATE,
             esp_err_to_name(err));
    return err;
}

void n2k_cerbo_get(n2k_cerbo_t *out, bool clear_dirty)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    *out = s_cerbo;
    if (clear_dirty) s_cerbo.dirty = false;
    xSemaphoreGive(s_mux);
}

void n2k_cerbo_seed(const uint8_t *sa, const uint64_t *name, int n)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    for (int i = 0; i < n && s_cerbo.n < N2K_CERBO_MAX; i++) {
        bool dup = false;
        for (int j = 0; j < s_cerbo.n; j++) dup |= s_cerbo.name[j] == name[i];
        if (dup || sa[i] >= 252) continue;
        s_cerbo.sa[s_cerbo.n] = sa[i];
        s_cerbo.name[s_cerbo.n++] = name[i];
    }
    cerbo_rebuild();
    xSemaphoreGive(s_mux);
}

void n2k_get(n2k_state_t *out)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    *out = s_state;
    xSemaphoreGive(s_mux);
}

/* days since 1970-01-01 -> y/m/d (H. Hinnant's civil_from_days, public domain) */
static void civil(int64_t z, int *y, int *m, int *d)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int)(yoe + era * 400) + (*m <= 2);
}

void n2k_log_summary(void)
{
    twai_node_status_t st = {0};
    twai_node_record_t rec = {0};
    twai_node_get_info(s_node, &st, &rec);
    xSemaphoreTake(s_mux, portMAX_DELAY);
    ESP_LOGI(TAG, "frames %lu (dropped %lu), bus state %d, rx err %u, bus errors %lu, %d PGN/source pairs",
             (unsigned long)s_total, (unsigned long)s_dropped, st.state, st.rx_error_count,
             (unsigned long)rec.bus_err_num, s_nseen);
    ESP_LOGI(TAG, "error types: arb_lost %lu bit %lu form %lu stuff %lu ack %lu", (unsigned long)s_err[0],
             (unsigned long)s_err[1], (unsigned long)s_err[2], (unsigned long)s_err[3], (unsigned long)s_err[4]);
    for (int i = 0; i < 5; i++) s_err[i] = 0;
    for (int i = 0; i < s_nseen; i++)
        ESP_LOGI(TAG, "  pgn %6lu  src %3u  n %lu", (unsigned long)s_seen[i].pgn, s_seen[i].sa,
                 (unsigned long)s_seen[i].n);
    s_nseen = 0;
    s_total = 0;
    n2k_state_t s = s_state;
    xSemaphoreGive(s_mux);
    if (s.have_time) {
        int64_t ms = s.utc_ms + (esp_timer_get_time() - s.utc_at_us) / 1000;
        int y, m, d;
        civil(ms / 86400000LL, &y, &m, &d);
        int64_t t = (ms % 86400000LL) / 1000;
        ESP_LOGI(TAG, "TIME %04d-%02d-%02d %02d:%02d:%02d UTC (126992 from src %u, kind %u)", y, m, d,
                 (int)(t / 3600), (int)(t / 60 % 60), (int)(t % 60), s.time_src, s.time_kind);
    } else {
        ESP_LOGI(TAG, "TIME none yet");
    }
    if (s.have_pos)
        ESP_LOGI(TAG, "POS %.6f, %.6f (129025 from src %u, %lld ms ago)", s.lat, s.lon, s.pos_src,
                 (long long)((esp_timer_get_time() - s.pos_at_us) / 1000));
    else
        ESP_LOGI(TAG, "POS none yet");
}
