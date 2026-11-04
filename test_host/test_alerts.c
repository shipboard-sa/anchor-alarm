/**
 * Purpose: Host (PC) test of the engine-alerts logic (main/alerts.c, ANQ-10): parsing the rules engine's GET /alerts
 *   reply (incl. quotes / braces / escapes inside a message, truncation to 4, bad replies) and the show decisions
 *   (take over, release only when cleared, tap = ACK or CLOCK, footer, anchor ALARM wins, the CLOCK and WATCH lines,
 *   NO LINK never blank) - the panel-LED model, 10-07.
 * Version: v3 (2026-10-21) - v3: crew RESET cases (rst, reset index / ok, footer, 409 detail - D-163). v2 (2026-10-07)
 *   - v2: tap-to-ack + panel-LED decisions. v1 (2026-10-07)
 * Scope: STANDALONE UTILITY - anchor-box test tooling.
 * Architecture: compiled by test_host/run.bat (zig cc) with main/alerts.c; fixed cases (the alert state lives in the
 *   rules engine - Ship's Automation tests/test_alerts.py checks the producing side of the same contract).
 * Invocation: test_host\run.bat (from code\anchor-box). Exit 0 = all cases pass.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "../main/alerts.h"

static int fails, n;

#define CHECK(cond, ...)                                                                                              \
    do {                                                                                                               \
        n++;                                                                                                           \
        if (!(cond)) {                                                                                                 \
            printf("alerts FAIL: " __VA_ARGS__);                                                                       \
            printf("\n");                                                                                              \
            fails++;                                                                                                   \
        }                                                                                                              \
    } while (0)

/* the engine's real shape (engine/api.py alerts_view, json.dumps default separators) */
static const char *REAL =
    "{\"seq\": 41, \"total\": 3, \"unacked\": 1, \"sounding\": true, \"alerts\": ["
    "{\"id\": \"WF-201\", \"sev\": \"P1\", \"acked\": false, \"sim\": false, \"age_s\": 74, "
    "\"text\": \"P1 Yanmar raw-water flow lost - stops in 30 s: flip the AUTO STOP OFF rocker\"}, "
    "{\"id\": \"WF-312\", \"sev\": \"P2\", \"acked\": true, \"sim\": true, \"age_s\": 1250, "
    "\"text\": \"say \\\"hi\\\" {not json} [nor this] \\\\ done\"}]}";

