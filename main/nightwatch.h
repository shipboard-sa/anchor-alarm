/**
 * Purpose: Night-watch remote control on the box (ANQ-30, Ship's Automation D-118 / WF-114 v2) - pure logic: parse the
 *   rules engine's GET/POST /night-watch JSON reply, and decide what the CLOCK screen's night-watch bar shows
 *   (ARMED / OFF / re-arm nudge / no link / sending) and whether a tap is allowed. Pure C - no ESP-IDF, no I/O.
 * Version: v2 (2026-10-07) - v2: nw_bar_shown - the bar shows only after dark, or while ARMED / nudging (10-07). v1 (2026-10-06)
 * Scope: PRODUCTION - anchor-box firmware (live on the box at the nav station, installed by signed OTA).
 * Architecture: anchor-box firmware, logic layer. The STATE lives in the engine (Pi, :8200); the box is a remote +
 *   display and only ever shows what the engine last reported. Fed by nightwatch_ctl.c (HTTP task); faces drawn by
 *   ui_clock.c; checked by test_host/test_nightwatch.c.
 * Invocation: imported by nightwatch_ctl.c, ui_clock.c and test_host/test_nightwatch.c.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/* the engine's reply: {"armed": bool, "since": "ISO" | null, "by": "str", "nudge": bool, "night": bool} */
typedef struct {
    bool armed, nudge, night;
    char since[32];               /* "" when null / missing */
    char by[24];
} nw_reply_t;

/* true when body is an engine reply (an "armed" boolean is required; nudge / night default false) */
bool nw_parse(const char *body, nw_reply_t *out);

/* what the box knows, assembled by nightwatch_ctl.c */
typedef struct {
    bool wifi;                    /* Wi-Fi configured on this box */
    bool polled;                  /* at least one poll has finished */
    bool link;                    /* the engine answered recently with a valid reply */
    bool token;                   /* a bearer token is built in (CONFIG_ANCHORBOX_NIGHT_WATCH_TOKEN) */
    bool pending;                 /* a tap is being sent */
    bool pending_arm;             /* ... asking for this */
    nw_reply_t eng;               /* the engine's last reply (valid while link) */
    char err[48];                 /* last tap that did not go through, "" = none (shown for a while) */
} nw_view_t;

typedef enum {
    NWF_WAIT = 0,                 /* no answer yet - dim, not tappable */
    NWF_NO_LINK,                  /* engine unreachable - dim, not tappable */
    NWF_ARMED,                    /* solid bar */
    NWF_OFF,                      /* hollow bar */
    NWF_NUDGE,                    /* hollow bar, full red, blinking: "re-arm?" */
} nw_kind_t;

typedef struct {
    nw_kind_t kind;
    bool tappable;                /* a tap sends the opposite of the engine's state */
    char big[32];                 /* the line that must read from across the cabin */
    char small[64];               /* the hint / reason line, "" = none */
} nw_face_t;

void nw_face(const nw_view_t *v, nw_face_t *f);

/* Is the bar on the CLOCK screen at all? (10-07: 'night time only'.) dark = the box's own night (sun altitude,
 * the same flag that dims the red). In daylight the bar is hidden unless night watch is ARMED (a watch left on in
 * the morning must be seen before going up on deck) or the engine is nudging. Hidden = not tappable. */
bool nw_bar_shown(nw_kind_t kind, bool dark);
