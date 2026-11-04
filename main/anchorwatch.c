/**
 * Purpose: The anchor watch - C port of reference/anchorwatch/{geo,fit,watch}.py (see anchorwatch.h).
 * Version: v2 (2026-10-02) - v2: aw_snapshot / aw_restore (power cut, 10-02)
 * Scope: PRODUCTION - anchor-box firmware, live on the box at the nav station since 2026-10-02 (installed by signed OTA).
 * Architecture: anchor-box firmware, pure C. Mirrors the Python function by function (same names in comments) so a
 *   behaviour change can be traced; any change goes into the Python first (CLAUDE.md). Host test:
 *   test_host/run.bat (scenario vectors from tools/make_c_vectors.py).
 * Invocation: imported by app_main.c; host test links it directly.
 */
#include "anchorwatch.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define RAD(d) ((d) * M_PI / 180.0)
#define DEG(r) ((r) * 180.0 / M_PI)
#define R_EARTH_M 6371008.8
#define FT 0.3048

void aw_default_config(aw_config_t *c)
{
    *c = (aw_config_t){
        .tolerance_m = 15.0, .debounce_s = 20.0,
        .max_speed_mps = 3.0, .jump_margin_m = 10.0, .reject_reset_count = 10,
        .bin_s = 10.0, .fit_window_s = 900.0, .min_fit_bins = 12,
        .lock_min_s = 600.0, .min_arc_deg = 40.0, .max_rms_m = 4.0, .centre_stable_m = 3.0,
        .centre_stable_s = 180.0, .min_radius_m = 15.0, .max_radius_m = 250.0, .min_sagitta_ratio = 3.0,
        .no_lock_after_s = 900.0, .stale_after_s = 30.0,
        .bow_to_antenna_m = 60 * FT,
        .trend_window_s = 720.0, .trend_segments = 3, .trend_min_slope_mps = 0.015, .trend_min_rise_m = 12.0,
    };
}

const char *aw_state_name(aw_state_t s)
{
    static const char *n[] = { "OFF", "ACQUIRING", "LOCKED", "CONSERVATIVE", "ALARM" };
    return n[s];
}

/* ------------------------------------------------------------------ geo.py LocalFrame */

static double pymod(double a, double b) { return a - b * floor(a / b); }
static double wrap_deg(double d) { return pymod(d + 180.0, 360.0) - 180.0; }

static void to_xy(const aw_t *w, double lat, double lon, double *x, double *y)
{
    double k = RAD(1.0) * R_EARTH_M;
    *x = wrap_deg(lon - w->lon0) * w->kx;
    *y = (lat - w->lat0) * k;
}

static void to_latlon(const aw_t *w, double x, double y, double *lat, double *lon)
{
    double k = RAD(1.0) * R_EARTH_M;
    *lon = wrap_deg(w->lon0 + x / w->kx);
    *lat = w->lat0 + y / k;
}

/* ------------------------------------------------------------------ fit.py */

static bool solve3(double a[3][3], const double b[3], double x[3])
{
    double m[3][4];
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) m[i][j] = a[i][j];
        m[i][3] = b[i];
    }
    for (int c = 0; c < 3; c++) {
        int p = c;
        for (int i = c + 1; i < 3; i++)
            if (fabs(m[i][c]) > fabs(m[p][c])) p = i;
        if (fabs(m[p][c]) < 1e-12) return false;
        if (p != c)
            for (int j = 0; j < 4; j++) { double t = m[c][j]; m[c][j] = m[p][j]; m[p][j] = t; }
        for (int i = c + 1; i < 3; i++) {
            double f = m[i][c] / m[c][c];
            for (int j = c; j < 4; j++) m[i][j] -= f * m[c][j];
        }
    }
    for (int i = 2; i >= 0; i--) {
        double s = 0.0;
        for (int j = i + 1; j < 3; j++) s += m[i][j] * x[j];
        x[i] = (m[i][3] - s) / m[i][i];
    }
    return true;
}

