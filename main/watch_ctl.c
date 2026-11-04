/**
 * Purpose: Runs the anchor watch on the box - see watch_ctl.h.
 * Version: v3 (2026-10-02) - v3: RE-ARM + breadcrumb track (ANQ-16). v2: the circle survives a power cut (NVS 'snap',
 *   no age limit - 10-02)
 * Scope: PRODUCTION - anchor-box firmware, live on the box at the nav station since 2026-10-02 (installed by signed OTA).
 * Architecture: anchor-box firmware, control layer. Task "watch" at 1 Hz: newest 129025 fix from n2k.c ->
 *   aw_update(); aw_tick() for GPS-stale; events -> log + DO0 + screen actions (done AFTER the mutex is released).
 *   NVS namespace "anchorbox": armed (u8), rode (u16 ft) - written on ARM, erased on DISARM; snap (aw_snapshot_t
 *   blob) - written on every state change into LOCKED / CONSERVATIVE / ALARM, erased by DISARM / ARM.
 * Invocation: imported by app_main.c / ui_watch.c.
 */
#include "watch_ctl.h"

#include <stdlib.h>
#include <string.h>
#include "board.h"
#include "display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "n2k.h"
#include "nvs.h"
#include "screens.h"

static const char *TAG = "watch";
static aw_t *s_w;
static SemaphoreHandle_t s_mux;
static bool s_muted;
static int64_t s_last_fed_us;
static double s_rode_ft;                 /* the last ARM's pick, reused by RE-ARM */
static watch_crumb_t *s_track;           /* ring, WATCH_TRACK_N, PSRAM */
static int s_track_head, s_track_n;
static double s_track_lat0, s_track_lon0;
static int64_t s_track_last_us;

/* breadcrumbs: one point per 10 s, armed or not (the trail is there from the moment the boat anchors) */
static void track_add(int64_t at_us, double lat, double lon)
{
    if (!s_track || at_us - s_track_last_us < 10 * 1000000LL) return;
    s_track_last_us = at_us;
    if (s_track_n == 0) {
        s_track_lat0 = lat;
        s_track_lon0 = lon;
    }
    s_track[s_track_head] = (watch_crumb_t){ (float)(lat - s_track_lat0), (float)(lon - s_track_lon0) };
    s_track_head = (s_track_head + 1) % WATCH_TRACK_N;
    if (s_track_n < WATCH_TRACK_N) s_track_n++;
}
static char s_log[WATCH_LOG_N][96];
static int s_nlog;

typedef enum { ACT_NONE = 0, ACT_ALARM_ON = 1, ACT_ALARM_OFF = 2, ACT_WAKE = 4 } act_t;

static double now_s(void) { return esp_timer_get_time() / 1e6; }

static void log_msg(const char *m)
{
    memmove(s_log[1], s_log[0], sizeof(s_log[0]) * (WATCH_LOG_N - 1));
    strncpy(s_log[0], m, sizeof(s_log[0]) - 1);
    s_log[0][sizeof(s_log[0]) - 1] = 0;
    if (s_nlog < WATCH_LOG_N) s_nlog++;
}

/* caller holds s_mux; returns screen actions to run after release */
/* The circle survives a power cut (10-02, no age limit): saved on every change into LOCKED / CONSERVATIVE /
 * ALARM, erased by DISARM or a fresh ARM. Written only on a state change - a handful of flash writes per anchorage. */
static void save_snap(void)
{
    nvs_handle_t h;
    if (nvs_open("anchorbox", NVS_READWRITE, &h) != ESP_OK) return;
    aw_snapshot_t s;
    if (aw_snapshot(s_w, &s)) nvs_set_blob(h, "snap", &s, sizeof(s));
    else nvs_erase_key(h, "snap");
    nvs_commit(h);
    nvs_close(h);
}

