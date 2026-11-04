/**
 * Purpose: Cold-box 'EXPECTED?' question on the box (ANC-45; Ship's Automation WF-502 / D-128, engine GET / POST
 *   /restock) - pure logic: parse the rules engine's /restock reply (and its 409 refusal), decide when the EXPECTED?
 *   screen takes over / goes away, which box it asks about (one at a time), its words, and what a YES / NO tap sends.
 *   Pure C - no ESP-IDF, no I/O.
 * Version: v2 (2026-10-18) - v2: YES / NO only - no 'not now' (10-18): from CLOCK at once; RS_CLOCK_HOLD_MS gone. v1 (2026-10-18)
 * Scope: PRODUCTION - anchor-box firmware (live on the box at the nav station, installed by signed OTA).
 * Architecture: anchor-box firmware, logic layer beside hotwater.c / boatmode.c. The STATE lives in the engine (Pi,
 *   :8200 - builtins/restock.py): WF-502-<BOX> asks 'expected?' when a fridge / freezer has been over its WF-501
 *   limit 10 min with no restock on record; YES = that box's WF-501 waits hold_h (2 h), NO = WF-501 fires now. The
 *   box only asks while the engine says so and only ever sends the crew's tap. Fed by restock_ctl.c (engine link);
 *   drawn by ui_ask.c; screen switching in screens.c; checked by test_host/test_restock.c.
 * Invocation: imported by restock_ctl.c, ui_ask.c, screens.c and test_host/test_restock.c.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RS_BOXES 4                /* fridge, engel, fz_aft, fz_fwd - asked in this order, one at a time */

typedef enum { RS_MARK_NONE = 0, RS_MARK_HOLD, RS_MARK_DENIED } rs_mark_t;

typedef struct {
    bool known;                   /* the engine listed this box */
    char label[24];               /* e.g. "REFRIGERATOR" */
    bool asking;                  /* its WF-502 'expected?' is up */
    bool alarm;                   /* its WF-501 alarm is up */
    rs_mark_t mark;               /* hold (restock / YES) or denied (NO), none = no mark */
} rs_box_t;

/* the engine's reply: {boxes: {fridge|engel|fz_aft|fz_fwd: {label, ask_rule, asking, alarm, mark: null | {kind, ...}}},
 *                      hold_h} */
typedef struct {
    rs_box_t b[RS_BOXES];
    float hold_h;
} rs_reply_t;

/* engine keys, index = rs_box_t position */
extern const char *const RS_KEYS[RS_BOXES];

/* true when body is an engine reply (a "boxes" object and a "hold_h" number are required; at least one known box) */
bool rs_parse(const char *body, rs_reply_t *out);

/* a 4xx body {"detail": "the REFRIGERATOR alarm is already up ..."} -> ASCII text cut to n-1; false if absent */
bool rs_detail(const char *body, char *buf, size_t n);

/* what the box knows, assembled by restock_ctl.c */
typedef struct {
    bool wifi;                    /* Wi-Fi configured on this box */
    bool polled;                  /* at least one poll has finished */
    bool link;                    /* the engine answered recently with a valid reply */
    bool token;                   /* the box token is built in */
    int get_http;                 /* last GET status (0 = no answer, -1 = none yet) */
    bool pending;                 /* a YES / NO is being sent */
    int pending_box;              /* ... for this box */
    bool pending_yes;             /* ... YES (true) or NO */
    bool msg_ok;                  /* msg is a confirmation (the answer went through), not a refusal */
    char msg[128];                /* last tap's outcome, "" = none (shown a while) */
    rs_reply_t eng;               /* the engine's last reply (valid while link) */
} rs_view_t;

#define RS_FROM_WATCH_MS (15 * 1000)       /* the question takes over from WATCH left alone this long (as alerts) */

/* The box to ask about now: the first (RS_KEYS order) whose question is up and not answered (asking, no mark, its
 * alarm not already up); -1 = none. *count = how many boxes are asking (may be NULL). Never on stale data. */
int rs_asking(const rs_view_t *v, int *count);

/* Should the EXPECTED? screen take over now? Only while a box is asking, never during an anchor ALARM, never on
 * stale data. From CLOCK at once; from WATCH (or FILL) after RS_FROM_WATCH_MS untouched. YES / NO only - no 'not
 * now' (10-18): once up it stays until answered or the engine stops asking. The caller gives an engine ALERT
 * priority (screens.c). */
bool rs_take_over(const rs_view_t *v, bool anchor_alarm, bool on_clock, uint32_t idle_ms);

/* Should the EXPECTED? screen go away? When nothing is asking and no answer is on its way or being confirmed, or the
 * link is gone. */
bool rs_release(const rs_view_t *v);

/* May a YES / NO tap go now? false = nothing is sent (no link / token, an answer already on its way, the box no longer
 * asking). *box = which box it answers. */
bool rs_tap(const rs_view_t *v, int *box);

/* The question: "<LABEL> warm 10 min - warm food just went in?" (box = rs_asking's). */
void rs_question(const rs_view_t *v, int box, char *buf, size_t n);

/* Button faces: "YES\ngive it 2 h" (from hold_h) / "NO\nalarm now". */
void rs_yes_label(const rs_view_t *v, char *buf, size_t n);
void rs_no_label(char *buf, size_t n);

/* The footer: what a tap does, 'sending ...', the outcome, or why the box cannot answer. count = rs_asking's. */
void rs_footer(const rs_view_t *v, int count, char *buf, size_t n);

/* POST /restock body for this box + answer. */
void rs_post_body(int box, bool yes, char *buf, size_t n);
