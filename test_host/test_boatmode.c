/**
 * Purpose: Host (PC) test of the DOCK / LEAVE tap logic (main/boatmode.c, ANC-44 / Ship's Automation D-151): parsing
 *   the rules engine's GET /mode reply and its 403 refusal, when the ARM page's DOCK and WATCH's LEAVE may send, the
 *   AT DOCK state lines and the POST body, in every state.
 * Version: v1 (2026-10-15)
 * Scope: STANDALONE UTILITY - anchor-box test tooling.
 * Architecture: compiled by test_host/run.bat (zig cc) with main/boatmode.c; fixed cases (replies captured from the
 *   engine's api.py v12 GET /mode + the 403 on 2026-10-15), no vectors - the mode lives in the engine.
 * Invocation: test_host\run.bat (from code\anchor-box). Exit 0 = all cases pass.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "../main/boatmode.h"

static int fails;

#define CHECK(cond, ...)                                                                                              \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("boatmode FAIL: " __VA_ARGS__);                                                                     \
            printf("\n");                                                                                              \
            fails++;                                                                                                   \
        }                                                                                                              \
    } while (0)

/* captured from the engine (api.py v12), 2026-10-15 */
static const char *NONE = "{\"mode\":null,\"secured\":false,\"crew_aboard\":[],\"leave_ok\":true,\"box_modes\":[\"DOCK\",\"MOTOR\"]}";
static const char *DOCK_ALONE =
    "{\"mode\":\"DOCK\",\"secured\":true,\"crew_aboard\":[],\"leave_ok\":false,\"box_modes\":[\"DOCK\",\"MOTOR\"]}";
static const char *DOCK_CREW =
    "{\"mode\":\"DOCK\",\"secured\":true,\"crew_aboard\":[\"crew.skipper_cell\"],\"leave_ok\":true,\"box_modes\":[\"DOCK\",\"MOTOR\"]}";
static const char *MOTOR = "{\"mode\":\"MOTOR\",\"secured\":false,\"crew_aboard\":[\"crew.skipper_cell\",\"crew.mate_cell\"],"
                           "\"leave_ok\":true,\"box_modes\":[\"DOCK\",\"MOTOR\"]}";
static const char *ANCHOR_ALONE =
    "{\"mode\":\"ANCHOR\",\"secured\":true,\"crew_aboard\":[],\"leave_ok\":false,\"box_modes\":[\"DOCK\",\"MOTOR\"]}";
static const char *REFUSED = "{\"detail\":\"DOCK armed, no crew phone aboard - end it from the phone or cockpit tablet\"}";

static bm_view_t base(const char *body)
{
    bm_view_t v;
    memset(&v, 0, sizeof(v));
    v.wifi = v.polled = v.token = true;
    v.get_http = 200;
    v.link = bm_parse(body, &v.eng);
    return v;
}

static int parse_tests(void)
{
    int n = 0;
    bm_reply_t r;
    n++;
    CHECK(bm_parse(NONE, &r) && r.mode == BM_NONE && !r.secured && r.leave_ok && r.crew == 0, "never tapped");
    n++;
    CHECK(bm_parse(DOCK_ALONE, &r) && r.mode == BM_DOCK && r.secured && !r.leave_ok && r.crew == 0, "DOCK, nobody");
    n++;
    CHECK(bm_parse(DOCK_CREW, &r) && r.mode == BM_DOCK && r.leave_ok && r.crew == 1, "DOCK, owner aboard");
    n++;
    CHECK(bm_parse(MOTOR, &r) && r.mode == BM_MOTOR && !r.secured && r.crew == 2, "MOTOR, two phones");
    n++;
    CHECK(bm_parse(ANCHOR_ALONE, &r) && r.mode == BM_ANCHOR && r.secured, "ANCHOR");
    n++;
    CHECK(bm_parse("{ \"leave_ok\" : true , \"mode\" : \"SAIL\" , \"secured\" : false }", &r) && r.mode == BM_SAIL,
          "spaces + any key order + no crew list");
    n++;
    CHECK(!bm_parse(REFUSED, &r), "a 403 body is not a reply");
    n++;
    CHECK(!bm_parse("{\"mode\":\"PARTY\",\"secured\":false,\"leave_ok\":true}", &r), "unknown mode refused");
    n++;
    CHECK(!bm_parse("{\"mode\":\"DOCK\",\"leave_ok\":true}", &r), "no secured -> not trusted");
    n++;
    CHECK(!bm_parse("{\"x\":{\"mode\":\"DOCK\",\"secured\":true,\"leave_ok\":true}}", &r), "keys inside another object");
    n++;
    CHECK(!bm_parse("{\"note\":\"\\\"mode\\\": \\\"DOCK\\\"\",\"secured\":true}", &r), "keys inside a string");
    n++;
    CHECK(!bm_parse(NULL, &r) && !bm_parse("", &r) && !bm_parse("[]", &r), "empty / not an object");
    char d[96];
    n++;
    CHECK(bm_detail(REFUSED, d, sizeof(d)) && strcmp(d, "DOCK armed, no crew phone aboard - end it from the phone or cockpit tablet") == 0,
          "403 detail '%s'", d);
    n++;
    CHECK(!bm_detail(DOCK_CREW, d, sizeof(d)) && d[0] == 0, "no detail");
    n++;
    CHECK(bm_detail(REFUSED, d, 10) && strlen(d) == 9, "detail cut to the buffer");
    char b[64];
    bm_post_body(BM_DOCK, b, sizeof(b));
    n++;
    CHECK(strcmp(b, "{\"mode\": \"DOCK\", \"by\": \"anchor-box\"}") == 0, "DOCK body '%s'", b);
    bm_post_body(BM_MOTOR, b, sizeof(b));
    n++;
    CHECK(strcmp(b, "{\"mode\": \"MOTOR\", \"by\": \"anchor-box\"}") == 0, "MOTOR body '%s'", b);
    return n;
}

