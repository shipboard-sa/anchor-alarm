/**
 * Purpose: Tank-fill taps on the box (ANC-41, Ship's Automation D-129): polls the rules engine's GET /fill, sends a
 *   'Filling' / 'Stop' tap as POST /fill (the box's bearer token), and hands the UI / status page what the ENGINE
 *   reported.
 * Version: v2 (2026-10-09) - v2: served by the shared engine link (engine_link.c, ANQ-33). v1 (2026-10-09)
 * Scope: PRODUCTION - anchor-box firmware (live on the box at the nav station, installed by signed OTA).
 * Architecture: anchor-box firmware, control layer beside nightwatch_ctl.c. All HTTP happens on the shared
 *   lowest-priority "engine" task (engine_link.c); the UI only sets a request flag and kicks that task, so neither the LVGL task,
 *   the anchor watch nor the host watch ever waits on the network. Logic in fill.c.
 *   Engine = http://CONFIG_ANCHORBOX_PI_HOST:CONFIG_ANCHORBOX_ENGINE_PORT/fill.
 * Invocation: imported by app_main.c (fill_ctl_start, after net_start), ui_fill.c / ui_watch.c / screens.c (get / tap / fast)
 *   and boatclock.c (status line). Token = CONFIG_ANCHORBOX_NIGHT_WATCH_TOKEN (sdkconfig.secrets).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "fill.h"

void fill_ctl_start(void);
void fill_ctl_get(fl_view_t *v);
/* a tap on tank i's button: asks the engine for the opposite of its last reported session (fl_tap). false = not
 * allowed now - nothing is sent and nothing changes on screen */
bool fill_ctl_tap(int i);
/* the FILL page is open: poll every 2 s instead of every CONFIG_ANCHORBOX_FILL_POLL_S (and ask at once) */
void fill_ctl_fast(bool on);
/* status-page line (no trailing newline); never contains the token */
void fill_ctl_status(char *buf, size_t n);
