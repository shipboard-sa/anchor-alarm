/**
 * Purpose: 'Monitoring host down' decisions - see hostwatch.h (ANC-25).
 * Version: v1 (2026-10-03)
 * Scope: PRODUCTION - anchor-box firmware (live on the box at the nav station, installed by signed OTA).
 * Architecture: anchor-box firmware, logic layer; line-for-line port of reference/anchorwatch/hostwatch.py.
 *   A lamp is released ONLY after its device has been healthy for release_s - never because the attribution
 *   (router / bus) changed while it is still down.
 * Invocation: imported by hostwatch_ctl.c and test_host/test_hostwatch.c.
 */
#include "hostwatch.h"

#include <stddef.h>
#include <string.h>

void hw_config_default(hw_config_t *c)
{
    c->pi_down_after_s = 60;
    c->cerbo_silent_s = 180;
    c->release_s = 30;
    c->bus_quiet_s = 10;
}

void hw_pi_init(hw_pi_t *w, const hw_config_t *c)
{
    *w = (hw_pi_t){ .cfg = *c, .state = HW_UNKNOWN };
}

void hw_cerbo_init(hw_cerbo_t *w, const hw_config_t *c)
{
    *w = (hw_cerbo_t){ .cfg = *c, .state = HW_UNKNOWN };
}

bool hw_pi_wants_router(const hw_pi_t *w, double t)
{
    return w->have_bad && t - w->bad_since >= w->cfg.pi_down_after_s;
}

hw_event_t hw_pi_update(hw_pi_t *w, double t, bool pi_ok, int router)
{
    if (pi_ok) {
        w->have_bad = false;
        if (w->asserted) {
            if (!w->have_good) {
                w->have_good = true;
                w->good_since = t;
            }
            if (t - w->good_since >= w->cfg.release_s) {
                w->asserted = false;
                w->have_good = false;
                w->state = HW_OK;
                return HW_EV_PI_RELEASED;
            }
            w->state = HW_DOWN;                 /* still lit, recovering */
            return HW_EV_NONE;
        }
        hw_event_t e = w->state == HW_NET_DOWN ? HW_EV_PI_NET_OK : HW_EV_NONE;
        w->state = HW_OK;
        return e;
    }
    w->have_good = false;
    if (!w->have_bad) {
        w->have_bad = true;
        w->bad_since = t;
    }
    if (w->asserted) {
        w->state = HW_DOWN;
        return HW_EV_NONE;
    }
    if (!hw_pi_wants_router(w, t)) {
        w->state = HW_DEGRADED;
        return HW_EV_NONE;
    }
    if (router < 0) {                           /* not checked: hold the current view */
        if (w->state != HW_NET_DOWN) w->state = HW_DEGRADED;
        return HW_EV_NONE;
    }
    if (router) {
        w->asserted = true;
        w->state = HW_DOWN;
        return HW_EV_PI_DOWN;
    }
    if (w->state != HW_NET_DOWN) {
        w->state = HW_NET_DOWN;
        return HW_EV_PI_NET_DOWN;
    }
    return HW_EV_NONE;
}

hw_event_t hw_cerbo_update(hw_cerbo_t *w, double t, bool known, double last_cerbo_t, double last_other_t)
{
    if (!known) {
        w->have_known = false;
        if (!w->asserted) w->state = HW_UNKNOWN;
        return HW_EV_NONE;
    }
    if (!w->have_known) {
        w->have_known = true;
        w->known_since = t;
    }
    double ref = last_cerbo_t < 0 ? w->known_since
                                  : (last_cerbo_t > w->known_since ? last_cerbo_t : w->known_since);
    bool silent = t - ref >= w->cfg.cerbo_silent_s;
    if (!silent) {
        if (w->asserted) {
            if (!w->have_recovered) {
                double first = last_cerbo_t >= 0 ? last_cerbo_t : t;
                w->have_recovered = true;
                w->recovered_since = first > w->known_since ? first : w->known_since;
            }
            if (t - w->recovered_since >= w->cfg.release_s) {
                w->asserted = false;
                w->have_recovered = false;
                w->state = HW_OK;
                return HW_EV_CERBO_RELEASED;
            }
            w->state = HW_DOWN;
            return HW_EV_NONE;
        }
        hw_event_t e = w->state == HW_NET_DOWN ? HW_EV_CERBO_BUS_OK : HW_EV_NONE;
        w->state = HW_OK;
        return e;
    }
    w->have_recovered = false;
    if (w->asserted) {
        w->state = HW_DOWN;
        return HW_EV_NONE;
    }
    bool bus_alive = last_other_t >= 0 && t - last_other_t < w->cfg.bus_quiet_s;
    if (bus_alive) {
        w->asserted = true;
        w->state = HW_DOWN;
        return HW_EV_CERBO_DOWN;
    }
    if (w->state != HW_NET_DOWN) {
        w->state = HW_NET_DOWN;
        return HW_EV_CERBO_BUS_SILENT;
    }
    return HW_EV_NONE;
}

