/**
 * Purpose: Host (PC) test of the tank-fill tap logic (main/fill.c, ANC-41 / Ship's Automation D-129): parsing the
 *   rules engine's GET / POST /fill reply, each tank row's face, what a tap may send, the head / footer lines and
 *   the WATCH FILL button's session count, in every state.
 * Version: v1 (2026-10-09)
 * Scope: STANDALONE UTILITY - anchor-box test tooling.
 * Architecture: compiled by test_host/run.bat (zig cc) with main/fill.c; fixed cases, no vectors (the box is only a
 *   remote + display - the fill sessions themselves live in the Ship's Automation engine, builtins/fill.py).
 * Invocation: test_host\run.bat (from code\anchor-box). Exit 0 = all cases pass.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "../main/fill.h"

static int fails;

#define CHECK(cond, ...)                                                                                              \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("fill FAIL: " __VA_ARGS__);                                                                         \
            printf("\n");                                                                                              \
            fails++;                                                                                                   \
        }                                                                                                              \
    } while (0)

/* shaped like engine builtins/fill.py view() (json.dumps defaults: ", " and ": ") */
static const char *FULL =
    "{\"tanks\": {"
    "\"water_port\": {\"label\": \"PORT FRESH WATER (port string)\", \"level\": 85.33333, \"session\": "
    "{\"source\": \"tap\", \"since\": \"2026-10-09T14:02:11Z\", \"by\": \"api:anchor-box\", \"active\": true}}, "
    "\"water_stbd\": {\"label\": \"STARBOARD FRESH WATER (starboard string)\", \"level\": 40.0, \"session\": "
    "{\"source\": \"watermaker\", \"since\": \"2026-10-09T13:00:00Z\", \"by\": \"\", \"active\": false}}, "
    "\"fuel_port\": {\"label\": \"PORT FUEL\", \"level\": null, \"session\": null}, "
    "\"fuel_stbd\": {\"label\": \"STARBOARD FUEL\", \"level\": 12, \"session\": null}}, "
    "\"watermaker\": {\"running\": true, \"watts\": 812.4}}";

