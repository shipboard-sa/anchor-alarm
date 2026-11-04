/**
 * Purpose: Host (PC) test of the cold-box EXPECTED? logic (main/restock.c, ANC-45 / Ship's Automation WF-502, D-128):
 *   parsing the rules engine's GET /restock reply and its 409 refusal, which box is asked (one at a time), when the
 *   EXPECTED? screen takes over / goes away, when YES / NO may send, the words and the POST body, in every state.
 * Version: v2 (2026-10-18) - v2: YES / NO only (10-18): from CLOCK at once, footer without 'tap elsewhere'. v1 (2026-10-18)
 * Scope: STANDALONE UTILITY - anchor-box test tooling.
 * Architecture: compiled by test_host/run.bat (zig cc) with main/restock.c; fixed cases (the idle reply captured from
 *   the live engine api.py v14 GET /restock on 2026-10-18; the asking / marked / 409 bodies built to the same shape
 *   from engine builtins/restock.py view() and AlarmUp), no vectors - the question lives in the engine.
 * Invocation: test_host\run.bat (from code\anchor-box). Exit 0 = all cases pass.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "../main/restock.h"

static int fails;

#define CHECK(cond, ...)                                                                                              \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("restock FAIL: " __VA_ARGS__);                                                                      \
            printf("\n");                                                                                              \
            fails++;                                                                                                   \
        }                                                                                                              \
    } while (0)

/* captured from the live engine (api.py v14), 2026-10-18 */
static const char *IDLE =
    "{\"boxes\":{\"fridge\":{\"label\":\"REFRIGERATOR\",\"ask_rule\":\"WF-502-FRIDGE\",\"asking\":false,\"alarm\":false,"
    "\"mark\":null},\"engel\":{\"label\":\"ENGEL (portable)\",\"ask_rule\":\"WF-502-ENGEL\",\"asking\":false,\"alarm\":"
    "false,\"mark\":null},\"fz_aft\":{\"label\":\"AFT FREEZER\",\"ask_rule\":\"WF-502-FZ-AFT\",\"asking\":false,\"alarm\":"
    "false,\"mark\":null},\"fz_fwd\":{\"label\":\"FORWARD FREEZER\",\"ask_rule\":\"WF-502-FZ-FWD\",\"asking\":false,"
    "\"alarm\":false,\"mark\":null}},\"hold_h\":2.0}";
/* the same shape: the aft freezer asking */
static const char *ASK_AFT =
    "{\"boxes\":{\"fridge\":{\"label\":\"REFRIGERATOR\",\"ask_rule\":\"WF-502-FRIDGE\",\"asking\":false,\"alarm\":false,"
    "\"mark\":null},\"engel\":{\"label\":\"ENGEL (portable)\",\"ask_rule\":\"WF-502-ENGEL\",\"asking\":false,\"alarm\":"
    "false,\"mark\":null},\"fz_aft\":{\"label\":\"AFT FREEZER\",\"ask_rule\":\"WF-502-FZ-AFT\",\"asking\":true,\"alarm\":"
    "false,\"mark\":null},\"fz_fwd\":{\"label\":\"FORWARD FREEZER\",\"ask_rule\":\"WF-502-FZ-FWD\",\"asking\":false,"
    "\"alarm\":false,\"mark\":null}},\"hold_h\":2.0}";
/* the fridge and the forward freezer asking; spaces as Python's json.dumps writes them */
static const char *ASK_TWO =
    "{\"boxes\": {\"fridge\": {\"label\": \"REFRIGERATOR\", \"ask_rule\": \"WF-502-FRIDGE\", \"asking\": true, "
    "\"alarm\": false, \"mark\": null}, \"engel\": {\"label\": \"ENGEL (portable)\", \"ask_rule\": \"WF-502-ENGEL\", "
    "\"asking\": false, \"alarm\": false, \"mark\": null}, \"fz_aft\": {\"label\": \"AFT FREEZER\", \"ask_rule\": "
    "\"WF-502-FZ-AFT\", \"asking\": false, \"alarm\": false, \"mark\": null}, \"fz_fwd\": {\"label\": \"FORWARD FREEZER\", "
    "\"ask_rule\": \"WF-502-FZ-FWD\", \"asking\": true, \"alarm\": false, \"mark\": null}}, \"hold_h\": 2.0}";
