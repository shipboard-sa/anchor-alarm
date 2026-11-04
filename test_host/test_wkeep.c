/**
 * Purpose: Host (PC) test of the watchkeeper check-in logic (main/wkeep.c, ANQ-27 / Ship's Automation WF-614, D-161):
 *   parsing the rules engine's GET /watch reply and its 4xx refusal, when the WATCH CHECK-IN screen takes over / goes
 *   away, when a tap may send I'M UP, the words by stage and the POST body.
 * Version: v1 (2026-10-21)
 * Scope: STANDALONE UTILITY - anchor-box test tooling.
 * Architecture: compiled by test_host/run.bat (zig cc) with main/wkeep.c; fixed cases - the OFF reply captured from the
 *   live engine (api.py v15 GET /watch, 2026-10-22, DOCK); the running replies built to the same shape from engine
 *   builtins/watchkeeper.py view(). The clock lives in the engine (its own tests: tests/test_watchkeeper.py).
 * Invocation: test_host\run.bat (from code\anchor-box). Exit 0 = all cases pass.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "../main/wkeep.h"

static int fails, n;

#define CHECK(cond, ...)                                                                                              \
    do {                                                                                                               \
        n++;                                                                                                           \
        if (!(cond)) {                                                                                                 \
            printf("wkeep FAIL: " __VA_ARGS__);                                                                        \
            printf("\n");                                                                                              \
            fails++;                                                                                                   \
        }                                                                                                              \
    } while (0)

/* captured from the live engine (api.py v15), 2026-10-22 01:3xZ, at the dock */
static const char *OFF =
    "{\"on\":false,\"stage\":0,\"started_by\":null,\"since\":null,\"last_up\":null,\"last_up_by\":null,"
    "\"since_up_s\":null,\"next_prompt_s\":null,\"prompt_s\":900,\"nudge_s\":1200,\"alarm_s\":1500,\"dark\":true,"
    "\"held\":false,\"mode\":\"DOCK\"}";
/* the same shape, running (json.dumps spaces), stage 1 at 16 min */
static const char *STAGE1 =
    "{\"on\": true, \"stage\": 1, \"started_by\": \"sunset\", \"since\": \"2026-10-22T00:30:00Z\", \"last_up\": "
    "\"2026-10-22T01:00:00Z\", \"last_up_by\": \"sunset\", \"since_up_s\": 960, \"next_prompt_s\": 0, \"prompt_s\": 900, "
    "\"nudge_s\": 1200, \"alarm_s\": 1500, \"dark\": true, \"held\": false, \"mode\": \"MOTOR\"}";
static const char *STAGE0 =
    "{\"on\":true,\"stage\":0,\"started_by\":\"sunset\",\"since\":\"2026-10-22T00:30:00Z\",\"last_up\":"
    "\"2026-10-22T01:16:00Z\",\"last_up_by\":\"api:anchor-box\",\"since_up_s\":3,\"next_prompt_s\":897,\"prompt_s\":900,"
    "\"nudge_s\":1200,\"alarm_s\":1500,\"dark\":true,\"held\":false,\"mode\":\"MOTOR\"}";

static wk_view_t view(const char *body)
{
    wk_view_t v;
    memset(&v, 0, sizeof(v));
    v.wifi = v.polled = v.token = true;
    v.link = wk_parse(body, &v.eng);
    return v;
}

static void parse_tests(void)
{
    wk_reply_t r;
    CHECK(wk_parse(OFF, &r) && !r.on && r.stage == 0 && r.since_up_s == -1 && r.next_prompt_s == -1 &&
              r.alarm_s == 1500 && !strcmp(r.mode, "DOCK"), "live OFF reply");
    CHECK(wk_parse(STAGE1, &r) && r.on && r.stage == 1 && r.since_up_s == 960 && !strcmp(r.mode, "MOTOR"), "stage 1");
    CHECK(!wk_parse("{\"stage\": 1}", &r), "no 'on' -> not a reply");
    CHECK(!wk_parse("{\"on\": true}", &r), "no 'stage' -> not a reply");
    CHECK(!wk_parse("{\"on\": true, \"stage\": 7}", &r), "stage out of range");
    CHECK(!wk_parse("not json", &r) && !wk_parse(NULL, &r), "garbage");
    CHECK(wk_parse("{\"on\": false, \"stage\": 2}", &r) && r.stage == 0, "a stopped timer never asks");
    char d[96];
    CHECK(wk_detail("{\"detail\":\"no watch timer running\"}", d, sizeof(d)) && !strcmp(d, "no watch timer running"),
          "409 detail: '%s'", d);
    CHECK(!wk_detail("{}", d, sizeof(d)), "no detail");
}

