/**
 * Purpose: Watchkeeper check-in on the box (ANQ-27; Ship's Automation WF-614 / D-161): polls the rules engine's
 *   GET /watch, sends the crew's tap as POST /watch {action: up} (the box's bearer token), and hands the WATCH CHECK-IN
 *   screen / the status page what the ENGINE reported (or why a tap was refused).
 * Version: v1 (2026-10-21)
 * Scope: PRODUCTION - anchor-box firmware (live on the box at the nav station, installed by signed OTA).
 * Architecture: anchor-box firmware, control layer beside restock_ctl.c. All HTTP happens on the shared
 *   lowest-priority "engine" task (engine_link.c); the UI only sets a request flag and kicks that task. Logic in
 *   wkeep.c. Engine = http://CONFIG_ANCHORBOX_PI_HOST:CONFIG_ANCHORBOX_ENGINE_PORT/watch.
 * Invocation: imported by app_main.c (wkeep_ctl_start, before engine_link_start), engine_link.c (service step),
 *   screens.c (get / up) and boatclock.c (status line). Token = CONFIG_ANCHORBOX_NIGHT_WATCH_TOKEN (sdkconfig.secrets).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "wkeep.h"

void wkeep_ctl_start(void);

/* What the box knows now (any task). */
void wkeep_ctl_get(wk_view_t *v);

/* A tap on the WATCH CHECK-IN screen: queue I'M UP (per wk_tap) and kick the engine task. false = nothing sent. */
bool wkeep_ctl_up(void);

/* One status-page line ("watchkeeper: off - link ok ..."). */
void wkeep_ctl_status(char *buf, size_t n);
