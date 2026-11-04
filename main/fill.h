/**
 * Purpose: Tank-fill taps on the box (ANC-41; Ship's Automation D-129, engine GET / POST /fill) - pure logic: parse
 *   the rules engine's /fill reply, and decide what the FILL page and WATCH's FILL button show (each tank's level,
 *   whether a fill session is on, the watermaker) and what a tap on a tank's button may send. Pure C - no ESP-IDF,
 *   no I/O.
 * Version: v1 (2026-10-09)
 * Scope: PRODUCTION - anchor-box firmware (live on the box at the nav station, installed by signed OTA).
 * Architecture: anchor-box firmware, logic layer beside nightwatch.c / alerts.c. The STATE lives in the engine (Pi,
 *   :8200 - builtins/fill.py); the box is a remote + display and only ever shows what the engine last reported.
 *   Fed by fill_ctl.c (HTTP task); drawn by ui_fill.c (FILL page) and ui_watch.c (FILL / FILLING button); checked by
 *   test_host/test_fill.c.
 * Invocation: imported by fill_ctl.c, ui_fill.c, ui_watch.c and test_host/test_fill.c.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define FL_TANKS 4                /* the engine's fixed keys, in page order */
extern const char *const FL_KEYS[FL_TANKS];   /* water_port, water_stbd, fuel_port, fuel_stbd */

typedef enum { FL_SRC_NONE = 0, FL_SRC_TAP, FL_SRC_WATERMAKER } fl_src_t;

typedef struct {
    bool present;                 /* the engine listed this tank (engine.yml may configure fewer) */
    char label[48];               /* e.g. "PORT FRESH WATER (port string)" ("" -> the key) */
    bool has_level;               /* level null = sender UNKNOWN */
    float level;                  /* % */
    fl_src_t src;                 /* NONE = no session */
    bool active;                  /* session watching this tank (a watermaker session waits for its string to rise) */
    char since[24];               /* UTC ISO, "" when missing */
    char by[24];
} fl_tank_t;

/* the engine's reply: {"tanks": {key: {label, level, session: null | {source, since, by, active}}},
 *                      "watermaker": {running, watts}} */
typedef struct {
    fl_tank_t t[FL_TANKS];
    bool wm_running;
    bool wm_has_watts;
    float wm_watts;
} fl_reply_t;

/* true when body is an engine reply (a "tanks" object holding at least one known tank is required) */
bool fl_parse(const char *body, fl_reply_t *out);

/* what the box knows, assembled by fill_ctl.c */
typedef struct {
    bool wifi;                    /* Wi-Fi configured on this box */
    bool polled;                  /* at least one poll has finished */
    bool link;                    /* the engine answered recently with a valid reply */
    bool token;                   /* the box token is built in (CONFIG_ANCHORBOX_NIGHT_WATCH_TOKEN) */
    int get_http;                 /* last GET status (0 = no answer, -1 = none yet) - says WHY there is no link */
    bool pending;                 /* a tap is being sent */
    int pending_tank;             /* ... for this tank (index into FL_KEYS) */
    bool pending_on;              /* ... start (true) or stop */
    char err[48];                 /* last tap that did not go through, "" = none (shown for a while) */
    fl_reply_t eng;               /* the engine's last reply (valid while link) */
} fl_view_t;

#define FL_NAME_FIT 32            /* a row name longer than this drops a trailing "(...)" */

/* One tank row on the FILL page. */
typedef struct {
    bool shown;                   /* the engine listed this tank */
    bool filling;                 /* a session is on and watching this tank: drawn heavier */
    char name[48];                /* the engine's label */
    char state[72];               /* "85 %   not filling" / "92 %   FILLING - started by anchor-box" ... */
    char btn[16];                 /* "FILLING" / "STOP" / "sending ..." / "watermaker" / "" */
    bool can_tap;                 /* a tap sends tap_on for this tank */
    bool tap_on;
} fl_row_t;

void fl_row(const fl_view_t *v, int i, fl_row_t *r);

/* May a tap on tank i's button go now, and what does it ask? false = nothing is sent. Never while another tap is on
 * its way, without link / token, for a tank the engine did not list, or for a WATERMAKER session (it ends when the
 * watermaker stops; stopping it here would also drop the 95 % HP-pump cut, D-130). */
bool fl_tap(const fl_view_t *v, int i, bool *on);

/* The FILL page head line (watermaker) and footer (what a tap does, or why it cannot). */
void fl_head(const fl_view_t *v, char *buf, size_t n);
void fl_footer(const fl_view_t *v, char *buf, size_t n);

/* Fill sessions on, for WATCH's button ('FILLING' when > 0): small = "ON" / "2 ON" while sessions are on, "" otherwise.
 * Returns the number of sessions on (watermaker sessions still waiting for a rise are not counted; no link = 0). */
int fl_tab(const fl_view_t *v, char *small, size_t n);
