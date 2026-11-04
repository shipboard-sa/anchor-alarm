/**
 * Purpose: DOCK / LEAVE taps on the box (ANC-44, Ship's Automation D-151): polls the rules engine's GET /mode, sends a
 *   DOCK or LEAVE (MOTOR) tap as POST /mode (the box's bearer token), and hands WATCH / the status page what the ENGINE
 *   reported (or why a tap was refused - e.g. no crew phone on the boat Wi-Fi).
 * Version: v1 (2026-10-15)
 * Scope: PRODUCTION - anchor-box firmware (live on the box at the nav station, installed by signed OTA).
 * Architecture: anchor-box firmware, control layer beside hotwater_ctl.c. All HTTP happens on the shared
 *   lowest-priority "engine" task (engine_link.c); the UI only sets a request flag and kicks that task. Logic in
 *   boatmode.c. Engine = http://CONFIG_ANCHORBOX_PI_HOST:CONFIG_ANCHORBOX_ENGINE_PORT/mode.
 * Invocation: imported by app_main.c (boatmode_ctl_start, before engine_link_start), engine_link.c (service step),
 *   ui_watch.c (get / tap) and boatclock.c (status line). Token = CONFIG_ANCHORBOX_NIGHT_WATCH_TOKEN (sdkconfig.secrets).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "boatmode.h"

void boatmode_ctl_start(void);

/* What the box knows now (any task). */
void boatmode_ctl_get(bm_view_t *v);

/* A tap: DOCK (ARM page) or MOTOR (WATCH's LEAVE). Queued only when bm_dock_ok / bm_leave_ok allow it; kicks the
 * engine task. false = nothing sent. */
bool boatmode_ctl_tap(bm_mode_t m);

/* One status-page line ("boat mode: DOCK (secured), 1 crew phone aboard, leave ok - link ok ..."). */
void boatmode_ctl_status(char *buf, size_t n);
