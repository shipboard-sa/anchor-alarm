/**
 * Purpose: Boat time for the anchor box CLOCK - C port of reference/anchorwatch/boattime.py (see boattime.h).
 * Version: v1 (2026-10-02)
 * Scope: PRODUCTION - anchor-box firmware, live on the box at the nav station since 2026-10-02 (installed by signed OTA).
 * Architecture: anchor-box firmware, pure C. Zone table and rules mirror the Python model line for line; any
 *   behaviour change goes into the Python first (CLAUDE.md). Calendar = H. Hinnant's public-domain algorithms;
 *   sun = NOAA Solar Calculator method (public domain).
 * Invocation: compiled into the firmware (boatclock.c, n2k.c use it); host test: test_host/run.bat.
 */
#include "boattime.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define RAD(d) ((d) * M_PI / 180.0)
#define DEG(r) ((r) * 180.0 / M_PI)

/* ------------------------------------------------------------------ calendar */

static int64_t fdiv(int64_t a, int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }

int64_t bt_days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = fdiv(y, 400);
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void bt_civil_from_days(int64_t z, int *y, int *m, int *d)
{
    z += 719468;
    int64_t era = fdiv(z, 146097);
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yoe + era * 400) + (*m <= 2);
}

static int64_t nth_sunday(int y, int m, int n)
{
    int64_t first = bt_days_from_civil(y, m, 1);
    int64_t wd = ((first + 4) % 7 + 7) % 7;      /* 1970-01-01 = Thursday; 0 = Sunday */
    return first + (7 - wd) % 7 + 7 * (n - 1);
}

/* ------------------------------------------------------------------ zones */

typedef struct { bt_zone_t zone; double lat_min, lat_max, lon_min, lon_max; } area_t;

static const bt_zone_t US_CENTRAL = { "CT", -360, true };
static const bt_zone_t US_EASTERN = { "ET", -300, true };
static const bt_zone_t BAHAMAS    = { "BS", -300, true };
static const bt_zone_t ATLANTIC   = { "AT", -240, false };

/* First match wins - same boxes as reference AREAS (wiki/Time-and-Zones.md). */
static const area_t AREAS[] = {
    { { "ET", -300, true },  24.0, 31.0, -85.39, -79.6 },
    { { "CT", -360, true },  25.0, 31.5, -97.6,  -85.39 },
    { { "BS", -300, true },  20.9, 27.5, -80.6,  -71.0 },
    { { "AT", -240, false }, 13.0, 20.5, -71.6,  -59.0 },
    { { "AT", -240, false }, 10.0, 13.0, -71.0,  -59.0 },
};

bt_zone_t bt_nautical_zone(double lon)
{
    int h = (int)floor(lon / 15.0 + 0.5);
    if (h < -12) h = -12;
    if (h > 12) h = 12;
    bt_zone_t z = { "", (int16_t)(h * 60), false };
    if (h == 0) strcpy(z.zid, "Z");
    else snprintf(z.zid, sizeof(z.zid), "N%+d", h);
    return z;
}

bt_zone_t bt_zone_at(double lat, double lon)
{
    for (size_t i = 0; i < sizeof(AREAS) / sizeof(AREAS[0]); i++) {
        const area_t *a = &AREAS[i];
        if (lat >= a->lat_min && lat < a->lat_max && lon >= a->lon_min && lon < a->lon_max) return a->zone;
    }
    return bt_nautical_zone(lon);
}

bool bt_zone_by_id(const char *zid, bt_zone_t *out)
{
    const bt_zone_t *named[] = { &US_CENTRAL, &US_EASTERN, &BAHAMAS, &ATLANTIC };
    for (size_t i = 0; i < 4; i++)
        if (strcmp(zid, named[i]->zid) == 0) { *out = *named[i]; return true; }
    if (strcmp(zid, "Z") == 0) { *out = bt_nautical_zone(0.0); return true; }
    if (zid[0] == 'N') {
        char *end;
        long h = strtol(zid + 1, &end, 10);
        if (*end || h < -12 || h > 12 || h == 0) return false;
        *out = bt_nautical_zone(h * 15.0);
        return true;
    }
    return false;
}

static bool us_dst_active(int64_t utc_s, int std_min)
{
    int y, m, d;
    bt_civil_from_days(fdiv(utc_s, 86400), &y, &m, &d);
    int64_t start = nth_sunday(y, 3, 2) * 86400 + 2 * 3600 - (int64_t)std_min * 60;
    int64_t end = nth_sunday(y, 11, 1) * 86400 + 2 * 3600 - (int64_t)(std_min + 60) * 60;
    return utc_s >= start && utc_s < end;
}

int bt_offset_min(const bt_zone_t *z, int64_t utc_s)
{
    if (z->us_dst && us_dst_active(utc_s, z->std_min)) return z->std_min + 60;
    return z->std_min;
}

/* ------------------------------------------------------------------ zone choice */

void bt_chooser_init(bt_chooser_t *c) { memset(c, 0, sizeof(*c)); }

void bt_chooser_pin(bt_chooser_t *c, const bt_zone_t *z)
{
    c->pinned = z != NULL;
    if (z) c->pin = *z;
}

const bt_zone_t *bt_chooser_zone(const bt_chooser_t *c)
{
    if (c->pinned) return &c->pin;
    return c->have_current ? &c->current : NULL;
}