static bool kasa(const aw_bin_t *p, int n, double *cx, double *cy, double *r)
{
    if (n < 3) return false;
    double mx = 0, my = 0;
    for (int i = 0; i < n; i++) { mx += p[i].x; my += p[i].y; }
    mx /= n;
    my /= n;
    double ata[3][3] = { { 0 } }, atb[3] = { 0 };
    for (int k = 0; k < n; k++) {
        double x = p[k].x - mx, y = p[k].y - my;
        double row[3] = { x, y, 1.0 };
        double rhs = -(x * x + y * y);
        for (int i = 0; i < 3; i++) {
            atb[i] += row[i] * rhs;
            for (int j = 0; j < 3; j++) ata[i][j] += row[i] * row[j];
        }
    }
    double sol[3];
    if (!solve3(ata, atb, sol)) return false;
    double ccx = -sol[0] / 2.0, ccy = -sol[1] / 2.0;
    double r2 = ccx * ccx + ccy * ccy - sol[2];
    if (r2 <= 0) return false;
    *cx = ccx + mx;
    *cy = ccy + my;
    *r = sqrt(r2);
    return true;
}

/* returns false only when r went <= 0 (the Python returns None -> caller keeps the Kasa values) */
static bool refine(const aw_bin_t *p, int n, double *pcx, double *pcy, double *pr)
{
    double cx = *pcx, cy = *pcy, r = *pr;
    for (int it = 0; it < 5; it++) {
        double jtj[3][3] = { { 0 } }, jtr[3] = { 0 };
        for (int k = 0; k < n; k++) {
            double dx = p[k].x - cx, dy = p[k].y - cy;
            double d = hypot(dx, dy);
            if (d < 1e-9) continue;
            double res = d - r;
            double jr[3] = { -dx / d, -dy / d, -1.0 };
            for (int i = 0; i < 3; i++) {
                jtr[i] += jr[i] * res;
                for (int j = 0; j < 3; j++) jtj[i][j] += jr[i] * jr[j];
            }
        }
        double neg[3] = { -jtr[0], -jtr[1], -jtr[2] }, step[3];
        if (!solve3(jtj, neg, step)) break;
        cx += step[0];
        cy += step[1];
        r += step[2];
        if (r <= 0) return false;
        if (hypot(step[0], step[1]) < 1e-4 && fabs(step[2]) < 1e-4) break;
    }
    *pcx = cx;
    *pcy = cy;
    *pr = r;
    return true;
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static double arc_span_deg(const aw_bin_t *p, int n, double cx, double cy)
{
    if (n < 2) return 0.0;
    double angs[AW_BINS_MAX];
    for (int i = 0; i < n; i++) angs[i] = pymod(DEG(atan2(p[i].y - cy, p[i].x - cx)), 360.0);
    qsort(angs, n, sizeof(double), cmp_double);
    double maxgap = angs[0] + 360.0 - angs[n - 1];
    for (int i = 1; i < n; i++)
        if (angs[i] - angs[i - 1] > maxgap) maxgap = angs[i] - angs[i - 1];
    return 360.0 - maxgap;
}

static bool fit_circle(aw_t *w)
{
    double cx, cy, r;
    if (!kasa(w->bins, w->n_bins, &cx, &cy, &r)) return false;
    double rcx = cx, rcy = cy, rr = r;
    if (refine(w->bins, w->n_bins, &rcx, &rcy, &rr)) { cx = rcx; cy = rcy; r = rr; }
    double ss = 0;
    for (int i = 0; i < w->n_bins; i++) {
        double e = hypot(w->bins[i].x - cx, w->bins[i].y - cy) - r;
        ss += e * e;
    }
    w->fit_cx = cx;
    w->fit_cy = cy;
    w->fit_r = r;
    w->fit_rms = sqrt(ss / w->n_bins);
    w->fit_arc = arc_span_deg(w->bins, w->n_bins, cx, cy);
    w->fit_n = w->n_bins;
    return true;
}

static double sagitta(const aw_t *w)
{
    double span = w->fit_arc < 360.0 ? w->fit_arc : 360.0;
    return w->fit_r * (1.0 - cos(RAD(span) / 2.0));
}

/* ------------------------------------------------------------------ watch.py */

static void reset(aw_t *w)
{
    aw_config_t cfg = w->cfg;
    aw_state_t st = w->state;
    memset(w, 0, sizeof(*w));
    w->cfg = cfg;
    w->state = st;
}

void aw_init(aw_t *w, const aw_config_t *cfg)
{
    memset(w, 0, sizeof(*w));
    if (cfg) w->cfg = *cfg;
    else aw_default_config(&w->cfg);
    w->state = AW_OFF;
}

static int push(aw_event_t *ev, int n, double t, aw_ev_kind_t kind, aw_state_t st, const char *fmt, ...)
{
    if (n >= AW_MAX_EVENTS) return n;
    ev[n].t = t;
    ev[n].kind = kind;
    ev[n].state = st;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ev[n].msg, sizeof(ev[n].msg), fmt, ap);
    va_end(ap);
    return n + 1;
}