static void parse_tests(void)
{
    al_reply_t r;
    CHECK(al_parse(REAL, &r), "real reply parses");
    CHECK(r.seq == 41 && r.total == 3 && r.unacked == 1 && r.sounding && r.n == 2, "real reply scalars");
    CHECK(!strcmp(r.a[0].id, "WF-201") && !strcmp(r.a[0].sev, "P1") && !r.a[0].acked && !r.a[0].sim &&
              r.a[0].age_s == 74 && strstr(r.a[0].text, "AUTO STOP OFF"),
          "first alert fields");
    CHECK(r.a[1].acked && r.a[1].sim && r.a[1].age_s == 1250, "second alert flags");
    CHECK(!strcmp(r.a[1].text, "say \"hi\" {not json} [nor this] \\ done"), "escapes + braces inside a message: '%s'",
          r.a[1].text);

    CHECK(al_parse("{\"seq\":0,\"total\":0,\"unacked\":0,\"sounding\":false,\"alerts\":[]}", &r) && r.n == 0 &&
              r.seq == 0,
          "empty engine, compact separators");
    CHECK(al_parse("{\"alerts\": [], \"later\": {\"x\": [1, 2]}, \"seq\": 7}", &r) && r.seq == 7,
          "key order free, unknown nested keys skipped");

    char big[4096];
    int k = snprintf(big, sizeof(big), "{\"seq\": 9, \"total\": 6, \"unacked\": 6, \"sounding\": true, \"alerts\": [");
    for (int i = 0; i < 6; i++)
        k += snprintf(big + k, sizeof(big) - k, "%s{\"id\": \"WF-%d\", \"sev\": \"P1\", \"acked\": false, \"sim\": false, "
                      "\"age_s\": %d, \"text\": \"t%d\"}", i ? ", " : "", i, i, i);
    snprintf(big + k, sizeof(big) - k, "]}");
    CHECK(al_parse(big, &r) && r.n == AL_MAX && r.total == 6 && !strcmp(r.a[3].id, "WF-3"), "more than 4 -> first 4");

    char longt[700];
    k = snprintf(longt, sizeof(longt), "{\"seq\": 1, \"alerts\": [{\"id\": \"WF-1\", \"text\": \"");
    for (int i = 0; i < 500; i++) longt[k++] = 'x';
    snprintf(longt + k, sizeof(longt) - k, "\"}]}");
    CHECK(al_parse(longt, &r) && strlen(r.a[0].text) == AL_TEXT_MAX - 1, "over-long text clipped, no overflow");

    CHECK(al_parse("{\"seq\": 1, \"alerts\": [{\"id\": \"WF-1\", \"text\": \"caf\\u00e9\\nok\"}]}", &r) &&
              !strcmp(r.a[0].text, "caf? ok"),
          "\\u escape -> '?', \\n -> space: '%s'", r.a[0].text);

    CHECK(!al_parse("", &r), "empty body");
    CHECK(!al_parse("{\"armed\": false}", &r), "the night-watch reply is not an alerts reply");
    CHECK(!al_parse("{\"seq\": 1, \"alerts\": [{\"id\": \"WF-1\", \"text\": \"cut off", &r), "truncated body");
    CHECK(!al_parse("<html>502 Bad Gateway</html>", &r), "a proxy error page");
    CHECK(!al_parse("{\"seq\": 1}", &r), "no alerts array");
}

static al_view_t live(int total, int unacked, uint32_t seq)
{
    al_view_t v = { .wifi = true, .polled = true, .link = true, .token = true, .answer_age_s = 1 };
    v.r.seq = seq;
    v.r.total = total;
    v.r.unacked = unacked;
    v.r.n = total < AL_MAX ? total : AL_MAX;
    for (int i = 0; i < v.r.n; i++) {
        snprintf(v.r.a[i].id, sizeof(v.r.a[i].id), "WF-%d", 100 + i);
        strcpy(v.r.a[i].sev, i ? "P2" : "P1");
        v.r.a[i].acked = i >= unacked;
        snprintf(v.r.a[i].text, sizeof(v.r.a[i].text), "text %d", i);
    }
    return v;
}

