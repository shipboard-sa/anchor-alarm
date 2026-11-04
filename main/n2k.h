/**
 * Purpose: NMEA 2000 listener for the anchor box - LISTEN-ONLY on the boat's N2K bus (never transmits, never
 *   acknowledges): decodes System Time (PGN 126992) and Position Rapid Update (PGN 129025), keeps per-PGN /
 *   per-source counts for a bus check, and learns the Cerbo's source addresses from the ISO Address Claims
 *   (PGN 60928) it overhears, timing the newest frame from them and from everyone else (ANC-25).
 * Version: v3 (2026-10-03) - v3: Cerbo address table + last-frame times (ANC-25). v2: every 126992 is fed to the
 *   boat-time GPS acceptance (bt_sync_t)
 * Scope: PRODUCTION - anchor-box firmware, live on the box at the nav station since 2026-10-02 (installed by signed OTA).
 * Architecture: anchor-box firmware, input layer. ESP-IDF v6 TWAI on-chip node, GPIO 15 TX / 16 RX through the
 *   board's TJA1051 (wiki/Hardware.md). Field layouts from canboat's PGN database (Apache-2.0) and the MSC-10
 *   install manual's PGN list - clean room, nothing from anchor-drag-pro (ANC-01). Started by app_main.c.
 * Invocation: imported by app_main.c; n2k_start() once after board_init().
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "boattime.h"

typedef struct {
    bool     have_time;        /* a valid 126992 seen */
    int64_t  utc_ms;           /* UTC (ms since 1970) from the last valid 126992 */
    int64_t  utc_at_us;        /* esp_timer time when it arrived */
    uint8_t  time_src;         /* sender address (SA) of that 126992 */
    uint8_t  time_kind;        /* 126992 'source' field: 0 = GPS */
    bool     have_pos;         /* a valid 129025 seen */
    double   lat, lon;         /* degrees */
    int64_t  pos_at_us;
    uint8_t  pos_src;
    bt_sync_t sync;            /* GPS time acceptance (two agreeing 126992s) - boattime.c */
} n2k_state_t;

/* The Cerbo's N2K addresses (ANC-25): every source whose Address Claim NAME carries the Victron manufacturer
 * code (CONFIG_ANCHORBOX_CERBO_MFR, 358) - the Cerbo GX and the virtual devices it puts on N2K (tanks, MPPTs,
 * MultiPlus, shunts, system). Keyed by NAME, so an address change after a Cerbo reboot moves the entry. */
#define N2K_CERBO_MAX 32

typedef struct {
    int n;
    uint8_t sa[N2K_CERBO_MAX];
    uint64_t name[N2K_CERBO_MAX];
    int64_t last_cerbo_us;      /* newest frame from any of them, -1 = none since boot */
    int64_t last_other_us;      /* newest frame from any OTHER source, -1 = none */
    uint8_t last_sa;            /* which Cerbo address was heard last */
    bool dirty;                 /* table changed since the last n2k_cerbo_get(.., true) - persist it */
} n2k_cerbo_t;

esp_err_t n2k_start(void);
void n2k_cerbo_get(n2k_cerbo_t *out, bool clear_dirty);
void n2k_cerbo_seed(const uint8_t *sa, const uint64_t *name, int n);   /* table saved in NVS, at start-up */
void n2k_get(n2k_state_t *out);     /* thread-safe snapshot */
void n2k_log_summary(void);         /* frames per PGN / source since the last call, bus state */