bool hw_do1(const hw_pi_t *p, const hw_cerbo_t *c)
{
    return p->asserted || c->asserted;
}

const char *hw_pi_text(const hw_pi_t *p)
{
    if (p->state == HW_DOWN) return "Pi / SignalK not answering";
    if (p->state == HW_NET_DOWN) return "Network down - router not answering";
    return NULL;
}

const char *hw_cerbo_text(const hw_cerbo_t *c)
{
    if (c->state == HW_DOWN) return "Cerbo silent on N2K";
    if (c->state == HW_NET_DOWN) return "N2K bus silent";
    return NULL;
}

const char *hw_state_name(hw_state_t s)
{
    static const char *N[] = { "OK", "UNKNOWN", "DEGRADED", "DOWN", "NET_DOWN" };
    return (unsigned)s < 5 ? N[s] : "?";
}

const char *hw_event_name(hw_event_t e)
{
    static const char *N[] = { "", "PI_DOWN", "PI_NET_DOWN", "PI_NET_OK", "PI_RELEASED",
                               "CERBO_DOWN", "CERBO_BUS_SILENT", "CERBO_BUS_OK", "CERBO_RELEASED" };
    return (unsigned)e < 9 ? N[e] : "?";
}

/* ---- SignalK reply parsing ---- */

/* days from 1970-01-01 for y/m/d (H. Hinnant's days_from_civil, public domain) */
static long long days_from_civil(long long y, unsigned m, unsigned d)
{
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
}

static bool digits(const char *s, int n, int *out)
{
    int v = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10 + (s[i] - '0');
    }
    *out = v;
    return true;
}

bool hw_parse_iso_ms(const char *s, long long *ms)
{
    int y, mo, d, h, mi, se;
    if (!digits(s, 4, &y) || s[4] != '-' || !digits(s + 5, 2, &mo) || s[7] != '-' || !digits(s + 8, 2, &d) ||
        s[10] != 'T' || !digits(s + 11, 2, &h) || s[13] != ':' || !digits(s + 14, 2, &mi) || s[16] != ':' ||
        !digits(s + 17, 2, &se))
        return false;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || se > 60) return false;
    const char *p = s + 19;
    int frac = 0, scale = 1000;
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9') {
            if (scale > 1) {
                scale /= 10;
                frac += (*p - '0') * scale;
            }
            p++;
        }
    }
    if (*p != 'Z') return false;          /* SignalK timestamps are UTC with 'Z' */
    *ms = ((days_from_civil(y, mo, d) * 24 + h) * 60 + mi) * 60000LL + se * 1000LL + frac;
    return true;
}

static const char *skip_ws_colon(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ':') p++;
    return p;
}

void hw_parse_signalk(const char *body, hw_sk_reply_t *out)
{
    *out = (hw_sk_reply_t){ .all_true = true };
    for (const char *p = body; (p = strstr(p, "\"timestamp\"")) != NULL;) {
        p = skip_ws_colon(p + 11);
        long long ms;
        if (*p == '"' && hw_parse_iso_ms(p + 1, &ms) && (!out->have_ts || ms > out->ts_ms)) {
            out->have_ts = true;
            out->ts_ms = ms;
        }
    }
    for (const char *p = body; (p = strstr(p, "\"value\"")) != NULL;) {   /* "values" does not match */
        p = skip_ws_colon(p + 7);
        if (strncmp(p, "true", 4) == 0) {
            out->have_bool = true;
        } else if (strncmp(p, "false", 5) == 0) {
            out->have_bool = true;
            out->all_true = false;
        }
    }
    if (!out->have_bool) out->all_true = false;
}