static int parse_tests(void)
{
    int n = 0;
    fl_reply_t r;

    n++;
    CHECK(fl_parse(FULL, &r), "full reply parses");
    n++;
    CHECK(r.t[0].present && !strcmp(r.t[0].label, "PORT FRESH WATER (port string)") && r.t[0].has_level &&
              r.t[0].level > 85.3f && r.t[0].level < 85.4f && r.t[0].src == FL_SRC_TAP && r.t[0].active &&
              !strcmp(r.t[0].by, "api:anchor-box") && !strcmp(r.t[0].since, "2026-10-09T14:02:11Z"),
          "water_port: tap session");
    n++;
    CHECK(r.t[1].present && r.t[1].src == FL_SRC_WATERMAKER && !r.t[1].active && r.t[1].level == 40.0f,
          "water_stbd: watermaker session, not active yet");
    n++;
    CHECK(r.t[2].present && !r.t[2].has_level && r.t[2].src == FL_SRC_NONE, "fuel_port: level null, no session");
    n++;
    CHECK(r.t[3].present && r.t[3].has_level && r.t[3].level == 12.0f && r.t[3].src == FL_SRC_NONE,
          "fuel_stbd: integer level");
    n++;
    CHECK(r.wm_running && r.wm_has_watts && r.wm_watts > 812.3f && r.wm_watts < 812.5f, "watermaker running 812 W");

    n++;   /* compact, reordered, extra keys, watts null, a tank missing (engine.yml configures fewer) */
    CHECK(fl_parse("{\"watermaker\":{\"watts\":null,\"running\":false},\"x\":[1,{\"a\":2}],\"tanks\":{"
                   "\"fuel_stbd\":{\"session\":null,\"level\":3.5,\"label\":\"STARBOARD FUEL\"}}}", &r) &&
              !r.t[0].present && !r.t[1].present && !r.t[2].present && r.t[3].present && r.t[3].level == 3.5f &&
              !r.wm_running && !r.wm_has_watts,
          "compact, reordered, one tank only, watts null");
    n++;   /* keys only count at their own level: a label holding key names / braces, a session key inside a string */
    CHECK(fl_parse("{\"tanks\": {\"fuel_port\": {\"label\": \"odd \\\"session\\\": {\\\"source\\\" } label\", "
                   "\"level\": 1, \"session\": null}}}", &r) &&
              r.t[2].present && r.t[2].src == FL_SRC_NONE && r.t[2].level == 1.0f && strstr(r.t[2].label, "session"),
          "key names inside a string are not keys");
    n++;   /* a nested object holding "tanks" is not the root's */
    CHECK(!fl_parse("{\"other\": {\"tanks\": {\"fuel_port\": {\"label\": \"x\"}}}}", &r), "nested tanks ignored");
    n++;   /* unknown tank keys are ignored; none known -> not a reply */
    CHECK(!fl_parse("{\"tanks\": {\"holding\": {\"label\": \"BLACK WATER\", \"level\": 50}}}", &r),
          "only unknown tanks -> not a reply");
    n++;
    CHECK(!fl_parse("{\"tanks\": {}}", &r), "empty tanks -> not a reply");
    n++;
    CHECK(!fl_parse("{\"detail\":\"Not Found\"}", &r), "404 body is not a reply");
    n++;
    CHECK(!fl_parse("{\"detail\":\"tank must be one of ['fuel_port']\"}", &r), "400 body is not a reply");
    n++;
    CHECK(!fl_parse("<html>502 Bad Gateway</html>", &r), "html is not a reply");
    n++;
    CHECK(!fl_parse(NULL, &r), "NULL body");
    n++;
    CHECK(!fl_parse("{\"tanks\": [1, 2]}", &r), "tanks must be an object");
    n++;   /* cut off mid-way (body buffer full): no crash; the tanks read before the cut still count */
    CHECK(fl_parse("{\"tanks\": {\"water_port\": {\"label\": \"P\", \"level\": 5, \"session\": null}, \"water_stbd\": "
                   "{\"label\": \"S\", \"lev", &r) && r.t[0].present && !r.t[1].present,
          "truncated body: what came before the cut");
    n++;
    CHECK(!fl_parse("{\"tanks\": {\"water_port\": {\"label\": \"unterminated", &r), "unterminated string");
    n++;   /* a long label is truncated, never overflows */
    CHECK(fl_parse("{\"tanks\": {\"fuel_port\": {\"label\": \"A VERY LONG LABEL THAT DOES NOT FIT IN THE FORTY EIGHT "
                   "CHARACTER FIELD AT ALL\", \"level\": 1, \"session\": null}}}", &r) &&
              strlen(r.t[2].label) == sizeof(r.t[2].label) - 1,
          "long label truncated");
    n++;   /* an empty label falls back to the key */
    CHECK(fl_parse("{\"tanks\": {\"fuel_port\": {\"label\": \"\", \"level\": 1, \"session\": null}}}", &r) &&
              !strcmp(r.t[2].label, "fuel_port"),
          "empty label -> key");
    n++;   /* a session without "active" takes the engine's default: tap = active, watermaker = waiting */
    CHECK(fl_parse("{\"tanks\": {\"water_port\": {\"label\": \"P\", \"level\": 5, \"session\": {\"source\": \"tap\"}},"
                   " \"water_stbd\": {\"label\": \"S\", \"level\": 5, \"session\": {\"source\": \"watermaker\"}}}}", &r) &&
              r.t[0].active && !r.t[1].active,
          "session without active");
    n++;   /* a level given as a string is not a number */
    CHECK(fl_parse("{\"tanks\": {\"fuel_port\": {\"label\": \"F\", \"level\": \"85\", \"session\": null}}}", &r) &&
              !r.t[2].has_level,
          "string level -> unknown");
    n++;   /* negative level (uncalibrated sender) still reads */
    CHECK(fl_parse("{\"tanks\": {\"fuel_port\": {\"label\": \"F\", \"level\": -2.5, \"session\": null}}}", &r) &&
              r.t[2].has_level && r.t[2].level == -2.5f,
          "negative level");
    return n;
}

