/**
 * Purpose: The CLOCK loop - see boatclock.h.
 * Version: v10 (2026-10-21) - v10: watchkeeper line on the status page (wkeep_ctl.c, ANQ-27). v9 (2026-10-18) - v9: restock (EXPECTED?) line on the status page (restock_ctl.c, ANC-45). v8 (2026-10-15) - v8: boat-mode line on the status page (boatmode_ctl.c, ANC-44). v7 (2026-10-11) - v7: hot-water line on the status page (hotwater_ctl.c, ANC-43). v6 (2026-10-09) - v6: tank-fill line on the status page (fill_ctl.c, ANC-41). v5 (2026-10-07) - v5: engine alerts line on the status page (alerts_ctl.c, ANQ-10). v4 (2026-10-06) - v4: night-watch line on the status page (nightwatch_ctl.c, ANQ-30). v3: monitoring-host notice on the CLOCK face + monitor lines on the status page
 *   (hostwatch_ctl.c, ANC-25). v2: status page shows armed time, rode, GPS, distance, lock reason. v1: split out of app_main.c v3 (step 4); zone pin + anchor symbol
 * Scope: PRODUCTION - anchor-box firmware, live on the box at the nav station since 2026-10-02 (installed by signed OTA).
 * Architecture: anchor-box firmware. Task "clock" at 1 Hz. Pin requests from the UI are handed over through a
 *   spinlock-protected slot and applied by this task (the chooser is only touched here). NVS "anchorbox"/"zone_pin".
 * Invocation: imported by app_main.c / ui_watch.c.
 */
#include "boatclock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "board.h"
#include "boattime.h"
#include "display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "hostwatch_ctl.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "n2k.h"
#include "net.h"
#include "alerts_ctl.h"
#include "fill_ctl.h"
#include "hotwater_ctl.h"
#include "boatmode_ctl.h"
#include "restock_ctl.h"
#include "wkeep_ctl.h"
#include "nightwatch_ctl.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "ui_clock.h"
#include "watch_ctl.h"

#define POS_FRESH_US (300LL * 1000000LL)
#define STATUS_MAX 1536                    /* = net.c s_extra */   /* a position older than 5 min is not used for zone / night */

static const char *TAG = "clock";
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_pin_pending;
static char s_pin_req[6];          /* "" = AUTO */
static char s_label[48] = "--:--";

void boatclock_pin(const char *zid)
{
    portENTER_CRITICAL(&s_mux);
    strlcpy(s_pin_req, zid ? zid : "", sizeof(s_pin_req));
    s_pin_pending = true;
    portEXIT_CRITICAL(&s_mux);
}

void boatclock_label(char *buf, size_t n)
{
    portENTER_CRITICAL(&s_mux);
    strlcpy(buf, s_label, n);
    portEXIT_CRITICAL(&s_mux);
}

static void apply_pin(bt_chooser_t *ch, const char *zid, bool save)
{
    bt_zone_t z;
    if (zid[0] && bt_zone_by_id(zid, &z)) {
        bt_chooser_pin(ch, &z);
        ESP_LOGW(TAG, "zone PINNED to %s", z.zid);
    } else {
        bt_chooser_pin(ch, NULL);
        ESP_LOGI(TAG, "zone AUTO");
        zid = "";
    }
    if (!save) return;
    nvs_handle_t h;
    if (nvs_open("anchorbox", NVS_READWRITE, &h) == ESP_OK) {
        if (zid[0]) nvs_set_str(h, "zone_pin", zid);
        else nvs_erase_key(h, "zone_pin");
        nvs_commit(h);
        nvs_close(h);
    }
}

static ui_anchor_t symbol(aw_state_t s)
{
    switch (s) {
    case AW_ACQUIRING: return UI_ANCHOR_ACQUIRING;
    case AW_LOCKED: return UI_ANCHOR_LOCKED;
    case AW_CONSERVATIVE: return UI_ANCHOR_CONSERVATIVE;
    case AW_ALARM: return UI_ANCHOR_LOCKED;
    default: return UI_ANCHOR_OFF;
    }
}