static void decision_tests(void)
{
    /* take over: the panel-LED model (10-07) */
    al_view_t v = live(2, 1, 5);
    CHECK(al_take_over(&v, false, true, 0), "unacked -> takes over from CLOCK at once");
    CHECK(!al_take_over(&v, false, false, AL_FROM_WATCH_MS - 1), "unacked, WATCH touched recently -> wait");
    CHECK(al_take_over(&v, false, false, AL_FROM_WATCH_MS), "unacked, WATCH left alone 15 s -> take over");
    CHECK(!al_take_over(&v, true, true, 999999), "never over an anchor ALARM");
    al_view_t acked = live(2, 0, 6);
    CHECK(!al_take_over(&acked, false, true, AL_CLOCK_HOLD_MS - 1), "acked only, CLOCK touched recently -> CLOCK holds");
    CHECK(al_take_over(&acked, false, true, AL_CLOCK_HOLD_MS), "acked only, CLOCK untouched 2 min -> ALERT again");
    CHECK(!al_take_over(&acked, false, false, 999999), "acked only never takes over from WATCH");
    al_view_t gone = live(0, 0, 7);
    CHECK(!al_take_over(&gone, false, true, 999999), "nothing active -> CLOCK");
    al_view_t nolink = live(2, 1, 5);
    nolink.link = false;
    CHECK(!al_take_over(&nolink, false, true, 999999), "no link -> never on stale data");

    /* release: only when CLEARED (acked ones stay, like a steady panel LED) */
    CHECK(!al_release(&v), "unacked -> stays");
    CHECK(!al_release(&acked), "acked but active -> STAYS (panel-LED model)");
    CHECK(al_release(&gone), "all cleared -> leaves");
    CHECK(al_release(&nolink), "no link -> leaves (the CLOCK says NO LINK)");

    /* tap anywhere */
    CHECK(al_tap(&v) == AL_TAP_ACK, "unacked + token + link -> ACK");
    al_view_t sending = v;
    sending.ack_pending = true;
    CHECK(al_tap(&sending) == AL_TAP_NONE, "ACK on its way -> a second tap does nothing");
    CHECK(al_tap(&acked) == AL_TAP_CLOCK, "all acked -> CLOCK (night-watch bar; tap again = WATCH)");
    al_view_t notoken = v;
    notoken.token = false;
    CHECK(al_tap(&notoken) == AL_TAP_CLOCK, "no token -> cannot ack, CLOCK");
    CHECK(al_tap(&nolink) == AL_TAP_CLOCK, "no link -> cannot ack, CLOCK");

    /* footer */
    char line[400];
    al_footer(&v, line, sizeof(line));
    CHECK(!strcmp(line, "TAP ANYWHERE TO ACK"), "footer unacked: '%s'", line);
    al_footer(&sending, line, sizeof(line));
    CHECK(!strcmp(line, "sending ACK ..."), "footer sending: '%s'", line);
    al_view_t failed = v;
    strcpy(failed.err, "ACK not sent: token refused (401)");
    al_footer(&failed, line, sizeof(line));
    CHECK(strstr(line, "401") != NULL, "footer shows why an ACK failed: '%s'", line);
    al_footer(&acked, line, sizeof(line));
    CHECK(strstr(line, "All acked") && strstr(line, "until it clears"), "footer acked: '%s'", line);
    al_footer(&notoken, line, sizeof(line));
    CHECK(strstr(line, "ACK / MUTE button") != NULL, "footer no token -> the button: '%s'", line);
    al_footer(&nolink, line, sizeof(line));
    CHECK(strstr(line, "NO LINK") != NULL, "footer no link: '%s'", line);

    /* CLOCK + WATCH lines */
    al_clock_line(&v, line, sizeof(line));
    CHECK(!strcmp(line, "2 ALERTS - 1 not acked - tap here"), "clock line unacked: '%s'", line);
    al_clock_line(&acked, line, sizeof(line));
    CHECK(!strcmp(line, "2 alerts (acked) - tap here"), "clock line acked: '%s'", line);
    al_view_t one = live(1, 0, 1);
    al_clock_line(&one, line, sizeof(line));
    CHECK(!strcmp(line, "1 alert (acked) - tap here"), "clock line singular: '%s'", line);
    al_clock_line(&gone, line, sizeof(line));
    CHECK(line[0] == 0, "nothing active -> no clock line");
    al_clock_line(&nolink, line, sizeof(line));
    CHECK(strstr(line, "NO LINK") != NULL, "no link -> NO LINK line, never blank");
    al_view_t first = { .wifi = true };
    al_clock_line(&first, line, sizeof(line));
    CHECK(line[0] == 0, "not polled yet -> quiet (no false NO LINK at boot)");
    al_view_t nowifi = { 0 };
    al_clock_line(&nowifi, line, sizeof(line));
    CHECK(line[0] == 0, "no Wi-Fi built in -> quiet");

    al_watch_line(&v, line, sizeof(line));
    CHECK(!strcmp(line, "ENGINE P1 WF-100: text 0"), "watch line = first unacked: '%s'", line);
    al_view_t second = live(2, 1, 5);
    second.r.a[0].acked = true;
    second.r.a[1].acked = false;
    second.r.a[1].sim = true;
    al_watch_line(&second, line, sizeof(line));
    CHECK(!strcmp(line, "ENGINE P2 TEST WF-101: text 1"), "watch line skips acked, marks TEST: '%s'", line);
    al_watch_line(&acked, line, sizeof(line));
    CHECK(line[0] == 0, "all acked -> no watch line");
    al_watch_line(&nolink, line, sizeof(line));
    CHECK(line[0] == 0, "no link -> no watch line (the clock says NO LINK)");

    al_age(42, line, sizeof(line));
    CHECK(!strcmp(line, "42 s ago"), "age s");
    al_age(600, line, sizeof(line));
    CHECK(!strcmp(line, "10 min ago"), "age min");
    al_age(3 * 3600 + 5 * 60, line, sizeof(line));
    CHECK(!strcmp(line, "3 h 05 min ago"), "age h");
}