static int change(aw_t *w, aw_event_t *ev, int n, double t, aw_state_t s, const char *msg)
{
    w->state = s;
    return push(ev, n, t, AW_EV_STATE, s, "%s", msg);
}

static double guard_radius(const aw_t *w) { return 2 * w->reach + w->cfg.tolerance_m; }

int aw_arm(aw_t *w, double t, double rode_ft, aw_event_t *ev)
{
    reset(w);
    w->armed = true;
    w->armed_at = t;
    char msg[192];
    if (rode_ft > 0) {
        w->have_reach = true;
        w->rode_ft = rode_ft;
        w->reach = rode_ft * FT + w->cfg.bow_to_antenna_m;
        snprintf(msg, sizeof(msg), "armed - observing the swing; rode %.0f ft -> reach %.0f m, coarse guard %.0f m",
                 w->rode_ft, w->reach, guard_radius(w));
    } else {
        snprintf(msg, sizeof(msg), "armed - observing the swing; no rode given - no coarse guard");
    }
    return change(w, ev, 0, t, AW_ACQUIRING, msg);
}

int aw_disarm(aw_t *w, double t, aw_event_t *ev)
{
    if (w->state == AW_OFF) return 0;
    int n = change(w, ev, 0, t, AW_OFF, "disarmed");
    reset(w);
    return n;
}

int aw_tick(aw_t *w, double t, aw_event_t *ev)
{
    if (w->state == AW_OFF || w->stale) return 0;
    double ref = w->have_last ? w->last_t : w->armed_at;
    if (t - ref > w->cfg.stale_after_s) {
        w->stale = true;
        return push(ev, 0, t, AW_EV_WARN, w->state, "GPS stale: no accepted fix for %.0f s - watch is blind", t - ref);
    }
    return 0;
}

static void reset_trend(aw_t *w)
{
    w->n_trend = 0;
    w->tbin_open = false;
    w->drag_suspected = false;
}

static int do_lock(aw_t *w, double t, aw_event_t *ev, int n)
{
    w->cx = w->fit_cx;
    w->cy = w->fit_cy;
    w->radius = w->fit_r;
    w->have_centre = true;
    double seen = 0;
    for (int i = 0; i < w->n_bins; i++) {
        double d = hypot(w->bins[i].x - w->fit_cx, w->bins[i].y - w->fit_cy);
        if (i == 0 || d > seen) seen = d;
    }
    w->alarm_radius = (w->fit_r > seen ? w->fit_r : seen) + w->cfg.tolerance_m;
    w->have_alarm_radius = true;
    w->locked = true;
    w->locked_at = t;
    w->no_lock_reason[0] = 0;
    w->outside = false;
    reset_trend(w);
    w->n_raw = 0;
    w->n_bins = 0;
    w->bin_open = false;
    double lat, lon;
    to_latlon(w, w->cx, w->cy, &lat, &lon);
    char msg[192];
    snprintf(msg, sizeof(msg), "LOCK: anchor %.6f,%.6f radius %.1f m (max seen %.1f m) alarm circle %.1f m (arc %.0f deg, "
             "rms %.1f m)", lat, lon, w->fit_r, seen, w->alarm_radius, w->fit_arc, w->fit_rms);
    return change(w, ev, n, t, AW_LOCKED, msg);
}

