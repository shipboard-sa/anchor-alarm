/**
 * Purpose: Entry point of the anchor box firmware (clean-room build, ANC-01): brings up the board, display, N2K
 *   listener, Wi-Fi / signed OTA, the anchor watch and the clock loop.
 * Version: v13 (2026-10-21) - v13: watchkeeper check-in started (wkeep_ctl.c, ANQ-27, Ship's Automation WF-614 / D-161). v12 (2026-10-18) - v12: cold-box EXPECTED? question started (restock_ctl.c, ANC-45, Ship's Automation WF-502 / D-128). v11 (2026-10-15) - v11: DOCK / LEAVE taps started (boatmode_ctl.c, ANC-44, Ship's Automation D-151). v10 (2026-10-11) - v10: HOT WATER tap started (hotwater_ctl.c, ANC-43, Ship's Automation D-135). v9 (2026-10-09) - v9: one engine link (engine_link.c) serves night watch + alerts + fill (ANQ-33). v8 (2026-10-09) - v8: tank-fill taps started (fill_ctl.c, ANC-41, D-129). v7 (2026-10-07) - v7: engine alerts started (alerts_ctl.c, ANQ-10, view-only). v6 (2026-10-06) - v6: night-watch remote started (nightwatch_ctl.c, ANQ-30). v5: monitoring-host watch started (hostwatch_ctl.c, DO1 -> AL-13, ANC-25). v4: anchor watch live (watch_ctl.c, step 4); clock loop moved to boatclock.c; OTA
 *   refused while armed. v3: real boat time (step 3). v2: N2K listener + Wi-Fi OTA. v1: step 2 bring-up.
 * Scope: PRODUCTION - anchor-box firmware, live on the box at the nav station since 2026-10-02 (installed by signed OTA).
 * Architecture: anchor-box firmware top. board.c (hardware) -> display.c (LVGL) -> screens.c (views);
 *   n2k.c (time + position in) -> boattime.c / boatclock.c (CLOCK) and anchorwatch.c / watch_ctl.c (WATCH, DO0);
 *   net.c (Wi-Fi, status page, OTA); hostwatch_ctl.c (Pi + Cerbo watch -> DO1); nightwatch_ctl.c
 *   (night-watch remote, rules engine :8200); alerts_ctl.c (engine alerts + ack); fill_ctl.c (tank-fill taps). Spec: code/anchor-box/SPEC.md. Reference model: code/anchor-box/reference.
 * Invocation: ESP-IDF app_main(); build with idf.py, install with tools/ota_push.py (see CHECKPOINT.md).
 */
#include "board.h"
#include "boatclock.h"
#include "display.h"
#include "esp_log.h"
#include "hostwatch_ctl.h"
#include "n2k.h"
#include "net.h"
#include "alerts_ctl.h"
#include "boatmode_ctl.h"
#include "engine_link.h"
#include "fill_ctl.h"
#include "hotwater_ctl.h"
#include "nightwatch_ctl.h"
#include "restock_ctl.h"
#include "wkeep_ctl.h"
#include "screens.h"
#include "watch_ctl.h"

static const char *TAG = "anchorbox";

/* SPEC 8.4: no update while the watch is armed or an ALARM shows. */
static bool ota_gate(void)
{
    return !screens_alarm() && !watch_ctl_armed();
}

void app_main(void)
{
    ESP_LOGI(TAG, "anchor box firmware v12 (anchor watch + monitoring-host watch + night-watch remote + engine alerts + fill taps + hot water + dock / leave + cold-box expected?) - outputs OFF at power-up");
    ESP_ERROR_CHECK(board_init());
    display_start();          /* first: its draw buffers need internal DMA RAM, which Wi-Fi would take */
    display_lock(0);
    screens_init();
    display_unlock();
    if (n2k_start() != ESP_OK) ESP_LOGE(TAG, "N2K listener failed to start");
    if (net_start(ota_gate) != ESP_OK) ESP_LOGE(TAG, "Wi-Fi / OTA failed to start");   /* also opens NVS */
    watch_ctl_start();
    hostwatch_ctl_start();    /* after net_start: needs NVS + the HTTP server; its own low-priority task */
    nightwatch_ctl_start();   /* the three rules-engine features: state + buffers, no tasks of their own ... */
    alerts_ctl_start();
    fill_ctl_start();
    hotwater_ctl_start();
    boatmode_ctl_start();
    restock_ctl_start();
    wkeep_ctl_start();
    engine_link_start();      /* ... ONE lowest-priority task + one keep-alive client serves them all (ANQ-33) */
    boatclock_start();
}
