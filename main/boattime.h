/**
 * Purpose: Boat time for the anchor box CLOCK (SPEC 3, ANC-07 / ANC-08) - C port of reference/anchorwatch/
 *   boattime.py: zone from position (table + US daylight saving + nautical fallback), 5-minute steadiness, manual
 *   pin, GPS time acceptance (two agreeing N2K 126992 messages), sun altitude -> night, and the clock face.
 * Version: v1 (2026-10-02)
 * Scope: PRODUCTION - anchor-box firmware, live on the box at the nav station since 2026-10-02 (installed by signed OTA).
 * Architecture: anchor-box firmware, pure C (stdint + math only, no ESP-IDF) so it is host-tested on the PC
 *   (test_host/test_boattime.c, same cases as reference/tests/test_boattime.py). Fed by boatclock.c (clock) and n2k.c (bt_sync_feed).
 * Invocation: imported by boatclock.c, n2k.c and test_host/test_boattime.c.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    char zid[5];        /* "CT", "ET", "BS", "AT", "Z", "N-5", "N+12" ... */
    int16_t std_min;    /* standard offset from UTC, minutes, east positive */
    bool us_dst;        /* US rule: 2nd Sun March 02:00 std -> 1st Sun November 02:00 daylight */
} bt_zone_t;

#define BT_ZONE_STEADY_S 300
#define BT_AGREE_MS      2000
#define BT_NIGHT_ALT_DEG (-0.833)

int64_t bt_days_from_civil(int y, int m, int d);
void bt_civil_from_days(int64_t z, int *y, int *m, int *d);

bt_zone_t bt_nautical_zone(double lon);
bt_zone_t bt_zone_at(double lat, double lon);
bool bt_zone_by_id(const char *zid, bt_zone_t *out);
int bt_offset_min(const bt_zone_t *z, int64_t utc_s);

typedef struct {
    bool have_current, have_cand, pinned;
    bt_zone_t current, cand, pin;
    int64_t cand_since;
} bt_chooser_t;

void bt_chooser_init(bt_chooser_t *c);
void bt_chooser_pin(bt_chooser_t *c, const bt_zone_t *z);        /* NULL = back to AUTO */
const bt_zone_t *bt_chooser_update(bt_chooser_t *c, int64_t t_s, bool have_pos, double lat, double lon);
const bt_zone_t *bt_chooser_zone(const bt_chooser_t *c);          /* NULL = no zone yet */

typedef struct {
    bool valid, have_last;
    int64_t utc_ms, mono_ms;          /* accepted UTC at mono_ms */
    int64_t last_utc_ms, last_mono_ms;
} bt_sync_t;

void bt_sync_init(bt_sync_t *s);
void bt_sync_feed(bt_sync_t *s, int64_t utc_ms, int64_t mono_ms, int kind);
int64_t bt_sync_now_ms(const bt_sync_t *s, int64_t mono_ms);

double bt_sun_altitude_deg(int64_t utc_s, double lat, double lon);
bool bt_is_night(int64_t utc_s, double lat, double lon);

typedef struct { int hh, mm; bool manual, night; } bt_face_t;   /* hh < 0 -> "--:--" */

bt_face_t bt_clock_face(const bt_sync_t *s, const bt_chooser_t *c, int64_t mono_ms, bool have_pos, double lat,
                        double lon);
