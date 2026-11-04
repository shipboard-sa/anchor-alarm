/**
 * Purpose: Engine alerts on the box (ANQ-10) - the HTTP side: polls the rules engine's GET /alerts every ~2 s, sends a
 *   tap's ACK (POST /ack with the box token - 10-07, risk accepted), and hands the UI / status page what the
 *   ENGINE reported.
 * Version: v4 (2026-10-21) - v4: alerts_ctl_reset (crew RESET, D-163). v3 (2026-10-09) - v3: served by the shared engine link (engine_link.c, ANQ-33). v2 (2026-10-07) - v2:
 *   alerts_ctl_ack (tap-to-ack). v1 (2026-10-07): read-only.
 * Scope: PRODUCTION - anchor-box firmware (live on the box at the nav station, installed by signed OTA).
 * Architecture: anchor-box firmware, control layer beside nightwatch_ctl.c. All HTTP happens on the shared
 *   lowest-priority "engine" task (engine_link.c); the UI only sets a request flag and kicks that task, so neither the
 *   LVGL task, the anchor watch nor the host watch ever waits on the network. Logic in alerts.c. Engine =
 *   http://CONFIG_ANCHORBOX_PI_HOST:CONFIG_ANCHORBOX_ENGINE_PORT/alerts and /ack.
 * Invocation: imported by app_main.c (alerts_ctl_start, after net_start), screens.c / ui_alert.c / ui_clock.c /
 *   ui_watch.c (alerts_ctl_get, alerts_ctl_ack) and boatclock.c (status line).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "alerts.h"

void alerts_ctl_start(void);
/* a copy of the current view (about 1.5 KB - callers keep it in PSRAM, not on a small stack) */
void alerts_ctl_get(al_view_t *v);
/* a tap's ACK: false = not allowed now (no link / no token / nothing unacked / already sending) - nothing is sent */
bool alerts_ctl_ack(void);
/* a confirmed RESET of rule id (D-163): false = not allowed now (not resettable / no token / something on its way) */
bool alerts_ctl_reset(const char *id);
/* status-page line (no trailing newline); never contains the token */
void alerts_ctl_status(char *buf, size_t n);
