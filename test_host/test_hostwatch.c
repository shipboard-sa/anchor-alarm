/**
 * Purpose: Host (PC) test of the C 'monitoring host down' logic (main/hostwatch.c, ANC-25): replays every
 *   observation generated from the Python reference (tools/make_hostwatch_vectors.py) and checks the same event,
 *   state and lamp after each one.
 * Version: v1 (2026-10-03)
 * Scope: STANDALONE UTILITY - anchor-box test tooling.
 * Architecture: compiled by test_host/run.bat (zig cc) with main/hostwatch.c; reads test_host/vectors/hostwatch.txt.
 * Invocation: test_host\run.bat (from code\anchor-box). Exit 0 = all observations match.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "../main/hostwatch.h"

/* SignalK reply parsing - fixed cases (expected epochs from Python datetime) */
static int parse_tests(void)
{
    int fails = 0;
    struct { const char *s; bool ok; long long ms; } iso[] = {
        { "2026-10-03T14:13:12.345Z", true, 1791036792345LL },
        { "1970-01-01T00:00:00Z", true, 0 },
        { "2024-02-29T23:59:59.9Z", true, 1709251199900LL },
        { "2000-03-01T00:00:00.000123Z", true, 951868800000LL },
        { "2026-10-03 14:13:12Z", false, 0 },
        { "2026-10-03T14:13:12+02:00", false, 0 },
        { "garbage", false, 0 },
    };
    for (unsigned i = 0; i < sizeof(iso) / sizeof(iso[0]); i++) {
        long long ms = -1;
        bool ok = hw_parse_iso_ms(iso[i].s, &ms);
        if (ok != iso[i].ok || (ok && ms != iso[i].ms)) {
            printf("hostwatch iso '%s': got %d %lld\n", iso[i].s, ok, ms);
            fails++;
        }
    }
    struct { const char *body; bool have_ts; long long ts; bool have_bool, all_true; } sk[] = {
        /* the engine's health leaf (Ship's Automation adapters/signalk.py) */
        { "{\"value\":true,\"$source\":\"rules-engine\",\"timestamp\":\"2026-10-03T14:13:12.345Z\"}",
          true, 1791036792345LL, true, true },
        { "{\"value\": false, \"timestamp\": \"2026-10-03T14:13:12.345Z\"}", true, 1791036792345LL, true, false },
        /* position with two sources: newest timestamp wins; an object value is not a boolean */
        { "{\"meta\":{\"units\":\"deg\"},\"value\":{\"longitude\":-90.1,\"latitude\":30.0},\"$source\":\"can0.3\","
          "\"timestamp\":\"2026-10-03T14:13:10.000Z\",\"values\":{\"can0.3\":{\"value\":{\"longitude\":-90.1},"
          "\"timestamp\":\"2026-10-03T14:13:10.000Z\"},\"yd.1\":{\"value\":{},\"timestamp\":\"2026-10-03T14:13:11.500Z\"}}}",
          true, 1791036791500LL, false, false },
        { "{\"message\":\"not found\"}", false, 0, false, false },
    };
    for (unsigned i = 0; i < sizeof(sk) / sizeof(sk[0]); i++) {
        hw_sk_reply_t r;
        hw_parse_signalk(sk[i].body, &r);
        if (r.have_ts != sk[i].have_ts || (r.have_ts && r.ts_ms != sk[i].ts) || r.have_bool != sk[i].have_bool ||
            r.all_true != sk[i].all_true) {
            printf("hostwatch signalk case %u: got ts %d %lld bool %d %d\n", i, r.have_ts, r.ts_ms, r.have_bool,
                   r.all_true);
            fails++;
        }
    }
    printf("hostwatch: %u parse cases, %d failed\n",
           (unsigned)(sizeof(iso) / sizeof(iso[0]) + sizeof(sk) / sizeof(sk[0])), fails);
    return fails;
}

int main(void)
{
    if (parse_tests()) return 1;
    FILE *f = fopen("test_host/vectors/hostwatch.txt", "r");
    if (!f) { printf("hostwatch: cannot open test_host/vectors/hostwatch.txt\n"); return 1; }
    hw_config_t cfg;
    hw_config_default(&cfg);
    hw_pi_t pi;
    hw_cerbo_t cb;
    char line[256], name[64] = "?", evx[32];
    int n = 0, fails = 0, scen = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == 'N') {
            sscanf(line + 2, "%63s", name);
            hw_pi_init(&pi, &cfg);
            hw_cerbo_init(&cb, &cfg);
            scen++;
            continue;
        }
        double t, lc, lo;
        int ok, router, known, state, asserted;
        hw_event_t e;
        int gs, ga;
        if (line[0] == 'P') {
            sscanf(line + 2, "%lf %d %d %31s %d %d", &t, &ok, &router, evx, &state, &asserted);
            e = hw_pi_update(&pi, t, ok, router);
            gs = pi.state;
            ga = pi.asserted;
        } else if (line[0] == 'C') {
            sscanf(line + 2, "%lf %d %lf %lf %31s %d %d", &t, &known, &lc, &lo, evx, &state, &asserted);
            e = hw_cerbo_update(&cb, t, known, lc, lo);
            gs = cb.state;
            ga = cb.asserted;
        } else {
            continue;
        }
        n++;
        const char *want = strcmp(evx, "-") == 0 ? "" : evx;
        if (strcmp(hw_event_name(e), want) != 0 || gs != state || ga != asserted) {
            if (fails++ < 10)
                printf("hostwatch %-16s t=%.0f: got %s %s %d, want %s %s %d\n", name, t, hw_event_name(e),
                       hw_state_name(gs), ga, want, hw_state_name(state), asserted);
        }
    }
    fclose(f);
    printf("hostwatch: %d scenarios, %d observations, %d mismatches\n", scen, n, fails);
    return fails ? 1 : 0;
}
