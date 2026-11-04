/**
 * Purpose: The CLOCK loop (1 Hz): boat time from boattime.c (GPS UTC + zone + night) onto the CLOCK screen, the
 *   anchor-watch symbol, the zone pin (SPEC 3.5, remembered across power-ups), the status-page lines, the expander
 *   refresh (ANC-16) and the new-firmware health mark.
 * Version: v1 (2026-10-02) - split out of app_main.c v3 (step 4)
 * Scope: PRODUCTION - anchor-box firmware, live on the box at the nav station since 2026-10-02 (installed by signed OTA).
 * Architecture: anchor-box firmware. Reads n2k.c (time, position) and watch_ctl.c (state); draws via ui_clock.c
 *   under display_lock(); posts status via net.c. The WATCH screen's ZONE button calls boatclock_pin().
 * Invocation: imported by app_main.c (boatclock_start) and ui_watch.c (pin + label).
 */
#pragma once

#include <stddef.h>

void boatclock_start(void);
void boatclock_pin(const char *zid);              /* NULL = AUTO; else "CT", "ET", "BS", "AT", "N-5" ... */
void boatclock_label(char *buf, size_t n);        /* e.g. "16:47 CT (auto)" / "--:-- (no GPS time yet)" */