/* caller holds s_mux */
static int handle(const aw_event_t *ev, int n)
{
    int act = ACT_NONE;
    bool state_changed = false;
    for (int i = 0; i < n; i++) {
        const aw_event_t *e = &ev[i];
        ESP_LOGW(TAG, "%s: %s", aw_state_name(e->state), e->msg);
        log_msg(e->msg);
        if (e->kind == AW_EV_STATE) state_changed = true;
        if (e->kind == AW_EV_STATE && e->state == AW_ALARM) act |= ACT_ALARM_ON;
        if (e->kind == AW_EV_STATE && e->state == AW_OFF) act |= ACT_ALARM_OFF;
        if (e->kind == AW_EV_DRAG || e->kind == AW_EV_WARN) act |= ACT_WAKE;
    }
    if (state_changed) save_snap();
    return act;
}

static void apply_outputs(void)
{
    /* DO0 = panel Anchor Drag (AL-12): asserted in ALARM (muted or not), released only by DISARM */
    board_output(BOARD_DO0, s_w->state == AW_ALARM);
}

static void run_actions(int act)
{
    if (!act || !display_lock(500)) return;
    if (act & ACT_ALARM_ON) screens_set_alarm(true);
    if (act & ACT_ALARM_OFF) screens_set_alarm(false);
    if ((act & ACT_WAKE) && !screens_alarm()) screens_show_watch();
    display_unlock();
}

static void save_armed(bool armed, double rode_ft)
{
    nvs_handle_t h;
    if (nvs_open("anchorbox", NVS_READWRITE, &h) != ESP_OK) return;
    if (armed) {
        nvs_set_u8(h, "armed", 1);
        nvs_set_u16(h, "rode", (uint16_t)(rode_ft > 0 ? rode_ft : 0));
    } else {
        nvs_erase_key(h, "armed");
        nvs_erase_key(h, "rode");
    }
    nvs_commit(h);
    nvs_close(h);
}