static int refit(aw_t *w, double t, aw_event_t *ev, int n)
{
    const aw_config_t *c = &w->cfg;
    int drop = 0;
    while (drop < w->n_bins && w->bins[drop].t < t - c->fit_window_s) drop++;
    if (drop) {
        memmove(w->bins, w->bins + drop, (w->n_bins - drop) * sizeof(aw_bin_t));
        w->n_bins -= drop;
    }
    if (w->n_bins < c->min_fit_bins) {
        snprintf(w->no_lock_reason, sizeof(w->no_lock_reason), "collecting (%d/%d averaged positions)", w->n_bins,
                 c->min_fit_bins);
        return n;
    }
    bool ok = fit_circle(w);
    w->have_fit = ok;
    if (!ok || !(c->min_radius_m <= w->fit_r && w->fit_r <= c->max_radius_m)) {
        w->n_hist = 0;
        if (!ok) snprintf(w->no_lock_reason, sizeof(w->no_lock_reason), "fit degenerate");
        else snprintf(w->no_lock_reason, sizeof(w->no_lock_reason), "radius %.0f m outside %.0f..%.0f m", w->fit_r,
                      c->min_radius_m, c->max_radius_m);
        return n;
    }
    if (w->n_hist == AW_HIST_MAX) {
        memmove(w->hist, w->hist + 1, (AW_HIST_MAX - 1) * sizeof(aw_bin_t));
        w->n_hist--;
    }
    w->hist[w->n_hist++] = (aw_bin_t){ t, w->fit_cx, w->fit_cy };
    drop = 0;
    while (drop < w->n_hist && w->hist[drop].t < t - c->centre_stable_s - 2 * c->bin_s) drop++;
    if (drop) {
        memmove(w->hist, w->hist + drop, (w->n_hist - drop) * sizeof(aw_bin_t));
        w->n_hist -= drop;
    }

    char why[160] = "";
    size_t len = 0;
#define WHY(...) do { if (len) len += snprintf(why + len, sizeof(why) - len, "; "); \
                      if (len < sizeof(why)) len += snprintf(why + len, sizeof(why) - len, __VA_ARGS__); } while (0)
    double elapsed = t - w->armed_at;
    if (elapsed < c->lock_min_s) WHY("observed %.1f/%.0f min", elapsed / 60, c->lock_min_s / 60);
    if (w->fit_arc < c->min_arc_deg) WHY("arc %.0f/%.0f deg", w->fit_arc, c->min_arc_deg);
    if (w->fit_rms > c->max_rms_m) WHY("rms %.1f > %.1f m", w->fit_rms, c->max_rms_m);
    if (sagitta(w) < c->min_sagitta_ratio * w->fit_rms) WHY("curvature not resolved");
    double oldest = w->hist[0].t;
    if (t - oldest < c->centre_stable_s) {
        WHY("centre history %.0f/%.0f s", t - oldest, c->centre_stable_s);
    } else {
        double moved = 0;
        bool any = false;
        for (int i = 0; i < w->n_hist; i++) {
            if (w->hist[i].t < t - c->centre_stable_s) continue;
            double d = hypot(w->fit_cx - w->hist[i].x, w->fit_cy - w->hist[i].y);
            if (!any || d > moved) moved = d;
            any = true;
        }
        if (moved >= c->centre_stable_m) WHY("centre moved %.1f m in %.0f min", moved, c->centre_stable_s / 60);
    }
#undef WHY
    if (len) {
        strncpy(w->no_lock_reason, why, sizeof(w->no_lock_reason) - 1);
        return n;
    }
    return do_lock(w, t, ev, n);
}

static int add_to_bin(aw_t *w, double t, double x, double y, aw_event_t *ev, int n)
{
    long idx = (long)floor((t - w->armed_at) / w->cfg.bin_s);
    if (w->bin_open && w->bin_idx != idx) {
        if (w->n_bins == AW_BINS_MAX) {
            memmove(w->bins, w->bins + 1, (AW_BINS_MAX - 1) * sizeof(aw_bin_t));
            w->n_bins--;
        }
        w->bins[w->n_bins++] = (aw_bin_t){ w->bin_st / w->bin_n, w->bin_sx / w->bin_n, w->bin_sy / w->bin_n };
        w->bin_open = false;
        n = refit(w, t, ev, n);
    }
    if (!w->bin_open) {
        w->bin_open = true;
        w->bin_idx = idx;
        w->bin_st = w->bin_sx = w->bin_sy = 0;
        w->bin_n = 0;
    }
    w->bin_st += t;
    w->bin_sx += x;
    w->bin_sy += y;
    w->bin_n++;
    return n;
}