/* the POST yes reply for the fridge: its mark is set while the annunciator still shows the question for a tick */
static const char *FRIDGE_HELD =
    "{\"boxes\":{\"fridge\":{\"label\":\"REFRIGERATOR\",\"ask_rule\":\"WF-502-FRIDGE\",\"asking\":true,\"alarm\":false,"
    "\"mark\":{\"kind\":\"hold\",\"since\":\"2026-10-18T16:40:00Z\",\"until\":\"2026-10-18T18:40:00Z\",\"by\":"
    "\"api:anchor-box\",\"source\":\"expected\"}},\"engel\":{\"label\":\"ENGEL (portable)\",\"ask_rule\":\"WF-502-ENGEL\","
    "\"asking\":false,\"alarm\":false,\"mark\":null}},\"hold_h\":2.0}";
static const char *FRIDGE_NO =
    "{\"boxes\":{\"fridge\":{\"label\":\"REFRIGERATOR\",\"ask_rule\":\"WF-502-FRIDGE\",\"asking\":false,\"alarm\":true,"
    "\"mark\":{\"kind\":\"denied\",\"since\":\"2026-10-18T16:40:00Z\",\"until\":\"2026-10-18T18:40:00Z\",\"by\":"
    "\"api:anchor-box\",\"source\":\"answer\"}}},\"hold_h\":2.0}";
/* asking but its WF-501 is already up (the engine's condition excludes it; belt and braces) */
static const char *ASK_ALARM =
    "{\"boxes\":{\"fridge\":{\"label\":\"REFRIGERATOR\",\"ask_rule\":\"WF-502-FRIDGE\",\"asking\":true,\"alarm\":true,"
    "\"mark\":null}},\"hold_h\":2.0}";
/* a tricky label: a brace and an escaped quote inside the string must not derail the walker */
static const char *TRICKY =
    "{\"boxes\":{\"engel\":{\"label\":\"ENGEL {\\\"x\\\"}\",\"asking\":true,\"alarm\":false,\"mark\":null}},"
    "\"hold_h\":1.5}";
static const char *REFUSED =
    "{\"detail\":\"the REFRIGERATOR alarm is already up - no restock hold; it clears when the box is back under its "
    "limit\"}";

static rs_view_t base(const char *body)
{
    rs_view_t v;
    memset(&v, 0, sizeof(v));
    v.wifi = v.polled = v.token = true;
    v.get_http = 200;
    v.pending_box = -1;
    v.link = rs_parse(body, &v.eng);
    return v;
}

static int parse_tests(void)
{
    int n = 0;
    rs_reply_t r;
    n++;
    CHECK(rs_parse(IDLE, &r) && r.hold_h == 2.0f, "idle reply parses");
    n++;
    CHECK(r.b[0].known && !strcmp(r.b[0].label, "REFRIGERATOR") && !r.b[0].asking && r.b[0].mark == RS_MARK_NONE,
          "fridge idle");
    n++;
    CHECK(r.b[3].known && !strcmp(r.b[3].label, "FORWARD FREEZER"), "forward freezer label");
    n++;
    CHECK(!strcmp(r.b[1].label, "ENGEL (portable)"), "engel label with brackets");
    n++;
    CHECK(rs_parse(ASK_AFT, &r) && r.b[2].asking && !r.b[0].asking && !r.b[3].asking, "aft freezer asking");
    n++;
    CHECK(rs_parse(ASK_TWO, &r) && r.b[0].asking && r.b[3].asking && !r.b[1].asking, "two asking, spaced json");
    n++;
    CHECK(rs_parse(FRIDGE_HELD, &r) && r.b[0].mark == RS_MARK_HOLD && r.b[1].known && !r.b[2].known && !r.b[3].known,
          "hold mark; missing boxes unknown");
    n++;
    CHECK(rs_parse(FRIDGE_NO, &r) && r.b[0].mark == RS_MARK_DENIED && r.b[0].alarm, "denied mark");
    n++;
    CHECK(rs_parse(TRICKY, &r) && r.b[1].asking && r.hold_h == 1.5f && !strncmp(r.b[1].label, "ENGEL {", 7),
          "escaped quote + brace inside a label");
    n++;
    CHECK(!rs_parse(NULL, &r), "NULL");
    n++;
    CHECK(!rs_parse("", &r), "empty");
    n++;
    CHECK(!rs_parse(REFUSED, &r), "a 409 body is not a reply");
    n++;
    CHECK(!rs_parse("{\"boxes\":{},\"hold_h\":2}", &r), "no boxes");
    n++;
    CHECK(!rs_parse("{\"boxes\":{\"fridge\":{\"asking\":true,\"alarm\":false}}}", &r), "no hold_h");
    n++;
    CHECK(!rs_parse("{\"boxes\":{\"fridge\":{\"asking\":true,\"alarm\":false}},\"hold_h\":0}", &r), "hold_h 0");
    n++;
    CHECK(!rs_parse("{\"boxes\":{\"fridge\":{\"asking\":\"yes\",\"alarm\":false}},\"hold_h\":2}", &r),
          "asking not a bool -> no box");
    n++;
    CHECK(!rs_parse("{\"hold_h\":2,\"x\":{\"boxes\":{\"fridge\":{\"asking\":true,\"alarm\":false}}}}", &r),
          "boxes only at the top level");
    n++;
    CHECK(!rs_parse("{\"boxes\":{\"fridge\":{\"asking\":true,\"alarm\":false}", &r), "cut-off body");
    n++;
    CHECK(rs_parse("{\"boxes\":{\"fz_fwd\":{\"asking\":true,\"alarm\":false}},\"hold_h\":2}", &r) &&
              !strcmp(r.b[3].label, "fz_fwd"),
          "no label -> the key");
    char d[128];
    n++;
    CHECK(rs_detail(REFUSED, d, sizeof(d)) && !strncmp(d, "the REFRIGERATOR alarm is already up", 36), "409 detail");
    n++;
    CHECK(!rs_detail(IDLE, d, sizeof(d)) && d[0] == 0, "no detail in a reply");
    n++;
    CHECK(rs_detail(REFUSED, d, 12) && strlen(d) == 11, "detail cut to the buffer");
    return n;
}

