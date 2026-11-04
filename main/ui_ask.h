/**
 * Purpose: The EXPECTED? screen - the rules engine's WF-502 cold-box question (ANC-45, Ship's Automation D-128):
 *   '<LABEL> warm 10 min - warm food just went in?' with YES (give it 2 h) / NO (alarm now); one tap answers, no
 *   confirm (10-18: y/n).
 * Version: v2 (2026-10-18) - v2: YES / NO only, no 'not now' (10-18). v1 (2026-10-18)
 * Scope: PRODUCTION - anchor-box firmware (live on the box at the nav station, installed by signed OTA).
 * Architecture: anchor-box UI; owned by screens.c (which decides when it shows; a press off the buttons does nothing);
 *   content from restock.c / restock_ctl.c. Black background, red text, a thin steady frame - a question, not an
 *   alarm (the ALERT screen's thick blinking frame and the anchor ALARM's full red stay theirs). Caller holds
 *   display_lock().
 * Invocation: imported by screens.c.
 */
#pragma once

#include <stdbool.h>
#include "lvgl.h"
#include "restock.h"

/* answer(yes) is called from a YES / NO button tap (LVGL task). */
lv_obj_t *ui_ask_create(void (*answer)(bool yes));
/* redraw from the current view (1 Hz from screens.c while showing, and at once after a tap) */
void ui_ask_show(const rs_view_t *v);