static int go_conservative(aw_t *w, double t, aw_event_t *ev, int n)
{
    double sx = 0, sy = 0;
    int np = w->n_raw;
    if (np) {
        for (int i = 0; i < np; i++) { sx += w->raw_x[i]; sy += w->raw_y[i]; }
        w->cx = sx / np;
        w->cy = sy / np;
        w->radius = 0;
        for (int i = 0; i < np; i++) {
            double d = hypot(w->raw_x[i] - w->cx, w->raw_y[i] - w->cy);
            if (i == 0 || d > w->radius) w->radius = d;
        }
    } else {
        np = 1;
        w->cx = w->last_x;
        w->cy = w->last_y;
        w->radius = 0;
    }
    w->have_centre = true;
    double floor_m = w->have_reach ? 2 * w->reach : 0.0;
    w->alarm_radius = (w->radius > floor_m ? w->radius : floor_m) + w->cfg.tolerance_m;
    w->have_alarm_radius = true;
    w->outside = false;
    reset_trend(w);
    w->n_raw = 0;
    char msg[192];
    snprintf(msg, sizeof(msg), "no lock - conservative circle: centroid of %d fixes, %s + %.0f m = %.1f m", np,
             floor_m > w->radius ? "rode floor 2 x reach" : "max distance", w->cfg.tolerance_m, w->alarm_radius);
    return change(w, ev, n, t, AW_CONSERVATIVE, msg);
}

static int check_guard(aw_t *w, double t, double x, double y, aw_event_t *ev, int n)
{
    if (w->state != AW_ACQUIRING || !w->have_arm_point) return n;
    double d = hypot(x - w->arm_x, y - w->arm_y);
    double g = guard_radius(w);
    if (d <= g) {
        w->outside = false;
        return n;
    }
    if (!w->outside) {
        w->outside = true;
        w->outside_since = t;
    } else if (t - w->outside_since >= w->cfg.debounce_s) {
        w->have_alarm_at = true;
        w->alarm_at = t;
        char msg[192];
        snprintf(msg, sizeof(msg), "ALARM: coarse guard - %.1f m from where the watch was armed > %.1f m for %.0f s", d,
                 g, t - w->outside_since);
        n = change(w, ev, n, t, AW_ALARM, msg);
    }
    return n;
}

static int check_circle(aw_t *w, double t, double x, double y, aw_event_t *ev, int n)
{
    double rx = w->have_centre ? w->cx : w->arm_x, ry = w->have_centre ? w->cy : w->arm_y;
    w->distance = hypot(x - rx, y - ry);
    w->have_distance = true;
    if (w->state == AW_ALARM) return n;
    if (w->distance <= w->alarm_radius) {
        w->outside = false;
        return n;
    }
    if (!w->outside) {
        w->outside = true;
        w->outside_since = t;
    } else if (t - w->outside_since >= w->cfg.debounce_s) {
        w->have_alarm_at = true;
        w->alarm_at = t;
        char msg[192];
        snprintf(msg, sizeof(msg), "ALARM: %.1f m from the %s > %.1f m for %.0f s", w->distance,
                 w->locked ? "anchor" : "conservative centre", w->alarm_radius, t - w->outside_since);
        n = change(w, ev, n, t, AW_ALARM, msg);
    }
    return n;
}

static double slope(const aw_tpt_t *p, int n)
{
    double mt = 0, md = 0;
    for (int i = 0; i < n; i++) { mt += p[i].t; md += p[i].d; }
    mt /= n;
    md /= n;
    double stt = 0, std_ = 0;
    for (int i = 0; i < n; i++) stt += (p[i].t - mt) * (p[i].t - mt);
    for (int i = 0; i < n; i++) std_ += (p[i].t - mt) * (p[i].d - md);
    return stt > 0 ? std_ / stt : 0.0;
}

