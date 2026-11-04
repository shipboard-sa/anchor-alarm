/**
 * Purpose: 'Monitoring host down' decisions (ANC-25, Ship's Automation OQ-117 1b + 1c): from plain observations, is the
 *   Pi (SignalK over Wi-Fi) or the Cerbo (its N2K frames) down, is DO1 (panel AL-13 'Automation') asserted, and
 *   what the box screen says. Pure C - no ESP-IDF, no I/O, no clock.
 * Version: v1 (2026-10-03)
 * Scope: PRODUCTION - anchor-box firmware (live on the box at the nav station, installed by signed OTA).
 * Architecture: anchor-box firmware, logic layer. Port of reference/anchorwatch/hostwatch.py (the spec - same
 *   states, events and texts); checked by test_host/test_hostwatch.c. Fed by hostwatch_ctl.c (HTTP poll, router
 *   ping, N2K bookkeeping), which also drives DO1 from hw_do1().
 * Invocation: imported by hostwatch_ctl.c and test_host/test_hostwatch.c.
 */
#pragma once

#include <stdbool.h>

typedef enum { HW_OK = 0, HW_UNKNOWN = 1, HW_DEGRADED = 2, HW_DOWN = 3, HW_NET_DOWN = 4 } hw_state_t;

typedef enum {
    HW_EV_NONE = 0,
    HW_EV_PI_DOWN, HW_EV_PI_NET_DOWN, HW_EV_PI_NET_OK, HW_EV_PI_RELEASED,
    HW_EV_CERBO_DOWN, HW_EV_CERBO_BUS_SILENT, HW_EV_CERBO_BUS_OK, HW_EV_CERBO_RELEASED,
} hw_event_t;

typedef struct {
    double pi_down_after_s;     /* 60: unhealthy this long -> router check -> assert */
    double cerbo_silent_s;      /* 180: no Cerbo frame this long -> assert */
    double release_s;           /* 30: healthy this long -> release */
    double bus_quiet_s;         /* 10: nothing from ANY other source this long = bus silent, not the Cerbo */
} hw_config_t;

typedef struct {
    hw_config_t cfg;
    hw_state_t state;
    bool asserted;
    bool have_bad, have_good;
    double bad_since, good_since;
} hw_pi_t;

typedef struct {
    hw_config_t cfg;
    hw_state_t state;
    bool asserted;
    bool have_known, have_recovered;
    double known_since, recovered_since;
} hw_cerbo_t;

void hw_config_default(hw_config_t *c);
void hw_pi_init(hw_pi_t *w, const hw_config_t *c);
void hw_cerbo_init(hw_cerbo_t *w, const hw_config_t *c);

bool hw_pi_wants_router(const hw_pi_t *w, double t);
/* router: -1 = not checked, 0 = silent, 1 = answered. Returns the event (at most one per call). */
hw_event_t hw_pi_update(hw_pi_t *w, double t, bool pi_ok, int router);
/* last_cerbo_t / last_other_t < 0 = never heard */
hw_event_t hw_cerbo_update(hw_cerbo_t *w, double t, bool known, double last_cerbo_t, double last_other_t);

bool hw_do1(const hw_pi_t *p, const hw_cerbo_t *c);
const char *hw_pi_text(const hw_pi_t *p);         /* NULL = nothing to show */
const char *hw_cerbo_text(const hw_cerbo_t *c);
const char *hw_state_name(hw_state_t s);
const char *hw_event_name(hw_event_t e);

/* ---- SignalK reply parsing (no JSON library; the replies are small and fixed in shape) ---- */

/* "2026-10-03T14:13:12.345Z" -> ms since 1970 UTC; false if it is not that shape */
bool hw_parse_iso_ms(const char *s, long long *ms);

typedef struct {
    bool have_ts;          /* at least one "timestamp" */
    long long ts_ms;       /* the NEWEST "timestamp" in the reply (a path can carry one per source) */
    bool have_bool;        /* at least one boolean "value" */
    bool all_true;         /* every boolean "value" is true */
} hw_sk_reply_t;

/* GET /signalk/v1/api/vessels/self/<path> reply: {"value": ..., "$source": ..., "timestamp": "...", ...} */
void hw_parse_signalk(const char *body, hw_sk_reply_t *out);
