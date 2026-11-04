/**
 * Purpose: DOCK / LEAVE taps on the box (ANC-44; Ship's Automation D-151, engine GET / POST /mode) - pure logic: parse
 *   the rules engine's GET /mode reply (and its 403 refusal), and decide what the ARM page's DOCK button and the WATCH
 *   screen (AT DOCK lines, LEAVE button) show and what a tap may send. Pure C - no ESP-IDF, no I/O.
 * Version: v1 (2026-10-15)
 * Scope: PRODUCTION - anchor-box firmware (live on the box at the nav station, installed by signed OTA).
 * Architecture: anchor-box firmware, logic layer beside hotwater.c. The STATE lives in the engine (Pi, :8200 -
 *   builtins/mode.py + fence.py): DOCK arms the WF-108 slip fence, only another tap ends it (D-092), and the engine
 *   lets THIS box end it only while one of our phones is on the boat Wi-Fi (leave_ok, D-151). The box is a remote +
 *   display and only shows what the engine last reported. Fed by boatmode_ctl.c (engine link); drawn by ui_watch.c;
 *   checked by test_host/test_boatmode.c. Nothing here touches the box's own anchor watch.
 * Invocation: imported by boatmode_ctl.c, ui_watch.c and test_host/test_boatmode.c.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef enum { BM_NONE = 0, BM_DOCK, BM_ANCHOR, BM_MOTOR, BM_SAIL, BM_AWAY } bm_mode_t;

/* the engine's reply: {mode: null | "DOCK" | ..., secured, crew_aboard: [...], leave_ok, box_modes: [...]} */
typedef struct {
    bm_mode_t mode;
    bool secured;                 /* DOCK / ANCHOR tap holds (only another tap ends it) */
    bool leave_ok;                /* the engine will take an end-it tap from this box now (a crew phone aboard) */
    int crew;                     /* how many crew phones the engine sees aboard */
} bm_reply_t;

/* true when body is an engine /mode reply ("secured" and "leave_ok" true/false and a known or null "mode") */
bool bm_parse(const char *body, bm_reply_t *out);

/* the 403 body {"detail": "DOCK is armed and no crew phone ..."} -> ASCII text cut to n-1; false if absent */
bool bm_detail(const char *body, char *buf, size_t n);

/* what the box knows, assembled by boatmode_ctl.c */
typedef struct {
    bool wifi;                    /* Wi-Fi configured on this box */
    bool polled;                  /* at least one poll has finished */
    bool link;                    /* the engine answered recently with a valid reply */
    bool token;                   /* the box token is built in */
    int get_http;                 /* last GET status (0 = no answer, -1 = none yet) */
    bool pending;                 /* a tap is being sent */
    bm_mode_t pending_mode;       /* ... DOCK or MOTOR */
    char msg[96];                 /* last tap's outcome worth saying (refusal / error), "" = none (shown a while) */
    bm_reply_t eng;               /* the engine's last reply (valid while link) */
} bm_view_t;

/* The engine says the boat is tapped DOCK (slip fence armed). Only with a live link - never a guess. */
bool bm_at_dock(const bm_view_t *v);

/* May the ARM page's DOCK button send now? (link + token, nothing on its way, not already DOCK, and not over an armed
 * ANCHOR unless the engine would allow it - leave_ok). */
bool bm_dock_ok(const bm_view_t *v);

/* May WATCH's LEAVE button send now? (at DOCK, link + token, nothing on its way, leave_ok). */
bool bm_leave_ok(const bm_view_t *v);

/* WATCH's two state lines while the engine says DOCK (or a tap is pending / just refused):
 * l1 e.g. "AT DOCK - slip fence armed", l2 the LEAVE hint / the refusal. Both "" = nothing to say (normal watch
 * lines stay). */
void bm_lines(const bm_view_t *v, char *l1, size_t n1, char *l2, size_t n2);

/* the POST body for a tap: {"mode": "DOCK", "by": "anchor-box"} (the engine tags the box itself, D-151) */
void bm_post_body(bm_mode_t m, char *buf, size_t n);

const char *bm_name(bm_mode_t m);