/* v3: crew RESET (Ship's Automation D-163) - the engine (api.py v13+) adds "rst": true to an alert the box may reset
 * now (box_reset on, clear: manual, input normal); absent otherwise. The 409 body is FastAPI's {"detail": "..."}. */
static void reset_tests(void)
{
    static const char *RST =
        "{\"seq\":9,\"total\":2,\"unacked\":1,\"sounding\":false,\"alerts\":["
        "{\"id\":\"WF-101\",\"sev\":\"P1\",\"acked\":false,\"sim\":false,\"age_s\":12,\"text\":\"high water salon\"},"
        "{\"id\":\"WF-104\",\"sev\":\"P1\",\"acked\":true,\"sim\":false,\"age_s\":600,\"rst\":true,\"text\":\"Fireboy\"}]}";
    al_view_t v;
    memset(&v, 0, sizeof(v));
    CHECK(al_parse(RST, &v.r), "parse with rst");
    CHECK(!v.r.a[0].rst && v.r.a[1].rst, "rst read per alert (absent = false)");
    v.wifi = v.polled = v.link = v.token = true;
    CHECK(al_reset_index(&v) == 1, "first resettable = WF-104 (index 1)");
    CHECK(al_reset_ok(&v, "WF-104") && !al_reset_ok(&v, "WF-101") && !al_reset_ok(&v, ""), "reset_ok by id");
    v.token = false;
    CHECK(al_reset_index(&v) == -1 && !al_reset_ok(&v, "WF-104"), "no token -> no RESET offered");
    v.token = true;
    v.link = false;
    CHECK(al_reset_index(&v) == -1 && !al_reset_ok(&v, "WF-104"), "no link -> no RESET offered");
    v.link = true;
    v.ack_pending = true;
    CHECK(al_reset_index(&v) == -1, "an ACK on its way -> no RESET button");
    v.ack_pending = false;
    v.reset_pending = true;
    strcpy(v.reset_id, "WF-104");
    CHECK(al_reset_index(&v) == -1, "a RESET on its way -> no second one");
    char line[160];
    al_footer(&v, line, sizeof(line));
    CHECK(!strcmp(line, "sending RESET WF-104 ..."), "footer while sending RESET: '%s'", line);
    v.reset_pending = false;
    strcpy(v.err, "RESET refused: WF-104: input still active - clear the cause first");
    al_footer(&v, line, sizeof(line));
    CHECK(strstr(line, "input still active") != NULL, "a refusal shows in the footer: '%s'", line);

    char why[80];
    CHECK(al_detail("{\"detail\":\"WF-104: input still active - clear the cause first\"}", why, sizeof(why)) &&
              !strcmp(why, "WF-104: input still active - clear the cause first"), "409 detail read: '%s'", why);
    CHECK(al_detail("{\"x\": 1, \"detail\": \"say \\\"why\\\"\"}", why, sizeof(why)) && !strcmp(why, "say \"why\""),
          "detail after another key, escapes: '%s'", why);
    CHECK(!al_detail("{\"x\": 1}", why, sizeof(why)) && !al_detail("not json", why, sizeof(why)) &&
              !al_detail(NULL, why, sizeof(why)), "no detail -> false");
}

int main(void)
{
    parse_tests();
    decision_tests();
    reset_tests();
    printf("alerts: %d cases, %d failed\n", n, fails);
    return fails ? 1 : 0;
}
