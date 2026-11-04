/**
 * Purpose: The WATCH screen (anchor watch GUI, SPEC 2 / 3.5 / 5): state, distance vs the alarm circle, lock reason,
 *   warnings, recent events; ARM (rode pick), DISARM (confirmed), MUTE, ZONE, CLOCK; red ALARM banner.
 * Version: v3 (2026-10-07) - v3: rules-engine alert line (ANQ-10). v2 (2026-10-02) - v2: the real WATCH screen (step 4). v1: step-2 bench placeholder.
 * Scope: PRODUCTION - anchor-box firmware, live on the box at the nav station since 2026-10-02 (installed by signed OTA).
 * Architecture: anchor-box UI; owned by screens.c; reads / commands watch_ctl.c and boatclock.c. Caller holds
 *   display_lock().
 * Invocation: imported by screens.c.
 */
#pragma once

#include "lvgl.h"

lv_obj_t *ui_watch_create(void);
void ui_watch_tick(void);       /* 1 Hz refresh from screens.c's LVGL timer */
void ui_watch_set_engine_line(const char *txt);   /* "" = none; an unacked rules-engine alarm (ANQ-10) */
