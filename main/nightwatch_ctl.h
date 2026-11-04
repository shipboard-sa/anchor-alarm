/**
 * Purpose: Night-watch remote control on the box (ANQ-30, ANC-27): polls the rules engine's GET /night-watch every
 *   ~5 s, sends a tap as POST /night-watch (bearer token), and hands the UI / status page what the ENGINE reported.
 * Version: v2 (2026-10-09) - v2: served by the shared engine link (engine_link.c, ANQ-33). v1 (2026-10-06)
 * Scope: PRODUCTION - anchor-box firmware (live on the box at the nav station, installed by signed OTA).
 * Architecture: anchor-box firmware, control layer beside hostwatch_ctl.c. All HTTP happens on the shared
 *   lowest-priority "engine" task (engine_link.c); the UI only sets a request flag and kicks that task, so neither
 *   the LVGL task, the anchor watch nor the host watch ever waits on the network. Logic in nightwatch.c.
 *   Engine = http://CONFIG_ANCHORBOX_PI_HOST:CONFIG_ANCHORBOX_ENGINE_PORT/night-watch.
 * Invocation: imported by app_main.c (nightwatch_ctl_start, after net_start), ui_clock.c (get / toggle) and
 *   boatclock.c (status line). Token from sdkconfig.secrets (CONFIG_ANCHORBOX_NIGHT_WATCH_TOKEN).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "nightwatch.h"

void nightwatch_ctl_start(void);
void nightwatch_ctl_get(nw_view_t *v);
/* a tap on the bar: asks the engine for the opposite of its last reported state. false = not allowed now
 * (no link / no token / already sending) - nothing is sent and nothing changes on screen */
bool nightwatch_ctl_toggle(void);
/* status-page line (no trailing newline); never contains the token */
void nightwatch_ctl_status(char *buf, size_t n);
