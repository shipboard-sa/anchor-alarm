/**
 * Purpose: Host (PC) test of the C anchor watch (main/anchorwatch.c): replays every scenario generated from the
 *   Python reference (tools/make_watch_vectors.py) and checks the C watch produces the same events (kind, state,
 *   time) and the same final circle.
 * Version: v2 (2026-10-02) - v2: R command (power-cut snapshot / restore)
 * Scope: STANDALONE UTILITY - anchor-box test tooling.
 * Architecture: compiled by test_host/run.bat (zig cc) with main/anchorwatch.c; reads test_host/vectors/index.txt and
 *   the scenario files it lists.
 * Invocation: test_host\run.bat (from code\anchor-box). Exit 0 = all scenarios match.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../main/anchorwatch.h"

static int run_one(const char *dir, const char *name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.txt", dir, name);
    FILE *f = fopen(path, "r");
    if (!f) { printf("%-15s cannot open %s\n", name, path); return 1; }
    static aw_t w;
    aw_init(&w, NULL);
    aw_event_t ev[AW_MAX_EVENTS];
    int got = 0, used = 0, fails = 0, cmds = 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char c = line[0];
        double t, a, b;
        if (c == 'E') {
            int kind, state;
            sscanf(line + 2, "%lf %d %d", &t, &kind, &state);
            if (used >= got) {
                if (fails++ < 5) printf("%-15s MISSING event t=%.1f kind %d state %s\n", name, t, kind,
                                        aw_state_name(state));
            } else {
                aw_event_t *e = &ev[used++];
                if (fabs(e->t - t) > 1e-9 || (int)e->kind != kind || (int)e->state != state) {
                    if (fails++ < 5)
                        printf("%-15s DIFF want t=%.1f kind %d %s, got t=%.1f kind %d %s (%s)\n", name, t, kind,
                               aw_state_name(state), e->t, e->kind, aw_state_name(e->state), e->msg);
                }
            }
            continue;
        }
        if (used < got) {   /* the previous command produced events the Python did not */
            for (; used < got; used++)
                if (fails++ < 5)
                    printf("%-15s EXTRA event t=%.1f kind %d %s (%s)\n", name, ev[used].t, ev[used].kind,
                           aw_state_name(ev[used].state), ev[used].msg);
        }
        used = got = 0;
        cmds++;
        if (c == 'A') { sscanf(line + 2, "%lf %lf", &t, &a); got = aw_arm(&w, t, a, ev); }
        else if (c == 'U') { sscanf(line + 2, "%lf %lf %lf", &t, &a, &b); got = aw_update(&w, t, a, b, ev); }
        else if (c == 'K') { sscanf(line + 2, "%lf", &t); got = aw_tick(&w, t, ev); }
        else if (c == 'D') { sscanf(line + 2, "%lf", &t); got = aw_disarm(&w, t, ev); }
        else if (c == 'R') {             /* power cut: snapshot, a fresh watch, restore */
            sscanf(line + 2, "%lf %lf", &a, &t);
            aw_snapshot_t snap;
            if (!aw_snapshot(&w, &snap) && fails++ < 5) printf("%-15s no snapshot at t=%.1f\n", name, a);
            aw_init(&w, NULL);
            got = aw_restore(&w, t, &snap, ev);
        }
        else if (c == 'F') {
            int st, has;
            double lat, lon, ar;
            sscanf(line + 2, "%d %d %lf %lf %lf", &st, &has, &lat, &lon, &ar);
            double clat = 0, clon = 0, r = 0;
            bool chas = w.have_centre && aw_centre_latlon(&w, &clat, &clon, &r);
            double car = w.have_alarm_radius ? w.alarm_radius : 0.0;
            if ((int)w.state != st || (int)chas != has || (has && (fabs(clat - lat) > 1e-7 || fabs(clon - lon) > 1e-7)) ||
                fabs(car - ar) > 1e-6) {
                fails++;
                printf("%-15s FINAL want %s circle=%d %.7f,%.7f r=%.3f got %s circle=%d %.7f,%.7f r=%.3f\n", name,
                       aw_state_name(st), has, lat, lon, ar, aw_state_name(w.state), chas, clat, clon, car);
            }
        }
    }
    fclose(f);
    printf("%-15s %s  (%d commands, final %s)\n", name, fails ? "FAIL" : "ok  ", cmds, aw_state_name(w.state));
    return fails ? 1 : 0;
}

int main(void)
{
    const char *dir = "test_host/vectors";
    FILE *ix = fopen("test_host/vectors/index.txt", "r");
    if (!ix) { printf("no test_host/vectors/index.txt - run tools/make_watch_vectors.py\n"); return 2; }
    char name[64];
    int bad = 0, n = 0;
    while (fscanf(ix, "%63s", name) == 1) { bad += run_one(dir, name); n++; }
    fclose(ix);
    printf("%d scenarios, %d failed\n", n, bad);
    return bad ? 1 : 0;
}