static void clock_task(void *arg)
{
    bt_chooser_t chooser;
    bt_chooser_init(&chooser);
    char saved[6] = "";
    nvs_handle_t h;
    if (nvs_open("anchorbox", NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(saved);
        nvs_get_str(h, "zone_pin", saved, &len);
        nvs_close(h);
    }
    if (saved[0]) apply_pin(&chooser, saved, false);

    char *line = heap_caps_malloc(STATUS_MAX, MALLOC_CAP_SPIRAM);   /* status page text; internal RAM is tight */
    if (!line) line = malloc(STATUS_MAX);
    char notice[96], last_notice[96] = "";
    bt_face_t last = { -2, 0, false, false };
    ui_anchor_t last_sym = (ui_anchor_t)-1;
    int refresh = 0, n2k_log = 0, healthy = 0;
    for (;;) {
        char req[6];
        bool pending;
        portENTER_CRITICAL(&s_mux);
        pending = s_pin_pending;
        s_pin_pending = false;
        memcpy(req, s_pin_req, sizeof(req));
        portEXIT_CRITICAL(&s_mux);
        if (pending) {
            apply_pin(&chooser, req, true);
            last.hh = -2;                       /* force a redraw */
        }

        int64_t now_us = esp_timer_get_time();
        n2k_state_t st;
        n2k_get(&st);
        bool have_pos = st.have_pos && now_us - st.pos_at_us < POS_FRESH_US;
        bt_chooser_update(&chooser, now_us / 1000000, have_pos, st.lat, st.lon);
        bt_face_t f = bt_clock_face(&st.sync, &chooser, now_us / 1000, have_pos, st.lat, st.lon);
#if CONFIG_ANCHORBOX_DEMO_CLOCK
        /* BENCH ONLY: a made-up time from uptime, starting 12:34. Never in a boat build (SPEC 3.3). */
        int64_t m = now_us / 60000000LL + 12 * 60 + 34;
        f.hh = (m / 60) % 24;
        f.mm = m % 60;
#endif
        watch_view_t wv;
        watch_ctl_get(&wv);
        ui_anchor_t sym = symbol(wv.state);
        bool changed = f.hh != last.hh || f.mm != last.mm || f.manual != last.manual || f.night != last.night;
        if ((changed || sym != last_sym) && display_lock(100)) {
            ui_clock_set_time(f.hh, f.mm);
            ui_clock_set_manual_zone(f.manual);
            ui_clock_set_night(f.night);
            ui_clock_set_anchor(sym);
            display_unlock();
            last = f;
            last_sym = sym;
        }
        hostwatch_ctl_notice(notice, sizeof(notice));
        if (strcmp(notice, last_notice) != 0 && display_lock(100)) {
            ui_clock_set_notice(notice);
            display_unlock();
            strlcpy(last_notice, notice, sizeof(last_notice));
        }

        const bt_zone_t *z = bt_chooser_zone(&chooser);
        char lbl[48];
        if (f.hh >= 0)
            snprintf(lbl, sizeof(lbl), "%02d:%02d %s (%s)", f.hh, f.mm, z->zid, f.manual ? "PINNED" : "auto");
        else
            snprintf(lbl, sizeof(lbl), "--:-- (%s)", !st.sync.valid ? "no GPS time yet" : "no position yet");
        portENTER_CRITICAL(&s_mux);
        strlcpy(s_label, lbl, sizeof(s_label));
        portEXIT_CRITICAL(&s_mux);
        const size_t LN = STATUS_MAX;
        int n = snprintf(line, LN, "clock %s %s\nwatch %s", lbl, f.night ? "NIGHT" : "day",
                         aw_state_name(wv.state));
        if (wv.state != AW_OFF) {
            int el = (int)wv.elapsed_s;
            n += snprintf(line + n, LN - n, " armed %d:%02d rode %.0f ft gps %s", el / 60, el % 60,
                          wv.rode_ft, wv.have_pos ? "ok" : "NONE");
            if (wv.have_distance && wv.have_alarm_radius)
                n += snprintf(line + n, LN - n, " %.0f m of %.0f m", wv.distance_m, wv.alarm_radius_m);
            else if (wv.have_distance)
                n += snprintf(line + n, LN - n, " %.0f m from the provisional centre", wv.distance_m);
            n += snprintf(line + n, LN - n, "%s%s%s", wv.drag ? " DRAG?" : "", wv.muted ? " MUTED" : "",
                          wv.stale ? " GPS-STALE" : "");
            if ((wv.state == AW_ACQUIRING || wv.state == AW_CONSERVATIVE) && wv.reason[0])
                n += snprintf(line + n, LN - n, "\nnot locked: %s", wv.reason);
        }
        n += snprintf(line + n, LN - n, "\nlast %s\n", wv.n_log ? wv.log[0] : "-");
        if (n > 0 && (size_t)n < LN - 2) {      /* the watch text is a few hundred bytes; the monitor gets the rest */
            hostwatch_ctl_status(line + n, LN - n - 1);
            strlcat(line, "\n", LN);
            size_t k = strlen(line);
            if (k < LN - 2) {
                nightwatch_ctl_status(line + k, LN - k - 1);
                strlcat(line, "\n", LN);
            }
            k = strlen(line);
            if (k < LN - 2) {
                alerts_ctl_status(line + k, LN - k - 1);
                strlcat(line, "\n", LN);
            }
            k = strlen(line);
            if (k < LN - 2) {
                fill_ctl_status(line + k, LN - k - 1);
                strlcat(line, "\n", LN);
            }
            k = strlen(line);
            if (k < LN - 2) {
                hotwater_ctl_status(line + k, LN - k - 1);
                strlcat(line, "\n", LN);
            }
            k = strlen(line);
            if (k < LN - 2) {
                boatmode_ctl_status(line + k, LN - k - 1);
                strlcat(line, "\n", LN);
            }
            k = strlen(line);
            if (k < LN - 2) {
                restock_ctl_status(line + k, LN - k - 1);
                strlcat(line, "\n", LN);
            }
            k = strlen(line);
            if (k < LN - 2) {
                wkeep_ctl_status(line + k, LN - k - 1);
                strlcat(line, "\n", LN);
            }
        }
        net_set_status(line);

        if (++refresh >= 5) {
            refresh = 0;
            if (board_refresh() != ESP_OK) ESP_LOGW(TAG, "expander refresh failed");
        }
        if (healthy >= 0 && ++healthy >= 30) {   /* 30 s up with display + clock running = good image */
            healthy = -1;
            net_mark_healthy();
        }
        if (++n2k_log >= 60) {
            n2k_log = 0;
            n2k_log_summary();
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void boatclock_start(void)
{
    xTaskCreate(clock_task, "clock", 6144, NULL, 3, NULL);
}