static fl_view_t base(void)
{
    fl_view_t v = { .wifi = true, .polled = true, .link = true, .token = true, .get_http = 200, .pending_tank = -1 };
    fl_parse(FULL, &v.eng);
    return v;
}

static int row_tests(void)
{
    int n = 0;
    fl_view_t v = base();
    fl_row_t r;
    bool on;

    fl_row(&v, 0, &r);
    n++;
    CHECK(r.shown && r.filling && r.can_tap && !r.tap_on && !strcmp(r.btn, "STOP") &&
              !strcmp(r.name, "PORT FRESH WATER (port string)") && strstr(r.state, "85 %") &&
              strstr(r.state, "FILLING - started by api:anchor-box"),
          "tap session -> STOP (state '%s')", r.state);
    fl_row(&v, 1, &r);
    n++;   /* watermaker session: no tap (ends with the watermaker; stopping would drop the D-130 cut) */
    CHECK(r.shown && !r.filling && !r.can_tap && !strcmp(r.btn, "watermaker") &&
              strstr(r.state, "watermaker running - not rising here"),
          "watermaker session waiting -> no tap ('%s')", r.state);
    n++;
    CHECK(!fl_tap(&v, 1, &on), "fl_tap refuses a watermaker session");
    v.eng.t[1].active = true;
    fl_row(&v, 1, &r);
    n++;
    CHECK(r.filling && !r.can_tap && strstr(r.state, "40 %   FILLING from the watermaker"),
          "watermaker session active ('%s')", r.state);
    fl_row(&v, 2, &r);
    n++;
    CHECK(r.shown && !r.filling && r.can_tap && r.tap_on && !strcmp(r.btn, "FILLING") &&
              !strcmp(r.state, "level ?   not filling"),
          "no session, level unknown -> FILLING ('%s')", r.state);
    n++;
    CHECK(fl_tap(&v, 3, &on) && on, "fl_tap: idle tank -> start");
    n++;
    CHECK(fl_tap(&v, 0, &on) && !on, "fl_tap: tap session -> stop");
    n++;
    CHECK(!fl_tap(&v, -1, &on) && !fl_tap(&v, 4, &on), "fl_tap: index out of range");

    /* a tap on its way: that row says sending, every tap waits */
    v = base();
    v.pending = true;
    v.pending_tank = 3;
    v.pending_on = true;
    fl_row(&v, 3, &r);
    n++;
    CHECK(!r.can_tap && !strcmp(r.btn, "sending ..."), "pending row");
    fl_row(&v, 2, &r);
    n++;
    CHECK(!r.can_tap && !strcmp(r.btn, "FILLING"), "other rows wait while a tap is on its way");

    /* no token: shown, never tappable */
    v = base();
    v.token = false;
    fl_row(&v, 0, &r);
    n++;
    CHECK(r.shown && !r.can_tap && !strcmp(r.btn, "STOP"), "no token -> not tappable");

    /* no link: nothing shown (never a stale state) */
    v = base();
    v.link = false;
    fl_row(&v, 0, &r);
    n++;
    CHECK(!r.shown && !r.can_tap, "no link -> row hidden");
    n++;
    CHECK(!fl_tap(&v, 0, &on), "no link -> no tap");

    /* a tank the engine did not list */
    v = base();
    v.eng.t[2].present = false;
    fl_row(&v, 2, &r);
    n++;
    CHECK(!r.shown && !fl_tap(&v, 2, &on), "unlisted tank hidden + not tappable");

    /* a long label loses its trailing "(...)"; a short one keeps it */
    v = base();
    fl_row(&v, 1, &r);
    n++;
    CHECK(!strcmp(r.name, "STARBOARD FRESH WATER"), "long label shortened ('%s')", r.name);
    fl_row(&v, 0, &r);
    n++;
    CHECK(!strcmp(r.name, "PORT FRESH WATER (port string)"), "short label kept ('%s')", r.name);
    strcpy(v.eng.t[3].label, "A LABEL OF MORE THAN THIRTY TWO CHARS NO PAREN");
    fl_row(&v, 3, &r);
    n++;
    CHECK(!strcmp(r.name, v.eng.t[3].label), "long label without (...) kept whole");

    /* a session with no 'by' */
    v = base();
    v.eng.t[0].by[0] = 0;
    fl_row(&v, 0, &r);
    n++;
    CHECK(strstr(r.state, "started by the crew") != NULL, "empty by -> 'the crew'");
    return n;
}

