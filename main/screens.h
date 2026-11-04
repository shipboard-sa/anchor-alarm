/**
 * Purpose: Which view is showing - CLOCK (default / screen saver), WATCH or ALERT - and the rules between them:
 *   a tap on CLOCK wakes WATCH and is swallowed; WATCH returns to CLOCK after 2 min with no touch; an anchor ALARM
 *   forces WATCH at full brightness and holds it (SPEC 2.1-2.5); an unacked rules-engine alarm shows the ALERT
 *   screen (ANQ-10) - never over an anchor ALARM; a tap acks it. The FILL page (ANC-41) opens from WATCH's FILL
 *   button (10-09), BACK -> WATCH, 2 min untouched -> CLOCK; an anchor ALARM (or DRAG? / GPS-stale wake) takes
 *   the screen away from it to WATCH.
 * Version: v5 (2026-10-18) - v5: EXPECTED? view (ANC-45, WF-502 cold-box question; screens.c decides). v4 (2026-10-09) - v4: FILL view (ANC-41). v3 (2026-10-07) - v3: ALERT view (ANQ-10). v2 (2026-10-02) - v2: screens_alarm() for the update gate
 * Scope: PRODUCTION - anchor-box firmware, live on the box at the nav station since 2026-10-02 (installed by signed OTA).
 * Architecture: anchor-box UI controller over ui_clock.c / ui_watch.c / ui_alert.c / ui_fill.c; backlight via board.c. Caller
 *   holds display_lock() for every call.
 * Invocation: imported by app_main.c, watch_ctl.c and ui_watch.c (screens_show_fill / _clock).
 */
#pragma once

#include <stdbool.h>

#define SCREENS_WATCH_TIMEOUT_MS (2 * 60 * 1000)   /* 10-02 */

void screens_init(void);
void screens_show_clock(void);
void screens_show_watch(void);
void screens_show_fill(void);       /* WATCH's FILL button; refused during an anchor ALARM (ANC-41) */
void screens_set_alarm(bool on);    /* step 4 drives this from the watch state machine */
bool screens_on_watch(void);
bool screens_alarm(void);           /* ALARM showing (blocks OTA, SPEC 8.4) */