static int asking_tests(void)
{
    int n = 0, c;
    rs_view_t v = base(IDLE);
    n++;
    CHECK(rs_asking(&v, &c) == -1 && c == 0, "idle: nobody asking");
    v = base(ASK_AFT);
    n++;
    CHECK(rs_asking(&v, &c) == 2 && c == 1, "aft freezer asked");
    v = base(ASK_TWO);
    n++;
    CHECK(rs_asking(&v, &c) == 0 && c == 2, "two: the fridge first, one at a time");
    v = base(FRIDGE_HELD);
    n++;
    CHECK(rs_asking(&v, &c) == -1 && c == 0, "answered (mark set) while the engine still shows asking: not asked");
    v = base(FRIDGE_NO);
    n++;
    CHECK(rs_asking(&v, NULL) == -1, "NO taken: not asked");
    v = base(ASK_ALARM);
    n++;
    CHECK(rs_asking(&v, NULL) == -1, "its alarm already up: not asked");
    v = base(ASK_AFT);
    v.link = false;
    n++;
    CHECK(rs_asking(&v, &c) == -1 && c == 0, "stale data: never asked");
    n++;
    CHECK(rs_asking(&v, NULL) == -1, "count may be NULL");
    return n;
}

static int screen_tests(void)
{
    int n = 0;
    rs_view_t v = base(ASK_AFT);
    n++;
    CHECK(rs_take_over(&v, false, true, 0), "from CLOCK, just touched: at once (no 'not now')");
    n++;
    CHECK(rs_take_over(&v, false, true, 6 * 3600 * 1000u), "from CLOCK, idle for hours: at once");
    n++;
    CHECK(rs_take_over(&v, false, true, 1000), "from CLOCK, touched 1 s ago: at once");
    n++;
    CHECK(rs_take_over(&v, false, false, RS_FROM_WATCH_MS), "from WATCH, 15 s untouched");
    n++;
    CHECK(!rs_take_over(&v, false, false, RS_FROM_WATCH_MS - 1), "from WATCH, being used");
    n++;
    CHECK(!rs_take_over(&v, true, true, 6 * 3600 * 1000u), "never during an anchor ALARM");
    v = base(IDLE);
    n++;
    CHECK(!rs_take_over(&v, false, true, 6 * 3600 * 1000u), "nothing asking: no take-over");
    v = base(ASK_AFT);
    v.link = false;
    n++;
    CHECK(!rs_take_over(&v, false, true, 6 * 3600 * 1000u), "no link: no take-over");

    v = base(ASK_AFT);
    n++;
    CHECK(!rs_release(&v), "asking: stays");
    v = base(IDLE);
    n++;
    CHECK(rs_release(&v), "nothing asking: goes");
    v.pending = true;
    n++;
    CHECK(!rs_release(&v), "an answer on its way: stays");
    v.pending = false;
    v.msg_ok = true;
    strcpy(v.msg, "YES taken - the REFRIGERATOR alarm waits 2 h");
    n++;
    CHECK(!rs_release(&v), "'taken' showing: stays");
    v = base(ASK_AFT);
    v.link = false;
    n++;
    CHECK(rs_release(&v), "no link: goes");
    v = base(FRIDGE_HELD);
    n++;
    CHECK(rs_release(&v), "answered, engine still asking for a tick: goes");
    return n;
}

