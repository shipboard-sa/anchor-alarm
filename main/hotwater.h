/**
 * Purpose: HOT WATER tap on the box (ANC-43; Ship's Automation D-135, engine GET / POST /hot-water) - pure logic:
 *   parse the rules engine's /hot-water reply (and its 409 refusal), and decide what WATCH's HOT WATER button and
 *   notice line show and what a tap may send. Pure C - no ESP-IDF, no I/O.
 * Version: v1 (2026-10-11)
 * Scope: PRODUCTION - anchor-box firmware (live on the box at the nav station, installed by signed OTA).
 * Architecture: anchor-box firmware, logic layer beside fill.c. The STATE lives in the engine (Pi, :8200 -
 *   builtins/hot_water.py): it switches the heater (WF-307 -> S6-R4) and ends the run by itself (tank hot, no draw,
 *   20 min); the box is a remote + display and only ever shows what the engine last reported. Fed by
 *   hotwater_ctl.c (engine link); drawn by ui_watch.c; checked by test_host/test_hotwater.c.
 * Invocation: imported by hotwater_ctl.c, ui_watch.c and test_host/test_hotwater.c.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef enum { HW_OFF = 0, HW_HEATING, HW_ON_NO_DRAW, HW_HELD } hw_state_t;
typedef enum { HW_LAST_NONE = 0, HW_LAST_HOT, HW_LAST_NO_DRAW, HW_LAST_TIME, HW_LAST_STOPPED } hw_last_t;

/* the engine's reply: {on, state, remaining_s, since, by, amps, soc, soc_floor, max_s,
 *                      last: null | {why, result, at, by, minutes, age_s}} */
typedef struct {
    bool on;
    hw_state_t state;
    int remaining_s;
    bool has_amps;
    float amps;
    bool has_soc;
    float soc;
    float soc_floor;
    hw_last_t last;               /* how the last run ended */
    int last_age_s;               /* ... this long ago (-1 = unknown) */
} hw_reply_t;

/* true when body is an engine reply (an "on" true/false and a known "state" are required) */
bool hw_parse(const char *body, hw_reply_t *out);

/* the 409 body {"detail": "house bank 46 % - below the 50 % floor ..."} -> ASCII text cut to n-1; false if absent */
bool hw_detail(const char *body, char *buf, size_t n);

/* what the box knows, assembled by hotwater_ctl.c */
typedef struct {
    bool wifi;                    /* Wi-Fi configured on this box */
    bool polled;                  /* at least one poll has finished */
    bool link;                    /* the engine answered recently with a valid reply */
    bool token;                   /* the box token is built in */
    int get_http;                 /* last GET status (0 = no answer, -1 = none yet) */
    bool pending;                 /* a tap is being sent */
    bool pending_on;              /* ... start (true) or stop */
    char msg[80];                 /* last tap's outcome worth saying (refusal / error), "" = none (shown a while) */
    int reply_age_s;              /* seconds since eng was received (counts the run's time down between polls) */
    hw_reply_t eng;               /* the engine's last reply (valid while link) */
} hw_view_t;

#define HW_READY_S (30 * 60)      /* 'READY' on the button this long after a run ended because the tank was hot */
#define HW_SAY_END_S (10 * 60)    /* the notice line says how the last run ended for this long */

/* WATCH's button face: two short lines ("HOT" / "WATER", "HOT" / "14 min", "HOT" / "READY", "HOT" / "..."), whether
 * it is drawn heavier (a run is on) and whether it is enabled at all (Wi-Fi + token). */
typedef struct {
    char top[8];
    char bottom[12];
    bool active;
    bool enabled;
} hw_button_t;

void hw_button(const hw_view_t *v, hw_button_t *b);

/* May a tap go now, and what does it ask? false = nothing is sent (no link / token, a tap already on its way).
 * Off -> start; on -> stop (a second tap ends the run early). */
bool hw_tap(const hw_view_t *v, bool *on);

/* WATCH's notice line while a run is on or just after a tap ("" = nothing to say), e.g.
 * "HOT WATER heating - 14 min left, ends when the tank is hot" / the engine's refusal. */
void hw_notice(const hw_view_t *v, char *buf, size_t n);
