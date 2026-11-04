/**
 * Purpose: Watchkeeper check-in on the box (ANQ-27; Ship's Automation WF-614 / D-161, engine GET / POST /watch) -
 *   pure logic: parse the rules engine's /watch reply (and its 4xx refusal), decide when the WATCH CHECK-IN screen
 *   takes over / goes away, its words, and what a tap sends (I'M UP). Pure C - no ESP-IDF, no I/O.
 * Version: v1 (2026-10-21)
 * Scope: PRODUCTION - anchor-box firmware (live on the box at the nav station, installed by signed OTA).
 * Architecture: anchor-box firmware, logic layer beside restock.c. The STATE and the CLOCK live in the engine (Pi,
 *   :8200 - builtins/watchkeeper.py): it starts at sunset underway (MOTOR / SAIL) or on a cockpit Start, and counts
 *   from the last I'm up: stage 1 = 15 min (check-in prompt), 2 = 20 min (NUDGE), 3 = 25 min (ALARM - a P1 that the
 *   ALERT screen shows; an ack there also counts as I'm up). The box only shows what the engine reports and sends the
 *   crew's tap (POST /watch {action: up} with the box token - the engine lets that token tap I'M UP only). Fed by
 *   wkeep_ctl.c (engine link); drawn by ui_wkup.c; screen switching in screens.c; checked by test_host/test_wkeep.c.
 * Invocation: imported by wkeep_ctl.c, ui_wkup.c, screens.c and test_host/test_wkeep.c.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* the engine's GET /watch reply (api.py v15): {on, stage, started_by, since, last_up, last_up_by, since_up_s,
 * next_prompt_s, prompt_s, nudge_s, alarm_s, dark, held, mode} */
typedef struct {
    bool on;                      /* the watch timer runs */
    int stage;                    /* 0 none, 1 prompt (15 min), 2 nudge (20), 3 alarm (25) */
    int since_up_s;               /* seconds since the last I'm up (-1 = not running) */
    int next_prompt_s;            /* seconds to the next check-in (-1 = not running) */
    int alarm_s;                  /* when stage 3 comes (default 1500 s) */
    char mode[8];                 /* boat mode, e.g. MOTOR */
} wk_reply_t;

/* true when body is an engine reply ("on" bool and "stage" number required) */
bool wk_parse(const char *body, wk_reply_t *out);

/* a 4xx body {"detail": "no watch timer running"} -> ASCII text cut to n-1; false if absent */
bool wk_detail(const char *body, char *buf, size_t n);

/* what the box knows, assembled by wkeep_ctl.c */
typedef struct {
    bool wifi;                    /* Wi-Fi configured on this box */
    bool polled;                  /* at least one poll has finished */
    bool link;                    /* the engine answered recently with a valid reply */
    bool token;                   /* the box token is built in */
    int get_http;                 /* last GET status (0 = no answer, -1 = none yet) */
    bool pending;                 /* an I'M UP is being sent */
    bool msg_ok;                  /* msg confirms the tap went through, not a refusal */
    char msg[96];                 /* last tap's outcome, "" = none (shown a while) */
    wk_reply_t eng;               /* the engine's last reply (valid while link) */
} wk_view_t;

#define WK_FROM_WATCH_MS (15 * 1000)       /* takes over from WATCH / FILL left alone this long (as alerts) */

/* Is the engine asking for a check-in (running, stage >= 1)? Never on stale data. */
bool wk_due(const wk_view_t *v);

/* Should the WATCH CHECK-IN screen take over now? Only while a check-in is due, never during an anchor ALARM. From
 * CLOCK at once; from WATCH (or FILL) after WK_FROM_WATCH_MS untouched. The caller gives an engine ALERT priority. */
bool wk_take_over(const wk_view_t *v, bool anchor_alarm, bool on_clock, uint32_t idle_ms);

/* Should it go away? When nothing is due and no tap is on its way or being confirmed, or the link is gone. */
bool wk_release(const wk_view_t *v);

/* May a tap send I'M UP now? (link, token, timer running, nothing on its way) */
bool wk_tap(const wk_view_t *v);

/* The head line by stage: "WATCH CHECK-IN" / "WATCH CHECK-IN - SECOND CALL" / "WATCHKEEPER NOT CHECKING IN". */
void wk_head(const wk_view_t *v, char *buf, size_t n);

/* The body: minutes since the last I'm up + what comes next. */
void wk_text(const wk_view_t *v, char *buf, size_t n);

/* The footer: what a tap does, 'sending ...', the outcome, or why the box cannot send. */
void wk_footer(const wk_view_t *v, char *buf, size_t n);

/* POST /watch body for I'M UP. */
void wk_post_body(char *buf, size_t n);