static int tap_tests(void)
{
    int n = 0, box = -9;
    rs_view_t v = base(ASK_AFT);
    n++;
    CHECK(rs_tap(&v, &box) && box == 2, "tap answers the aft freezer");
    v = base(ASK_TWO);
    n++;
    CHECK(rs_tap(&v, &box) && box == 0, "two asking: the one shown");
    v.pending = true;
    box = -9;
    n++;
    CHECK(!rs_tap(&v, &box) && box == -9, "an answer on its way: no second");
    v = base(ASK_AFT);
    v.token = false;
    n++;
    CHECK(!rs_tap(&v, &box), "no token: nothing sent");
    v = base(ASK_AFT);
    v.link = false;
    n++;
    CHECK(!rs_tap(&v, &box), "no link: nothing sent");
    v = base(IDLE);
    n++;
    CHECK(!rs_tap(&v, &box), "nothing asking: nothing sent");
    v = base(FRIDGE_HELD);
    n++;
    CHECK(!rs_tap(&v, &box), "already answered: nothing sent");
    v = base(ASK_AFT);
    strcpy(v.msg, "not answered: engine not answering - try again");
    n++;
    CHECK(rs_tap(&v, &box) && box == 2, "after a failed answer: may try again");
    return n;
}

static int words_tests(void)
{
    int n = 0;
    char b[160];
    rs_view_t v = base(ASK_AFT);
    rs_question(&v, 2, b, sizeof(b));
    n++;
    CHECK(!strcmp(b, "AFT FREEZER warm 10 min - warm food just went in?"), "question: %s", b);
    rs_question(&v, -1, b, sizeof(b));
    n++;
    CHECK(b[0] == 0, "no box: no question");
    rs_question(&v, 7, b, sizeof(b));
    n++;
    CHECK(b[0] == 0, "bad box: no question");
    rs_yes_label(&v, b, sizeof(b));
    n++;
    CHECK(!strcmp(b, "YES\ngive it 2 h"), "yes label: %s", b);
    v = base(TRICKY);
    rs_yes_label(&v, b, sizeof(b));
    n++;
    CHECK(!strcmp(b, "YES\ngive it 1.5 h"), "yes label follows hold_h: %s", b);
    rs_no_label(b, sizeof(b));
    n++;
    CHECK(!strcmp(b, "NO\nalarm now"), "no label");

    v = base(ASK_AFT);
    int c;
    rs_asking(&v, &c);
    rs_footer(&v, c, b, sizeof(b));
    n++;
    CHECK(strstr(b, "one tap answers") && !strstr(b, "elsewhere") && strstr(b, "30 min"), "footer, one asking: %s", b);
    v = base(ASK_TWO);
    rs_asking(&v, &c);
    rs_footer(&v, c, b, sizeof(b));
    n++;
    CHECK(strstr(b, "1 more box asking"), "footer, two asking: %s", b);
    v.pending = true;
    v.pending_box = 0;
    v.pending_yes = true;
    rs_footer(&v, c, b, sizeof(b));
    n++;
    CHECK(!strcmp(b, "sending YES for the REFRIGERATOR ..."), "footer, sending: %s", b);
    v.pending = false;
    strcpy(v.msg, "the REFRIGERATOR alarm is already up");
    rs_footer(&v, c, b, sizeof(b));
    n++;
    CHECK(!strcmp(b, "the REFRIGERATOR alarm is already up"), "footer, refusal");
    v.msg[0] = 0;
    v.token = false;
    rs_footer(&v, c, b, sizeof(b));
    n++;
    CHECK(strstr(b, "cockpit page") != NULL, "footer, no token: %s", b);
    v.link = false;
    rs_footer(&v, 0, b, sizeof(b));
    n++;
    CHECK(strstr(b, "no link") != NULL, "footer, no link");

    rs_post_body(0, true, b, sizeof(b));
    n++;
    CHECK(!strcmp(b, "{\"box\": \"fridge\", \"answer\": \"yes\", \"by\": \"anchor-box\"}"), "post yes: %s", b);
    rs_post_body(3, false, b, sizeof(b));
    n++;
    CHECK(!strcmp(b, "{\"box\": \"fz_fwd\", \"answer\": \"no\", \"by\": \"anchor-box\"}"), "post no: %s", b);
    return n;
}

int main(void)
{
    int n = parse_tests() + asking_tests() + screen_tests() + tap_tests() + words_tests();
    if (fails) {
        printf("restock: %d of %d cases FAILED\n", fails, n);
        return 1;
    }
    printf("restock: %d cases OK\n", n);
    return 0;
}
