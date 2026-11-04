/**
 * Purpose: The CLOCK screen - large red seven-segment HH:MM filling the display, the anchor-watch state symbol,
 *   the manual-zone 'M' mark (SPEC 2.1, 2.2, 2.6, 3.3, 3.5), and a notice line under the digits naming a failed
 *   monitoring host (ANC-25), the rules-engine alerts line (ANQ-10 - tap to read them) and the night-watch bar
 *   (ARMED / OFF / re-arm? / no link - tap to flip; ANQ-30, ANC-28).
 * Version: v4 (2026-10-07) - v4: engine alerts line. v3 (2026-10-06) - v3: night-watch bar. v2: notice line (Pi / Cerbo down). v1 (2026-10-02)
 * Scope: PRODUCTION - anchor-box firmware, live on the box at the nav station since 2026-10-02 (installed by signed OTA).
 * Architecture: anchor-box UI. Built from plain LVGL rectangles / arcs (no font for the digits, so they
 *   scale to the whole screen). Owned by screens.c; time and state are pushed in by app_main.c. Caller holds
 *   display_lock().
 * Invocation: imported by screens.c / app_main.c.
 */
#pragma once

#include <stdbool.h>
#include "lvgl.h"

typedef enum {
    UI_ANCHOR_OFF = 0,        /* no symbol */
    UI_ANCHOR_ACQUIRING,      /* outline anchor, slow blink */
    UI_ANCHOR_LOCKED,         /* solid anchor */
    UI_ANCHOR_CONSERVATIVE,   /* solid anchor + small '!' */
} ui_anchor_t;

lv_obj_t *ui_clock_create(void);
void ui_clock_set_time(int hh, int mm);       /* hh < 0 -> "--:--" (no valid time yet) */
void ui_clock_set_anchor(ui_anchor_t st);
void ui_clock_set_manual_zone(bool on);
void ui_clock_set_night(bool night);          /* dims the red (backlight dimming is not available) */
void ui_clock_set_notice(const char *txt);    /* "" hides it; one or two short lines under the digits */
void ui_clock_nightwatch_tick(void);          /* 1 Hz from screens.c: redraw the night-watch bar if it changed */
void ui_clock_set_alert_line(const char *txt, bool tappable);   /* "" hides it; one line under the notice */
void ui_clock_on_alert_line(lv_event_cb_t cb);                  /* screens.c: a tap on the line opens the list */
