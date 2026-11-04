/**
 * Purpose: The ALERT screen - what the rules engine is alarming about (ANQ-10): the top alert's whole text, the
 *   others one line each; a tap anywhere acks (10-07); open alarms stay until they clear (panel-LED model).
 * Version: v3 (2026-10-21) - v3: RESET button + confirm page (crew RESET, D-163). v2 (2026-10-07) - v2: footer from al_footer (TAP ANYWHERE TO ACK, large); frame steady once acked (panel LED). v1 (2026-10-07)
 * Scope: PRODUCTION - anchor-box firmware (live on the box at the nav station, installed by signed OTA).
 * Architecture: anchor-box UI; owned by screens.c (which decides when it shows and handles the tap); content from
 *   alerts.c / alerts_ctl.c. Black background, red text at FULL brightness even at night, thick red frame that
 *   blinks while anything is unacked - a coloured full screen stays the ANCHOR alarm's (ANC-06). Caller holds
 *   display_lock().
 * Invocation: imported by screens.c.
 */
#pragma once

#include "alerts.h"
#include "lvgl.h"

/* on_reset: the RESET button was tapped (open the confirm page); on_confirm(yes): RESET / CANCEL on it (LVGL task) */
lv_obj_t *ui_alert_create(void (*on_reset)(void), void (*on_confirm)(bool yes));
/* redraw from the current view (1 Hz from screens.c while showing); confirm_id non-empty = the RESET confirm page */
void ui_alert_show(const al_view_t *v, const char *confirm_id);