static int eval_trend(aw_t *w, double t, aw_event_t *ev, int n)
{
    const aw_config_t *c = &w->cfg;
    int drop = 0;
    while (drop < w->n_trend && w->trend[drop].t < t - c->trend_window_s) drop++;
    if (drop) {
        memmove(w->trend, w->trend + drop, (w->n_trend - drop) * sizeof(aw_tpt_t));
        w->n_trend -= drop;
    }
    if (w->state == AW_ALARM || !w->n_trend || t - w->trend[0].t < c->trend_window_s - 2 * c->bin_s) return n;
    if (w->n_trend < 0.6 * c->trend_window_s / c->bin_s) return n;
    double seg_s = c->trend_window_s / c->trend_segments;
    double sslopes[8], minsl = 0;
    for (int s = 0; s < c->trend_segments && s < 8; s++) {
        int k = c->trend_segments - 1 - s;          /* oldest third first */
        aw_tpt_t seg[AW_TREND_MAX];
        int ns = 0;
        for (int i = 0; i < w->n_trend; i++)
            if (t - (k + 1) * seg_s <= w->trend[i].t && w->trend[i].t < t - k * seg_s) seg[ns++] = w->trend[i];
        if (ns < 4) return n;
        sslopes[s] = slope(seg, ns);
        if (s == 0 || sslopes[s] < minsl) minsl = sslopes[s];
    }
    double sl = slope(w->trend, w->n_trend);
    double rslope = sslopes[c->trend_segments - 1];
    double rise = sl * c->trend_window_s;
    if (!w->drag_suspected) {
        if (sl >= c->trend_min_slope_mps && rise >= c->trend_min_rise_m && minsl >= 0.5 * c->trend_min_slope_mps) {
            w->drag_suspected = true;
            n = push(ev, n, t, AW_EV_DRAG, w->state, "DRAG? distance from the %s has risen %.0f m in %.0f min "
                     "(%.1f m/min) and is not levelling off; now %.0f m of %.0f m",
                     w->locked ? "anchor" : "conservative centre", rise, c->trend_window_s / 60, sl * 60, w->distance,
                     w->alarm_radius);
        }
    } else if (sl <= 0.25 * c->trend_min_slope_mps && rslope <= 0.5 * c->trend_min_slope_mps) {
        w->drag_suspected = false;
        n = push(ev, n, t, AW_EV_INFO, w->state, "DRAG? cleared - distance levelled off at %.0f m", w->distance);
    }
    return n;
}

static int add_trend(aw_t *w, double t, aw_event_t *ev, int n)
{
    if (!w->have_centre) return n;
    long idx = (long)floor((t - w->armed_at) / w->cfg.bin_s);
    if (w->tbin_open && w->tbin_idx != idx) {
        if (w->n_trend == AW_TREND_MAX) {
            memmove(w->trend, w->trend + 1, (AW_TREND_MAX - 1) * sizeof(aw_tpt_t));
            w->n_trend--;
        }
        w->trend[w->n_trend++] = (aw_tpt_t){ w->tbin_st / w->tbin_n, w->tbin_sd / w->tbin_n };
        w->tbin_open = false;
        n = eval_trend(w, t, ev, n);
    }
    if (!w->tbin_open) {
        w->tbin_open = true;
        w->tbin_idx = idx;
        w->tbin_st = w->tbin_sd = 0;
        w->tbin_n = 0;
    }
    w->tbin_st += t;
    w->tbin_sd += w->distance;
    w->tbin_n++;
    return n;
}

