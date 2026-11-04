/**
 * Purpose: The FILL page (ANC-41, Ship's Automation D-129) - the four tanks the rules engine knows, each with its
 *   level, whether a fill session is on and a FILLING / STOP button; the watermaker's state; BACK.
 * Version: v1 (2026-10-09)
 * Scope: PRODUCTION - anchor-box firmware (live on the box at the nav station, installed by signed OTA).
 * Architecture: anchor-box UI; owned by screens.c (which decides when it shows: from WATCH's FILL button, never
 *   during an anchor ALARM); content from fill.c / fill_ctl.c. Black background, red like the WATCH pages; a
 *   coloured area stays the anchor ALARM's (ANC-06). Caller holds display_lock().
 * Invocation: imported by screens.c.
 */
#pragma once

#include "lvgl.h"

lv_obj_t *ui_fill_create(lv_event_cb_t back_cb);   /* back_cb: the BACK button (screens.c -> CLOCK) */
void ui_fill_tick(void);                           /* 1 Hz from screens.c while showing, and right after a tap */
