/**
 * Purpose: The WATCH CHECK-IN screen - the rules engine's watchkeeper timer (ANQ-27; Ship's Automation WF-614 / D-161):
 *   'WATCH CHECK-IN - N min since the last I'm up' at full brightness; a tap anywhere = I'M UP.
 * Version: v1 (2026-10-21)
 * Scope: PRODUCTION - anchor-box firmware (live on the box at the nav station, installed by signed OTA).
 * Architecture: anchor-box UI; owned by screens.c (which decides when it shows and handles the tap); content from
 *   wkeep.c / wkeep_ctl.c. Black background, red text at FULL brightness even at night (it must wake a dozing
 *   watchkeeper - no buzzer here, ANQ-27); thick frame, steady at stage 1, blinking from stage 2. A coloured full
 *   screen stays the ANCHOR alarm's (ANC-06). Caller holds display_lock().
 * Invocation: imported by screens.c.
 */
#pragma once

#include "lvgl.h"
#include "wkeep.h"

lv_obj_t *ui_wkup_create(void);
/* redraw from the current view (1 Hz from screens.c while showing, and at once after a tap) */
void ui_wkup_show(const wk_view_t *v);