int aw_update(aw_t *w, double t, double lat, double lon, aw_event_t *ev)
{
    if (w->state == AW_OFF) return 0;
    const aw_config_t *c = &w->cfg;
    int n = 0;
    if (!w->have_frame) {
        w->have_frame = true;
        w->lat0 = lat;
        w->lon0 = lon;
        w->kx = RAD(1.0) * R_EARTH_M * cos(RAD(lat));
    }
    double x, y;
    to_xy(w, lat, lon, &x, &y);

    if (w->have_last) {
        if (t <= w->last_t) {
            w->rej_duplicate++;
            return n;
        }
        double dt = t - w->last_t;
        if (hypot(x - w->last_x, y - w->last_y) > c->max_speed_mps * dt + c->jump_margin_m) {
            w->jump_streak++;
            if (w->jump_streak < c->reject_reset_count) {
                w->rej_jump++;
                return n;
            }
            w->rej_filter_reset++;
            n = push(ev, n, t, AW_EV_WARN, w->state, "%d jumps in a row - accepting the new position", w->jump_streak);
        }
    }
    w->jump_streak = 0;
    if (!w->have_arm_point) {
        w->have_arm_point = true;
        w->arm_x = x;
        w->arm_y = y;
    }
    bool repeat = w->have_last && lat == w->last_lat && lon == w->last_lon;
    w->have_last = true;
    w->last_t = t;
    w->last_x = x;
    w->last_y = y;
    w->last_lat = lat;
    w->last_lon = lon;
    w->accepted++;
    if (w->stale) {
        w->stale = false;
        n = push(ev, n, t, AW_EV_INFO, w->state, "GPS fixes resumed");
    }

    if (repeat) {
        w->rej_repeat++;
    } else if (w->state == AW_ACQUIRING || w->state == AW_CONSERVATIVE) {
        if (w->state == AW_ACQUIRING && w->n_raw < AW_RAW_MAX) {
            w->raw_x[w->n_raw] = x;
            w->raw_y[w->n_raw] = y;
            w->n_raw++;
        }
        n = add_to_bin(w, t, x, y, ev, n);
    }

    double elapsed = t - w->armed_at;
    if (w->state == AW_ACQUIRING && elapsed >= c->no_lock_after_s) n = go_conservative(w, t, ev, n);
    if (w->state == AW_LOCKED || w->state == AW_CONSERVATIVE || w->state == AW_ALARM) {
        n = check_circle(w, t, x, y, ev, n);
        n = add_trend(w, t, ev, n);
    } else {
        if (w->have_fit) {
            w->distance = hypot(x - w->fit_cx, y - w->fit_cy);
            w->have_distance = true;
        }
        if (w->have_reach) n = check_guard(w, t, x, y, ev, n);
    }
    return n;
}

bool aw_centre_latlon(const aw_t *w, double *lat, double *lon, double *radius)
{
    if (!w->have_frame) return false;
    if (w->have_centre) {
        to_latlon(w, w->cx, w->cy, lat, lon);
        *radius = w->radius;
        return true;
    }
    if (w->have_fit) {
        to_latlon(w, w->fit_cx, w->fit_cy, lat, lon);
        *radius = w->fit_r;
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ power cut (watch.py snapshot / restore) */

bool aw_snapshot(const aw_t *w, aw_snapshot_t *s)
{
    if (!(w->state == AW_LOCKED || w->state == AW_CONSERVATIVE || w->state == AW_ALARM) || !w->have_frame) return false;
    memset(s, 0, sizeof(*s));
    s->version = AW_SNAP_VERSION;
    s->state = (uint8_t)w->state;
    s->has_centre = w->have_centre;
    s->locked = w->locked;
    double x = w->have_centre ? w->cx : w->arm_x, y = w->have_centre ? w->cy : w->arm_y;
    to_latlon(w, x, y, &s->lat, &s->lon);
    s->radius = w->have_centre ? w->radius : 0.0;
    s->alarm_radius = w->have_alarm_radius ? w->alarm_radius : 0.0;
    s->rode_ft = w->have_reach ? w->rode_ft : 0.0;
    return true;
}

int aw_restore(aw_t *w, double t, const aw_snapshot_t *s, aw_event_t *ev)
{
    reset(w);
    w->armed = true;
    w->armed_at = t;
    if (s->rode_ft > 0) {
        w->have_reach = true;
        w->rode_ft = s->rode_ft;
        w->reach = s->rode_ft * FT + w->cfg.bow_to_antenna_m;
    }
    w->have_frame = true;
    w->lat0 = s->lat;
    w->lon0 = s->lon;
    w->kx = RAD(1.0) * R_EARTH_M * cos(RAD(s->lat));
    w->have_arm_point = true;
    w->arm_x = w->arm_y = 0.0;
    if (s->has_centre) {
        w->have_centre = true;
        w->cx = w->cy = 0.0;
        w->radius = s->radius;
        w->have_alarm_radius = true;
        w->alarm_radius = s->alarm_radius;
    }
    if (s->locked) {
        w->locked = true;
        w->locked_at = t;
    }
    if (s->state == AW_ALARM) {
        w->have_alarm_at = true;
        w->alarm_at = t;
    }
    w->state = AW_OFF;
    char msg[192];
    snprintf(msg, sizeof(msg), "RESTORED after power-up: %s, circle %.1f m round %.6f,%.6f", aw_state_name(s->state),
             s->alarm_radius, s->lat, s->lon);
    return change(w, ev, 0, t, (aw_state_t)s->state, msg);
}