static int face_tests(void)
{
    int n = 0;
    char l1[64], l2[112];
    bm_view_t v;

    v = base(NONE);                                    /* nothing tapped: DOCK may arm, no LEAVE, normal lines */
    bm_lines(&v, l1, sizeof(l1), l2, sizeof(l2));
    n++;
    CHECK(bm_dock_ok(&v) && !bm_at_dock(&v) && !bm_leave_ok(&v) && !l1[0] && !l2[0], "none: DOCK ok, quiet");

    v = base(MOTOR);
    n++;
    CHECK(bm_dock_ok(&v) && !bm_leave_ok(&v), "MOTOR: DOCK ok");

    v = base(DOCK_ALONE);                              /* the theft case: at DOCK, nobody's phone aboard */
    bm_lines(&v, l1, sizeof(l1), l2, sizeof(l2));
    n++;
    CHECK(bm_at_dock(&v) && !bm_leave_ok(&v) && !bm_dock_ok(&v), "DOCK alone: no LEAVE, no second DOCK");
    n++;
    CHECK(strcmp(l1, "AT DOCK - slip fence armed") == 0 && strstr(l2, "needs one of our phones aboard"), "'%s' / '%s'", l1, l2);

    v = base(DOCK_CREW);                               /* a phone aboard: LEAVE offered */
    bm_lines(&v, l1, sizeof(l1), l2, sizeof(l2));
    n++;
    CHECK(bm_leave_ok(&v) && !bm_dock_ok(&v) && strstr(l2, "LEAVE ends it"), "DOCK + crew: LEAVE ok '%s'", l2);

    v = base(ANCHOR_ALONE);                            /* DOCK over an armed ANCHOR would end the anchor watch */
    n++;
    CHECK(!bm_dock_ok(&v) && !bm_at_dock(&v) && !bm_leave_ok(&v), "ANCHOR alone: no DOCK");
    v.eng.leave_ok = true;
    n++;
    CHECK(bm_dock_ok(&v), "ANCHOR + crew: DOCK ok");

    v = base(DOCK_CREW);                               /* link lost: never claim DOCK, never send */
    v.link = false;
    bm_lines(&v, l1, sizeof(l1), l2, sizeof(l2));
    n++;
    CHECK(!bm_at_dock(&v) && !bm_leave_ok(&v) && !bm_dock_ok(&v) && !l1[0] && !l2[0], "no link: nothing");

    v = base(DOCK_CREW);                               /* no token on this box */
    v.token = false;
    bm_lines(&v, l1, sizeof(l1), l2, sizeof(l2));
    n++;
    CHECK(bm_at_dock(&v) && !bm_leave_ok(&v) && strstr(l2, "no token"), "no token: shown, not sendable '%s'", l2);
    v = base(NONE);
    v.token = false;
    n++;
    CHECK(!bm_dock_ok(&v), "no token: no DOCK");

    v = base(DOCK_CREW);                               /* a tap on its way: one at a time */
    v.pending = true;
    v.pending_mode = BM_MOTOR;
    bm_lines(&v, l1, sizeof(l1), l2, sizeof(l2));
    n++;
    CHECK(!bm_leave_ok(&v) && strstr(l2, "sending LEAVE"), "pending LEAVE '%s'", l2);
    v = base(NONE);
    v.pending = true;
    v.pending_mode = BM_DOCK;
    bm_lines(&v, l1, sizeof(l1), l2, sizeof(l2));
    n++;
    CHECK(!bm_dock_ok(&v) && strcmp(l1, "DOCK") == 0 && strstr(l2, "sending DOCK"), "pending DOCK '%s' / '%s'", l1, l2);

    v = base(DOCK_ALONE);                              /* the engine refused (e.g. the last phone left meanwhile) */
    strcpy(v.msg, "DOCK armed, no crew phone aboard - end it from the phone or cockpit tablet");
    bm_lines(&v, l1, sizeof(l1), l2, sizeof(l2));
    n++;
    CHECK(strcmp(l1, "AT DOCK - slip fence armed") == 0 && strstr(l2, "no crew phone aboard"), "refusal '%s'", l2);
    v = base(NONE);
    strcpy(v.msg, "engine not answering");
    bm_lines(&v, l1, sizeof(l1), l2, sizeof(l2));
    n++;
    CHECK(strcmp(l1, "boat mode not changed") == 0 && strcmp(l2, "engine not answering") == 0, "error off-dock");

    n++;
    CHECK(strcmp(bm_name(BM_DOCK), "DOCK") == 0 && strcmp(bm_name(BM_NONE), "") == 0 &&
          strcmp(bm_name((bm_mode_t)99), "") == 0, "names");
    return n;
}

int main(void)
{
    int n = parse_tests() + face_tests();
    printf("boatmode: %d cases, %d failed\n", n, fails);
    return fails ? 1 : 0;
}
