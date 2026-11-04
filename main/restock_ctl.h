/**
 * Purpose: Cold-box 'EXPECTED?' question on the box (ANC-45, Ship's Automation WF-502 / D-128): polls the rules
 *   engine's GET /restock, sends the crew's YES / NO as POST /restock (the box's bearer token), and hands the
 *   EXPECTED? screen / the status page what the ENGINE reported (or why an answer was refused).
 * Version: v1 (2026-10-18)
 * Scope: PRODUCTION - anchor-box firmware (live on the box at the nav station, installed by signed OTA).
 * Architecture: anchor-box firmware, control layer beside hotwater_ctl.c. All HTTP happens on the shared
 *   lowest-priority "engine" task (engine_link.c); the UI only sets a request flag and kicks that task. Logic in
 *   restock.c. Engine = http://CONFIG_ANCHORBOX_PI_HOST:CONFIG_ANCHORBOX_ENGINE_PORT/restock.
 * Invocation: imported by app_main.c (restock_ctl_start, before engine_link_start), engine_link.c (service step),
 *   screens.c (get / answer) and boatclock.c (status line). Token = CONFIG_ANCHORBOX_NIGHT_WATCH_TOKEN
 *   (sdkconfig.secrets).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "restock.h"

void restock_ctl_start(void);

/* What the box knows now (any task). */
void restock_ctl_get(rs_view_t *v);

/* A tap on YES / NO: queue the answer for the box being asked (per rs_tap) and kick the engine task. false = nothing
 * sent. */
bool restock_ctl_answer(bool yes);

/* One status-page line ("restock: none asking ... - link ok ..."). */
void restock_ctl_status(char *buf, size_t n);
