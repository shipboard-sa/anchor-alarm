/**
 * Purpose: Runs the anchor watch on the box (step 4): feeds anchorwatch.c the MSC-10 position once a second,
 *   drives the panel output (DO0 = ALARM, held through MUTE until DISARM - SPEC 1.5, ANC-17), wakes the screen on
 *   ALARM / DRAG?, keeps the last events for the WATCH screen and status page, and keeps the CIRCLE across a power
 *   cut (restored as it was, no age limit; armed-without-circle re-arms) - 10-02.
 * Version: v3 (2026-10-02) - v3: RE-ARM (same rode) + breadcrumb track, 10 s x 6 h (10-02, ANQ-16). v2: the circle
 *   survives a power cut (NVS 'snap', no age limit - 10-02)
 * Scope: PRODUCTION - anchor-box firmware, live on the box at the nav station since 2026-10-02 (installed by signed OTA).
 * Architecture: anchor-box firmware, control layer between n2k.c (positions), anchorwatch.c (the model), board.c
 *   (DO0), screens.c (ALARM display) and ui_watch.c / net.c (buttons, status). Own task at 1 Hz; the watch itself
 *   lives in PSRAM. Never calls into LVGL while holding its own mutex (no lock-order deadlock with the UI).
 * Invocation: imported by app_main.c (watch_ctl_start), ui_watch.c (buttons + view), net.c gate (via app_main).
 */
#pragma once

#include <stdbool.h>
#include "anchorwatch.h"

#define WATCH_LOG_N 5

typedef struct {
    aw_state_t state;
    bool muted, drag, stale, have_pos;
    double elapsed_s;                 /* since ARM */
    double rode_ft;                   /* 0 = none */
    bool have_distance, have_alarm_radius;
    double distance_m, alarm_radius_m;
    char reason[160];                 /* why not locked yet */
    char log[WATCH_LOG_N][96];        /* newest first */
    int n_log;
    bool have_centre;                 /* frozen circle centre, or the provisional fit while ACQUIRING */
    double centre_lat, centre_lon;
    bool have_fix;
    double lat, lon;                  /* latest position */
} watch_view_t;

typedef struct { float lat_off, lon_off; } watch_crumb_t;   /* degrees from the track origin (float: < 1 m error) */

#define WATCH_TRACK_N 2160            /* 6 h at one point per 10 s */

void watch_ctl_start(void);
void watch_ctl_arm(double rode_ft);   /* rode_ft <= 0 = none (no coarse guard) */
void watch_ctl_disarm(void);
void watch_ctl_mute(void);
void watch_ctl_rearm(void);           /* fresh watch, same rode ('reset the circle', SPEC 5.13) */
/* copies the breadcrumb track (oldest first) into out[max]; returns the count and the origin it is relative to */
int watch_ctl_track(watch_crumb_t *out, int max, double *lat0, double *lon0);
void watch_ctl_get(watch_view_t *v);
bool watch_ctl_armed(void);           /* ACQUIRING / LOCKED / CONSERVATIVE / ALARM */