static void watch_task(void *arg)
{
    aw_event_t ev[AW_MAX_EVENTS];
    for (;;) {
        n2k_state_t st;
        n2k_get(&st);
        int act = ACT_NONE;
        xSemaphoreTake(s_mux, portMAX_DELAY);
        /* only fixes newer than the last one fed, and never one from before the ARM */
        if (st.have_pos && st.pos_at_us != s_last_fed_us && s_w->state != AW_OFF &&
            st.pos_at_us / 1e6 > s_w->armed_at) {
            s_last_fed_us = st.pos_at_us;
            act |= handle(ev, aw_update(s_w, st.pos_at_us / 1e6, st.lat, st.lon, ev));
        }
        act |= handle(ev, aw_tick(s_w, now_s(), ev));
        if (st.have_pos) track_add(st.pos_at_us, st.lat, st.lon);
        apply_outputs();
        xSemaphoreGive(s_mux);
        run_actions(act);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void watch_ctl_start(void)
{
    s_mux = xSemaphoreCreateMutex();
    s_track = heap_caps_malloc(sizeof(watch_crumb_t) * WATCH_TRACK_N, MALLOC_CAP_SPIRAM);
    s_w = heap_caps_malloc(sizeof(aw_t), MALLOC_CAP_SPIRAM);
    if (!s_w) s_w = malloc(sizeof(aw_t));
    aw_init(s_w, NULL);
    ESP_LOGI(TAG, "anchor watch ready (%u bytes in PSRAM)", (unsigned)sizeof(aw_t));

    nvs_handle_t h;
    uint8_t armed = 0;
    uint16_t rode = 0;
    if (nvs_open("anchorbox", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "armed", &armed);
        nvs_get_u16(h, "rode", &rode);
        nvs_close(h);
    }
    aw_snapshot_t snap;
    size_t len = sizeof(snap);
    bool have_snap = false;
    if (nvs_open("anchorbox", NVS_READONLY, &h) == ESP_OK) {
        have_snap = nvs_get_blob(h, "snap", &snap, &len) == ESP_OK && len == sizeof(snap) &&
                    snap.version == AW_SNAP_VERSION;
        nvs_close(h);
    }
    aw_event_t ev[AW_MAX_EVENTS];
    int act = ACT_NONE;
    if (armed && have_snap) {
        /* carry on with the same circle - never re-fit after an outage (a slow drift would hide) */
        act = handle(ev, aw_restore(s_w, now_s(), &snap, ev));
        ESP_LOGW(TAG, "circle RESTORED after power-up (%s)", aw_state_name(s_w->state));
    } else if (armed) {
        handle(ev, aw_arm(s_w, now_s(), rode, ev));
        log_msg("RE-ARMED after power-up (was armed, no circle yet) - observing the swing again");
        ESP_LOGW(TAG, "re-armed after power-up, rode %u ft", rode);
    }
    apply_outputs();
    run_actions(act);            /* a restored ALARM takes the screen at once */
    xTaskCreate(watch_task, "watch", 6144, NULL, 4, NULL);
}

void watch_ctl_arm(double rode_ft)
{
    aw_event_t ev[AW_MAX_EVENTS];
    xSemaphoreTake(s_mux, portMAX_DELAY);
    handle(ev, aw_arm(s_w, now_s(), rode_ft, ev));
    s_rode_ft = rode_ft;
    s_muted = false;
    s_last_fed_us = 0;
    apply_outputs();
    xSemaphoreGive(s_mux);
    save_armed(true, rode_ft);
    /* a re-ARM clears a latched ALARM: the screen hold must go too (called from the UI, which holds the lock) */
    screens_set_alarm(false);
}

void watch_ctl_disarm(void)
{
    aw_event_t ev[AW_MAX_EVENTS];
    xSemaphoreTake(s_mux, portMAX_DELAY);
    handle(ev, aw_disarm(s_w, now_s(), ev));
    s_muted = false;
    apply_outputs();
    xSemaphoreGive(s_mux);
    save_armed(false, 0);
    screens_set_alarm(false);    /* UI caller holds the display lock */
}

void watch_ctl_rearm(void)
{
    double rode;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    rode = s_w->have_reach ? s_w->rode_ft : s_rode_ft;
    log_msg("RE-ARMED by crew - old circle dropped, observing the new swing");
    xSemaphoreGive(s_mux);
    watch_ctl_arm(rode);
}

int watch_ctl_track(watch_crumb_t *out, int max, double *lat0, double *lon0)
{
    if (!s_mux || !s_track) return 0;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    int n = s_track_n < max ? s_track_n : max;
    int start = (s_track_head - n + WATCH_TRACK_N) % WATCH_TRACK_N;
    for (int i = 0; i < n; i++) out[i] = s_track[(start + i) % WATCH_TRACK_N];
    *lat0 = s_track_lat0;
    *lon0 = s_track_lon0;
    xSemaphoreGive(s_mux);
    return n;
}

void watch_ctl_mute(void)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    if (s_w->state == AW_ALARM) {
        s_muted = true;
        log_msg("MUTED - panel output stays on until DISARM");
    }
    xSemaphoreGive(s_mux);
}

bool watch_ctl_armed(void)
{
    if (!s_mux) return false;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    bool a = s_w->state != AW_OFF;
    xSemaphoreGive(s_mux);
    return a;
}

void watch_ctl_get(watch_view_t *v)
{
    memset(v, 0, sizeof(*v));
    if (!s_mux) return;          /* the UI is built before watch_ctl_start(): show OFF */
    n2k_state_t st;
    n2k_get(&st);
    v->have_pos = st.have_pos && esp_timer_get_time() - st.pos_at_us < 30 * 1000000LL;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    v->state = s_w->state;
    v->muted = s_muted;
    v->drag = s_w->drag_suspected;
    v->stale = s_w->stale;
    v->elapsed_s = s_w->state != AW_OFF ? now_s() - s_w->armed_at : 0;
    v->rode_ft = s_w->have_reach ? s_w->rode_ft : 0;
    v->have_distance = s_w->have_distance;
    v->distance_m = s_w->distance;
    v->have_alarm_radius = s_w->have_alarm_radius;
    v->alarm_radius_m = s_w->alarm_radius;
    strncpy(v->reason, s_w->no_lock_reason, sizeof(v->reason) - 1);
    memcpy(v->log, s_log, sizeof(s_log));
    v->n_log = s_nlog;
    double r;
    v->have_centre = aw_centre_latlon(s_w, &v->centre_lat, &v->centre_lon, &r);
    v->have_fix = st.have_pos;           /* straight from the GPS - the plot works disarmed too */
    v->lat = st.lat;
    v->lon = st.lon;
    xSemaphoreGive(s_mux);
}
