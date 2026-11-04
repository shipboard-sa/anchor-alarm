/**
 * Purpose: Engine alerts on the box (ANQ-10; Ship's Automation spec/04-Interfaces/anchor-box-alerts.md) - pure logic:
 *   parse the rules engine's GET /alerts reply, and decide what the box shows: when the ALERT screen takes over,
 *   the one line on the CLOCK face and the one line on the WATCH screen, and what a tap does (10-07, second
 *   call: a tap anywhere ACKS; open alarms stay on the screen until they clear - the panel-LED model).
 *   Pure C - no ESP-IDF, no I/O.
 * Version: v3 (2026-10-21) - v3: crew RESET (Ship's Automation D-163 / OQ-135): `rst` per alert = the box may reset it
 *   now (a clear: manual alarm whose input reads normal again); al_reset_index / al_detail; reset_pending + reset_id;
 *   err widened for the engine's 409 reason. v2 (2026-10-07) - v2: tap-to-ack + panel-LED model (10-07; replaces v1's view-only + snooze). v1 (2026-10-07)
 * Scope: PRODUCTION - anchor-box firmware (live on the box at the nav station, installed by signed OTA).
 * Architecture: anchor-box firmware, logic layer beside nightwatch.c. The STATE lives in the engine (Pi, :8200); the
 *   box shows only what the engine last reported. Fed by alerts_ctl.c (HTTP task); drawn by ui_alert.c, ui_clock.c,
 *   ui_watch.c; screen switching in screens.c; checked by test_host/test_alerts.c.
 * Invocation: imported by alerts_ctl.c, ui_alert.c, ui_clock.c, ui_watch.c, screens.c and test_host/test_alerts.c.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AL_MAX 4                  /* the engine's default ?max= */
#define AL_TEXT_MAX 344           /* engine cap 340 + room */

typedef struct {
    char id[24];                  /* rule id, e.g. WF-201 */
    char sev[8];                  /* P1 / P2 */
    bool acked, sim;              /* sim = a TEST alarm (D-111) */
    bool rst;                     /* the box may RESET it now (engine api.box_reset; input reads normal) - D-163 */
    int age_s;                    /* seconds since raised, at the engine's answer */
    char text[AL_TEXT_MAX];
} al_item_t;

/* the engine's reply: {"seq", "total", "unacked", "sounding", "alerts": [{id, sev, acked, sim, age_s, text}]} */
typedef struct {
    uint32_t seq;
    int total, unacked;           /* ALL active P1 / P2, the list may be shorter */
    bool sounding;                /* horns on (unacked, not TEST) */
    int n;
    al_item_t a[AL_MAX];
} al_reply_t;

/* true when body is an engine reply ("seq" and an "alerts" array are required) */
bool al_parse(const char *body, al_reply_t *out);

/* what the box knows, assembled by alerts_ctl.c */
typedef struct {
    bool wifi;                    /* Wi-Fi configured on this box */
    bool polled;                  /* at least one poll has finished */
    bool link;                    /* the engine answered recently with a valid reply */
    bool token;                   /* a bearer token is built in (the box token - night watch + ack) */
    bool ack_pending;             /* a tap's ACK is on its way */
    bool reset_pending;           /* a confirmed RESET is on its way ... */
    char reset_id[24];            /* ... for this rule */
    int answer_age_s;             /* seconds since that answer (-1 = never) */
    char err[96];                 /* last ACK / RESET outcome to show, "" = none (shown for a while) */
    al_reply_t r;                 /* the engine's last reply (valid while link) */
} al_view_t;

#define AL_FROM_WATCH_MS (15 * 1000)       /* an unacked alarm takes over from WATCH left alone this long */
#define AL_CLOCK_HOLD_MS (2 * 60 * 1000)   /* acked-only: the CLOCK (night-watch bar) stays this long after a touch */

/* Should the ALERT screen take over now? The panel-LED model (10-07): while ANY P1 / P2 is active the ALERT
 * screen stands in for the CLOCK. Never during an anchor ALARM, never on stale data (no link). Unacked: from CLOCK at
 * once, from WATCH once nobody touched it for AL_FROM_WATCH_MS. Acked only: from CLOCK once nobody touched the box for
 * AL_CLOCK_HOLD_MS (so the night-watch bar stays reachable), never from WATCH (WATCH times out to CLOCK first). */
bool al_take_over(const al_view_t *v, bool anchor_alarm, bool on_clock, uint32_t idle_ms);

/* Should the ALERT screen go away? Only when everything has CLEARED (acked ones stay, like a steady panel LED), or
 * the link is gone (the CLOCK then says NO LINK). */
bool al_release(const al_view_t *v);

typedef enum { AL_TAP_NONE = 0, AL_TAP_ACK, AL_TAP_CLOCK } al_tap_t;
/* A tap anywhere on ALERT: ACK when something is unacked and the box can ack; nothing while an ACK is on its way;
 * otherwise (all acked, or no token / no link) the CLOCK - night-watch bar, and one more tap = WATCH. */
al_tap_t al_tap(const al_view_t *v);

/* The ALERT screen footer: what a tap does now, or why it cannot. */
void al_footer(const al_view_t *v, char *buf, size_t n);

/* The CLOCK face line: "" = nothing to say. Never blank when the link is lost (silence must not read as all-clear). */
void al_clock_line(const al_view_t *v, char *buf, size_t n);

/* The WATCH screen line while an unacked engine alert exists, else "". */
void al_watch_line(const al_view_t *v, char *buf, size_t n);

/* The first listed alert the box may RESET now (rst, link, token, nothing on its way); -1 = none. */
int al_reset_index(const al_view_t *v);

/* Is rule `id` still resettable now (for the confirm page - it closes when the answer changes)? */
bool al_reset_ok(const al_view_t *v, const char *id);

/* a 4xx body {"detail": "WF-104: input still active ..."} -> ASCII text cut to n-1; false if absent */
bool al_detail(const char *body, char *buf, size_t n);

/* "2 min ago" style age for the ALERT screen */
void al_age(int s, char *buf, size_t n);