const bt_zone_t *bt_chooser_update(bt_chooser_t *c, int64_t t_s, bool have_pos, double lat, double lon)
{
    if (have_pos) {
        bt_zone_t z = bt_zone_at(lat, lon);
        if (!c->have_current) {
            c->current = z;
            c->have_current = true;
        } else if (strcmp(z.zid, c->current.zid) == 0) {
            c->have_cand = false;
        } else if (!c->have_cand || strcmp(c->cand.zid, z.zid) != 0) {
            c->cand = z;
            c->cand_since = t_s;
            c->have_cand = true;
        } else if (t_s - c->cand_since >= BT_ZONE_STEADY_S) {
            c->current = z;
            c->have_cand = false;
        }
    }
    return bt_chooser_zone(c);
}

/* ------------------------------------------------------------------ GPS time acceptance */

#define MIN_VALID_UTC_MS (20454LL * 86400000LL)   /* 2026-01-01 */

void bt_sync_init(bt_sync_t *s) { memset(s, 0, sizeof(*s)); }

int64_t bt_sync_now_ms(const bt_sync_t *s, int64_t mono_ms) { return s->utc_ms + (mono_ms - s->mono_ms); }

void bt_sync_feed(bt_sync_t *s, int64_t utc_ms, int64_t mono_ms, int kind)
{
    if (kind != 0 || utc_ms < MIN_VALID_UTC_MS) return;
    bool had_prev = s->have_last;
    int64_t pu = s->last_utc_ms, pm = s->last_mono_ms;
    s->last_utc_ms = utc_ms;
    s->last_mono_ms = mono_ms;
    s->have_last = true;
    if (s->valid && llabs(bt_sync_now_ms(s, mono_ms) - utc_ms) <= BT_AGREE_MS) {
        s->utc_ms = utc_ms;
        s->mono_ms = mono_ms;
        return;
    }
    if (had_prev && llabs(pu + (mono_ms - pm) - utc_ms) <= BT_AGREE_MS) {
        s->utc_ms = utc_ms;
        s->mono_ms = mono_ms;
        s->valid = true;
    }
}

/* ------------------------------------------------------------------ sun */

double bt_sun_altitude_deg(int64_t utc_s, double lat, double lon)
{
    double jd = (double)utc_s / 86400.0 + 2440587.5;
    double t = (jd - 2451545.0) / 36525.0;
    double l0 = fmod(280.46646 + t * (36000.76983 + t * 0.0003032), 360.0);
    if (l0 < 0) l0 += 360.0;
    double m = RAD(357.52911 + t * (35999.05029 - 0.0001537 * t));
    double e = 0.016708634 - t * (0.000042037 + 0.0000001267 * t);
    double c = sin(m) * (1.914602 - t * (0.004817 + 0.000014 * t)) + sin(2 * m) * (0.019993 - 0.000101 * t) +
               sin(3 * m) * 0.000289;
    double om = RAD(125.04 - 1934.136 * t);
    double app = RAD(l0 + c - 0.00569 - 0.00478 * sin(om));
    double eps0 = 23.0 + (26.0 + (21.448 - t * (46.815 + t * (0.00059 - t * 0.001813))) / 60.0) / 60.0;
    double eps = RAD(eps0 + 0.00256 * cos(om));
    double decl = asin(sin(eps) * sin(app));
    double y = tan(eps / 2.0);
    y *= y;
    double l0r = RAD(l0);
    double eqt = 4.0 * DEG(y * sin(2 * l0r) - 2 * e * sin(m) + 4 * e * y * sin(m) * cos(2 * l0r) -
                           0.5 * y * y * sin(4 * l0r) - 1.25 * e * e * sin(2 * m));
    int64_t sod = utc_s - fdiv(utc_s, 86400) * 86400;
    double tst = (double)sod / 60.0 + eqt + 4.0 * lon;
    double ha = RAD(tst / 4.0 - 180.0);
    double la = RAD(lat);
    double cz = sin(la) * sin(decl) + cos(la) * cos(decl) * cos(ha);
    if (cz > 1.0) cz = 1.0;
    if (cz < -1.0) cz = -1.0;
    return 90.0 - DEG(acos(cz));
}

bool bt_is_night(int64_t utc_s, double lat, double lon)
{
    return bt_sun_altitude_deg(utc_s, lat, lon) < BT_NIGHT_ALT_DEG;
}

/* ------------------------------------------------------------------ clock face */

bt_face_t bt_clock_face(const bt_sync_t *s, const bt_chooser_t *c, int64_t mono_ms, bool have_pos, double lat,
                        double lon)
{
    const bt_zone_t *z = bt_chooser_zone(c);
    bt_face_t f = { -1, 0, c->pinned, false };
    if (!s->valid || !z) return f;
    int64_t utc_s = fdiv(bt_sync_now_ms(s, mono_ms), 1000);
    int64_t local = utc_s + (int64_t)bt_offset_min(z, utc_s) * 60;
    int64_t sod = local - fdiv(local, 86400) * 86400;
    f.hh = (int)(sod / 3600);
    f.mm = (int)(sod / 60 % 60);
    f.night = have_pos && bt_is_night(utc_s, lat, lon);
    return f;
}
