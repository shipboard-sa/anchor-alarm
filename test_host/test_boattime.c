/**
 * Purpose: Host (PC) test of the C port main/boattime.c against the reference model's cases: generated vectors
 *   (zones at every harbour, DST edges 2026-2030, sun altitude) + the behaviour scenarios mirrored from
 *   reference/tests/test_boattime.py (steadiness, pin, GPS time acceptance, clock face).
 * Version: v1 (2026-10-02)
 * Scope: STANDALONE UTILITY - anchor-box test tooling.
 * Architecture: compiled with zig cc (pip package ziglang in reference/.venv) by test_host/run.bat together with
 *   main/boattime.c and the generated boattime_vectors.h.
 * Invocation: test_host\run.bat (from code\anchor-box). Exit 0 = all pass.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../main/boattime.h"
#include "boattime_vectors.h"

static int fails, checks;
#define CHECK(cond, ...)                                     \
    do {                                                     \
        checks++;                                            \
        if (!(cond)) {                                       \
            fails++;                                         \
            if (fails < 20) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
        }                                                    \
    } while (0)

static const int64_t T0 = 20728LL * 86400000LL + 19LL * 3600000LL;   /* 2026-10-02 19:00 UTC, ms */

int main(void)
{
    CHECK(bt_days_from_civil(2026, 10, 2) == 20728, "days_from_civil");

    for (size_t i = 0; i < sizeof(ZONE_CASES) / sizeof(ZONE_CASES[0]); i++) {
        const zone_case_t *c = &ZONE_CASES[i];
        bt_zone_t z = bt_zone_at(c->lat, c->lon);
        CHECK(strcmp(z.zid, c->zid) == 0, "zone %f,%f: %s want %s", c->lat, c->lon, z.zid, c->zid);
        CHECK(bt_offset_min(&z, c->t) == c->off, "offset %f,%f t=%lld: %d want %d", c->lat, c->lon,
              (long long)c->t, bt_offset_min(&z, c->t), c->off);
    }
    for (size_t i = 0; i < sizeof(SUN_CASES) / sizeof(SUN_CASES[0]); i++) {
        const sun_case_t *c = &SUN_CASES[i];
        double a = bt_sun_altitude_deg(c->t, c->lat, c->lon);
        CHECK(fabs(a - c->alt) < 1e-6, "sun %f,%f t=%lld: %.9f want %.9f", c->lat, c->lon, (long long)c->t, a, c->alt);
    }

    /* nautical + ids */
    double lons[] = { 0.0, -7.4, -7.6, 7.6, -60.0, -89.9, -112.6, 179.9, -179.9 };
    int hours[] = { 0, 0, -1, 1, -4, -6, -8, 12, -12 };
    for (int i = 0; i < 9; i++) CHECK(bt_nautical_zone(lons[i]).std_min == hours[i] * 60, "nautical %f", lons[i]);
    const char *ids[] = { "CT", "ET", "BS", "AT", "Z", "N-5", "N+3" };
    for (int i = 0; i < 7; i++) {
        bt_zone_t z;
        CHECK(bt_zone_by_id(ids[i], &z) && strcmp(z.zid, ids[i]) == 0, "zone id %s", ids[i]);
    }

    /* steadiness (test_first_zone_immediate_then_steady_rule) */
    bt_chooser_t ch;
    bt_chooser_init(&ch);
    CHECK(strcmp(bt_chooser_update(&ch, 0, true, 30.0, -85.42)->zid, "CT") == 0, "first zone");
    CHECK(strcmp(bt_chooser_update(&ch, 10, true, 29.81, -85.30)->zid, "CT") == 0, "not yet");
    CHECK(strcmp(bt_chooser_update(&ch, 309, true, 29.81, -85.30)->zid, "CT") == 0, "299 s");
    CHECK(strcmp(bt_chooser_update(&ch, 310, true, 29.81, -85.30)->zid, "ET") == 0, "300 s");

    /* swinging on a line */
    bt_chooser_init(&ch);
    bt_chooser_update(&ch, 0, true, 29.9, -85.395);
    for (int t = 1; t < 3600; t += 30) {
        double lon = ((t / 30) % 2) ? -85.385 : -85.395;
        CHECK(strcmp(bt_chooser_update(&ch, t, true, 29.9, lon)->zid, "CT") == 0, "line flip at %d", t);
    }

    /* pin */
    bt_chooser_init(&ch);
    bt_chooser_update(&ch, 0, true, 25.05, -77.35);
    bt_zone_t ct;
    bt_zone_by_id("CT", &ct);
    bt_chooser_pin(&ch, &ct);
    CHECK(strcmp(bt_chooser_update(&ch, 1, true, 25.05, -77.35)->zid, "CT") == 0 && ch.pinned, "pin");
    bt_chooser_pin(&ch, NULL);
    CHECK(strcmp(bt_chooser_update(&ch, 2, true, 25.05, -77.35)->zid, "BS") == 0 && !ch.pinned, "unpin");
    bt_chooser_init(&ch);
    CHECK(bt_chooser_update(&ch, 0, false, 0, 0) == NULL, "no position no zone");

    /* GPS time acceptance */
    bt_sync_t s;
    bt_sync_init(&s);
    bt_sync_feed(&s, T0, 1000, 0);
    CHECK(!s.valid, "one message");
    bt_sync_feed(&s, T0 + 1000, 2000, 0);
    CHECK(s.valid && bt_sync_now_ms(&s, 5000) == T0 + 4000, "two agreeing");
    bt_sync_init(&s);
    bt_sync_feed(&s, T0, 1000, 0);
    bt_sync_feed(&s, T0 + 60000, 2000, 0);
    CHECK(!s.valid, "disagreeing pair");
    bt_sync_init(&s);
    bt_sync_feed(&s, T0, 1000, 5);
    bt_sync_feed(&s, T0 + 1000, 2000, 5);
    CHECK(!s.valid, "non-GPS");
    int64_t old = bt_days_from_civil(2019, 4, 7) * 86400000LL;
    bt_sync_feed(&s, old, 1000, 0);
    bt_sync_feed(&s, old + 1000, 2000, 0);
    CHECK(!s.valid, "pre-2026");
    bt_sync_init(&s);
    bt_sync_feed(&s, T0, 1000, 0);
    bt_sync_feed(&s, T0 + 1000, 2000, 0);
    bt_sync_feed(&s, T0 + 3600000, 3000, 0);
    CHECK(bt_sync_now_ms(&s, 3000) == T0 + 2000, "single wild message");
    bt_sync_feed(&s, T0 + 3000, 4000, 0);
    CHECK(bt_sync_now_ms(&s, 4000) == T0 + 3000, "good again");
    bt_sync_init(&s);
    bt_sync_feed(&s, T0, 1000, 0);
    bt_sync_feed(&s, T0 + 1000, 2000, 0);
    bt_sync_feed(&s, T0 + 600000, 3000, 0);
    bt_sync_feed(&s, T0 + 601000, 4000, 0);
    CHECK(bt_sync_now_ms(&s, 4000) == T0 + 601000, "real jump");

    /* clock face */
    bt_sync_init(&s);
    bt_chooser_init(&ch);
    CHECK(bt_clock_face(&s, &ch, 0, false, 0, 0).hh == -1, "dashes before time");
    bt_sync_feed(&s, T0, 1000, 0);
    bt_sync_feed(&s, T0 + 1000, 2000, 0);
    CHECK(bt_clock_face(&s, &ch, 2000, false, 0, 0).hh == -1, "dashes without zone");
    bt_chooser_update(&ch, 2, true, 29.95, -90.07);
    bt_face_t f = bt_clock_face(&s, &ch, 2000, true, 29.95, -90.07);
    CHECK(f.hh == 14 && f.mm == 0 && !f.manual && !f.night, "NOLA 14:00 CDT (got %02d:%02d)", f.hh, f.mm);
    bt_chooser_init(&ch);
    bt_zone_t et;
    bt_zone_by_id("ET", &et);
    bt_chooser_pin(&ch, &et);
    f = bt_clock_face(&s, &ch, 2000, false, 0, 0);
    CHECK(f.hh == 15 && f.mm == 0 && f.manual && !f.night, "pinned ET 15:00");

    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
