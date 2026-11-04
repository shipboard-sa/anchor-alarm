/**
 * Purpose: HOT WATER tap on the box (ANC-43, Ship's Automation D-135): polls the rules engine's GET /hot-water, sends a
 *   start / stop tap as POST /hot-water (the box's bearer token), and hands WATCH / the status page what the ENGINE
 *   reported (or why a tap was refused - e.g. the house bank below the engine's start floor).
 * Version: v1 (2026-10-11)
 * Scope: PRODUCTION - anchor-box firmware (live on the box at the nav station, installed by signed OTA).
 * Architecture: anchor-box firmware, control layer beside fill_ctl.c. All HTTP happens on the shared lowest-priority
 *   "engine" task (engine_link.c); the UI only sets a request flag and kicks that task. Logic in hotwater.c.
 *   Engine = http://CONFIG_ANCHORBOX_PI_HOST:CONFIG_ANCHORBOX_ENGINE_PORT/hot-water.
 * Invocation: imported by app_main.c (hotwater_ctl_start, before engine_link_start), engine_link.c (service step),
 *   ui_watch.c (get / tap) and boatclock.c (status line). Token = CONFIG_ANCHORBOX_NIGHT_WATCH_TOKEN (sdkconfig.secrets).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "hotwater.h"

void hotwater_ctl_start(void);

/* What the box knows now (any task). */
void hotwater_ctl_get(hw_view_t *v);

/* A tap on WATCH's HOT WATER button: queue start / stop (per hw_tap) and kick the engine task. false = nothing sent. */
bool hotwater_ctl_tap(void);

/* One status-page line ("hot water: off, last run hot 12 min ago - link ok ..."). */
void hotwater_ctl_status(char *buf, size_t n);
