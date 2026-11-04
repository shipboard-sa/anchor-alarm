/**
 * Purpose: Host (PC) test of the night-watch remote logic (main/nightwatch.c, ANQ-30 / ANC-27 / ANC-28): parsing the
 *   rules engine's /night-watch reply and the bar's face (what it shows, whether a tap is allowed) in every state.
 * Version: v2 (2026-10-07) - v2: bar shown only after dark unless ARMED / nudge. v1 (2026-10-06)
 * Scope: STANDALONE UTILITY - anchor-box test tooling.
 * Architecture: compiled by test_host/run.bat (zig cc) with main/nightwatch.c; fixed cases, no vectors (the box is
 *   only a remote + display - the night-watch logic itself lives in the Ship's Automation engine).
 * Invocation: test_host\run.bat (from code\anchor-box). Exit 0 = all cases pass.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "../main/nightwatch.h"

static int fails;

#define CHECK(cond, ...)                                                                                              \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("nightwatch FAIL: " __VA_ARGS__);                                                                   \
            printf("\n");                                                                                              \
            fails++;                                                                                                   \
        }                                                                                                              \
    } while (0)

static int parse_tests(void)
{
    int n = 0;
    nw_reply_t r;
    /* the contract (D-118): {"armed", "since", "by", "nudge", "night"} */
    n++;
    CHECK(nw_parse("{\"armed\": true, \"since\": \"2026-10-06T03:12:00Z\", \"by\": \"anchor-box\", \"nudge\": false, "
                   "\"night\": true}", &r) && r.armed && !r.nudge && r.night &&
              !strcmp(r.since, "2026-10-06T03:12:00Z") && !strcmp(r.by, "anchor-box"),
          "full armed reply");
    n++;
    CHECK(nw_parse("{\"armed\":false,\"since\":null,\"by\":\"\",\"nudge\":true,\"night\":true}", &r) && !r.armed &&
              r.nudge && r.night && r.since[0] == 0 && r.by[0] == 0,
          "compact off + nudge, since null");
    n++;   /* key order and whitespace do not matter; unknown keys ignored */
    CHECK(nw_parse("{\n  \"night\" : false,\n  \"extra\": [1, 2],\n  \"armed\" :\ttrue\n}", &r) && r.armed &&
              !r.night,
          "reordered, whitespace, extra keys");
    n++;   /* "armed" is required */
    CHECK(!nw_parse("{\"nudge\": true}", &r), "no armed key -> not a reply");
    n++;
    CHECK(!nw_parse("{\"detail\":\"Not authenticated\"}", &r), "401 body is not a reply");
    n++;
    CHECK(!nw_parse("<html>502 Bad Gateway</html>", &r), "html is not a reply");
    n++;
    CHECK(!nw_parse("{\"armed\": \"yes\"}", &r), "armed must be a JSON boolean");
    n++;
    CHECK(!nw_parse(NULL, &r), "NULL body");
    n++;   /* "armed_at" must not be taken for "armed" */
    CHECK(!nw_parse("{\"armed_at\": true}", &r), "armed_at is not armed");
    n++;   /* long by: truncated, never overflows */
    CHECK(nw_parse("{\"armed\":true,\"by\":\"a-very-long-name-that-does-not-fit-in-the-field\"}", &r) &&
              strlen(r.by) == sizeof(r.by) - 1,
          "long by truncated");
    return n;
}

static nw_view_t base(void)
{
    nw_view_t v = { .wifi = true, .polled = true, .link = true, .token = true };
    return v;
}

static int face_tests(void)
{
    int n = 0;
    nw_face_t f;
    nw_view_t v;

    v = base();
    v.wifi = false;
    nw_face(&v, &f);
    n++;
    CHECK(f.kind == NWF_NO_LINK && !f.tappable && strstr(f.big, "no link"), "no Wi-Fi -> no link");

    v = base();
    v.polled = false;
    nw_face(&v, &f);
    n++;
    CHECK(f.kind == NWF_WAIT && !f.tappable, "not polled yet -> wait, not tappable");

    v = base();
    v.link = false;
    v.eng.armed = true;   /* an old reply must NOT be shown once the link is gone */
    nw_face(&v, &f);
    n++;
    CHECK(f.kind == NWF_NO_LINK && !f.tappable && !strcmp(f.big, "Night watch: no link") && !strstr(f.big, "ARMED"),
          "no link -> greyed 'Night watch: no link', no stale state");

    v = base();
    v.eng.armed = true;
    nw_face(&v, &f);
    n++;
    CHECK(f.kind == NWF_ARMED && f.tappable && !strcmp(f.big, "NIGHT WATCH ARMED"), "armed");

    v = base();
    nw_face(&v, &f);
    n++;
    CHECK(f.kind == NWF_OFF && f.tappable && !strcmp(f.big, "NIGHT WATCH OFF") && !strcmp(f.small, "tap to arm"),
          "off");

    v = base();
    v.eng.nudge = true;
    nw_face(&v, &f);
    n++;
    CHECK(f.kind == NWF_NUDGE && f.tappable && strstr(f.small, "re-arm?"), "off + nudge -> re-arm prompt, tappable");

    v = base();
    v.eng.armed = true;
    v.eng.nudge = true;   /* a nudge while armed makes no sense - armed wins */
    nw_face(&v, &f);
    n++;
    CHECK(f.kind == NWF_ARMED, "armed + nudge -> armed");

    v = base();
    v.token = false;
    v.eng.nudge = true;
    nw_face(&v, &f);
    n++;
    CHECK(f.kind == NWF_NUDGE && !f.tappable && strstr(f.small, "no token"), "no token -> shown, not tappable");

    v = base();
    v.pending = true;
    v.pending_arm = true;
    nw_face(&v, &f);
    n++;
    CHECK(f.kind == NWF_OFF && !f.tappable && strstr(f.small, "sending ARM"),
          "pending -> engine's state still shown (OFF), 'sending ARM', not tappable");

    v = base();
    v.eng.armed = true;
    strcpy(v.err, "not changed: token refused (401)");
    nw_face(&v, &f);
    n++;
    CHECK(f.kind == NWF_ARMED && f.tappable && strstr(f.small, "401"), "failed tap -> state unchanged, reason shown");
    return n;
}

static int shown_tests(void)
{
    int n = 0;
    const nw_kind_t all[] = { NWF_WAIT, NWF_NO_LINK, NWF_ARMED, NWF_OFF, NWF_NUDGE };
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        n++;
        CHECK(nw_bar_shown(all[i], true), "dark -> bar shown (kind %d)", (int)all[i]);
    }
    n++;
    CHECK(nw_bar_shown(NWF_ARMED, false), "daylight + ARMED -> shown (a watch left on must be seen)");
    n++;
    CHECK(nw_bar_shown(NWF_NUDGE, false), "daylight + nudge -> shown");
    n++;
    CHECK(!nw_bar_shown(NWF_OFF, false), "daylight + OFF -> hidden");
    n++;
    CHECK(!nw_bar_shown(NWF_NO_LINK, false), "daylight + no link -> hidden");
    n++;
    CHECK(!nw_bar_shown(NWF_WAIT, false), "daylight + waiting -> hidden");
    return n;
}

int main(void)
{
    int n = parse_tests() + face_tests() + shown_tests();
    printf("nightwatch: %d cases, %d failed\n", n, fails);
    return fails ? 1 : 0;
}