static void decision_tests(void)
{
    wk_view_t off = view(OFF), s1 = view(STAGE1), s0 = view(STAGE0);
    CHECK(!wk_due(&off) && !wk_due(&s0) && wk_due(&s1), "due only while running at stage >= 1");
    CHECK(wk_take_over(&s1, false, true, 0), "from CLOCK at once");
    CHECK(!wk_take_over(&s1, false, false, 5000), "from WATCH not before 15 s untouched");
    CHECK(wk_take_over(&s1, false, false, WK_FROM_WATCH_MS), "from WATCH after 15 s untouched");
    CHECK(!wk_take_over(&s1, true, true, 99999), "never during an anchor ALARM");
    CHECK(!wk_take_over(&off, false, true, 0) && !wk_take_over(&s0, false, true, 0), "nothing due -> no take-over");
    wk_view_t stale = s1;
    stale.link = false;
    CHECK(!wk_take_over(&stale, false, true, 0) && wk_release(&stale), "no link -> never shown, goes away");
    CHECK(!wk_release(&s1) && wk_release(&s0) && wk_release(&off), "release when nothing is due");
    wk_view_t sending = s1;
    sending.pending = true;
    CHECK(!wk_tap(&sending), "no second tap while one is on its way");
    wk_view_t taken = s0;
    strcpy(taken.msg, "I'M UP taken - next check-in in 15 min");
    taken.msg_ok = true;
    CHECK(!wk_release(&taken), "stays while 'taken' shows");
    CHECK(wk_tap(&s1) && wk_tap(&s0), "a tap may send while the timer runs (even early, stage 0)");
    CHECK(!wk_tap(&off), "no timer -> nothing to send");
    wk_view_t notok = s1;
    notok.token = false;
    CHECK(!wk_tap(&notok), "no token -> nothing sent");

    char b[160];
    wk_head(&s1, b, sizeof(b));
    CHECK(!strcmp(b, "WATCH CHECK-IN"), "head stage 1: '%s'", b);
    s1.eng.stage = 2;
    wk_head(&s1, b, sizeof(b));
    CHECK(!strcmp(b, "WATCH CHECK-IN - SECOND CALL"), "head stage 2: '%s'", b);
    s1.eng.stage = 3;
    wk_head(&s1, b, sizeof(b));
    CHECK(!strcmp(b, "WATCHKEEPER NOT CHECKING IN"), "head stage 3: '%s'", b);
    wk_text(&s1, b, sizeof(b));
    CHECK(strstr(b, "16 min since the last I'm up") && strstr(b, "NOW"), "stage 3 text: '%s'", b);
    s1.eng.stage = 1;
    wk_text(&s1, b, sizeof(b));
    CHECK(strstr(b, "16 min since") && strstr(b, "At 25 min the alarm wakes everyone"), "stage 1 text: '%s'", b);

    wk_footer(&s1, b, sizeof(b));
    CHECK(!strcmp(b, "TAP ANYWHERE: I'M UP"), "footer: '%s'", b);
    wk_footer(&sending, b, sizeof(b));
    CHECK(!strcmp(b, "sending I'M UP ..."), "footer sending: '%s'", b);
    wk_footer(&notok, b, sizeof(b));
    CHECK(strstr(b, "cockpit tablet") != NULL, "footer no token: '%s'", b);
    wk_footer(&stale, b, sizeof(b));
    CHECK(strstr(b, "no link") != NULL, "footer no link: '%s'", b);
    wk_post_body(b, sizeof(b));
    CHECK(!strcmp(b, "{\"action\": \"up\", \"by\": \"anchor-box\"}"), "POST body: '%s'", b);
}

int main(void)
{
    parse_tests();
    decision_tests();
    printf("wkeep: %d cases, %d failed\n", n, fails);
    return fails ? 1 : 0;
}