static int line_tests(void)
{
    int n = 0;
    char b[96];
    fl_view_t v = base();

    fl_head(&v, b, sizeof(b));
    n++;
    CHECK(!strcmp(b, "Watermaker RUNNING 812 W"), "head running ('%s')", b);
    v.eng.wm_has_watts = false;
    fl_head(&v, b, sizeof(b));
    n++;
    CHECK(!strcmp(b, "Watermaker RUNNING"), "head running, watts unknown");
    v.eng.wm_running = false;
    fl_head(&v, b, sizeof(b));
    n++;
    CHECK(!strcmp(b, "Watermaker off"), "head off");
    v.link = false;
    fl_head(&v, b, sizeof(b));
    n++;
    CHECK(b[0] == 0, "head empty without link");

    v = base();
    fl_footer(&v, b, sizeof(b));
    n++;
    CHECK(strstr(b, "90 %") && strstr(b, "95 %"), "footer hint ('%s')", b);
    v.pending = true;
    v.pending_on = false;
    fl_footer(&v, b, sizeof(b));
    n++;
    CHECK(!strcmp(b, "sending STOP ..."), "footer sending");
    v = base();
    strcpy(v.err, "not changed: token refused (401)");
    fl_footer(&v, b, sizeof(b));
    n++;
    CHECK(!strcmp(b, v.err), "footer error");
    v = base();
    v.token = false;
    fl_footer(&v, b, sizeof(b));
    n++;
    CHECK(strstr(b, "no token") != NULL, "footer no token");
    v = base();
    v.link = false;
    v.get_http = 404;
    fl_footer(&v, b, sizeof(b));
    n++;
    CHECK(strstr(b, "(404)") != NULL, "footer 404 = engine without /fill ('%s')", b);
    v.get_http = 0;
    fl_footer(&v, b, sizeof(b));
    n++;
    CHECK(strstr(b, "not answering") != NULL, "footer no answer");
    v = base();
    v.polled = false;
    v.link = false;
    fl_footer(&v, b, sizeof(b));
    n++;
    CHECK(strstr(b, "asking") != NULL, "footer before the first poll");
    v.wifi = false;
    fl_footer(&v, b, sizeof(b));
    n++;
    CHECK(strstr(b, "no Wi-Fi") != NULL, "footer no Wi-Fi");

    /* the CLOCK tab: tap + active watermaker sessions count, a waiting watermaker session does not */
    v = base();
    n++;
    CHECK(fl_tab(&v, b, sizeof(b)) == 1 && !strcmp(b, "ON"), "tab: one session ('%s')", b);
    v.eng.t[1].active = true;
    n++;
    CHECK(fl_tab(&v, b, sizeof(b)) == 2 && !strcmp(b, "2 ON"), "tab: two sessions ('%s')", b);
    v.eng.t[0].src = FL_SRC_NONE;
    v.eng.t[1].src = FL_SRC_NONE;
    n++;
    CHECK(fl_tab(&v, b, sizeof(b)) == 0 && b[0] == 0, "tab: none");
    v = base();
    v.link = false;
    n++;
    CHECK(fl_tab(&v, b, sizeof(b)) == 0 && b[0] == 0, "tab: no link -> nothing claimed");
    return n;
}

int main(void)
{
    int n = parse_tests() + row_tests() + line_tests();
    printf("fill: %d cases, %d failed\n", n, fails);
    return fails ? 1 : 0;
}
