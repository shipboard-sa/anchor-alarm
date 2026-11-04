/**
 * Purpose: Host (PC) test of the HOT WATER tap logic (main/hotwater.c, ANC-43 / Ship's Automation D-135): parsing the
 *   rules engine's GET / POST /hot-water reply and its 409 refusal, WATCH's button face, what a tap may send and the
 *   notice line, in every state.
 * Version: v1 (2026-10-11)
 * Scope: STANDALONE UTILITY - anchor-box test tooling.
 * Architecture: compiled by test_host/run.bat (zig cc) with main/hotwater.c; fixed cases (replies captured from the
 *   engine's builtins/hot_water.py view() on 2026-10-11), no vectors - the run itself lives in the engine.
 * Invocation: test_host\run.bat (from code\anchor-box). Exit 0 = all cases pass.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "../main/hotwater.h"

static int fails;

#define CHECK(cond, ...)                                                                                              \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("hotwater FAIL: " __VA_ARGS__);                                                                     \
            printf("\n");                                                                                              \
            fails++;                                                                                                   \
        }                                                                                                              \
    } while (0)

static const char *OFF =
    "{\"on\": false, \"state\": \"off\", \"remaining_s\": 0, \"since\": null, \"by\": null, \"amps\": null, "
    "\"soc\": null, \"soc_floor\": 50.0, \"max_s\": 1200, \"last\": null}";
static const char *HEATING =
    "{\"on\": true, \"state\": \"heating\", \"remaining_s\": 1170, \"since\": \"1970-01-12T13:46:40Z\", "
    "\"by\": \"api:anchor-box\", \"amps\": 16.62, \"soc\": 72.4, \"soc_floor\": 50.0, \"max_s\": 1200, \"last\": null}";
static const char *ENDED_HOT =
    "{\"on\": false, \"state\": \"off\", \"remaining_s\": 0, \"since\": null, \"by\": null, \"amps\": 0.1, "
    "\"soc\": 71.0, \"soc_floor\": 50.0, \"max_s\": 1200, \"last\": {\"why\": \"tank hot (thermostat off)\", "
    "\"result\": \"hot\", \"at\": \"1970-01-12T13:50:00Z\", \"by\": \"\", \"minutes\": 3.3, \"age_s\": 60}}";
static const char *REFUSED =
    "{\"detail\": \"house bank 46 % - below the 50 % floor for the heater; use the kettle\"}";

static hw_view_t base(const char *reply)
{
    hw_view_t v;
    memset(&v, 0, sizeof(v));
    v.wifi = v.token = v.polled = v.link = true;
    v.get_http = 200;
    if (reply && !hw_parse(reply, &v.eng)) printf("hotwater: base reply did not parse\n");
    return v;
}

static int parse_tests(void)
{
    int n = 0;
    hw_reply_t r;
    char d[80];

    n++;
    CHECK(hw_parse(OFF, &r) && !r.on && r.state == HW_OFF && !r.has_amps && !r.has_soc && r.last == HW_LAST_NONE &&
              r.last_age_s == -1 && r.soc_floor == 50.0f, "off reply");
    n++;
    CHECK(hw_parse(HEATING, &r) && r.on && r.state == HW_HEATING && r.remaining_s == 1170 && r.has_amps &&
              r.amps > 16.6f && r.has_soc && r.soc > 72.3f, "heating reply");
    n++;
    CHECK(hw_parse(ENDED_HOT, &r) && !r.on && r.last == HW_LAST_HOT && r.last_age_s == 60, "ended-hot reply");
    n++;
    CHECK(!hw_parse(REFUSED, &r), "a 409 body is not a state reply");
    n++;
    CHECK(hw_detail(REFUSED, d, sizeof(d)) && strstr(d, "below the 50 % floor") && strstr(d, "kettle"), "409 detail");
    n++;
    CHECK(!hw_parse("{\"on\": true, \"state\": \"boiling\"}", &r), "unknown state refused");
    n++;
    CHECK(!hw_parse("{\"state\": \"off\"}", &r) && !hw_parse("", &r) && !hw_parse(NULL, &r), "missing on / empty");
    n++;
    CHECK(hw_parse("{\"on\": true, \"state\": \"held\", \"remaining_s\": 600, \"last\": {\"result\": \"x\\\"}\"}}", &r) &&
              r.state == HW_HELD && r.remaining_s == 600, "held + an escaped quote inside a string");
    n++;
    CHECK(hw_parse("{\"note\": {\"on\": false, \"state\": \"off\"}, \"on\": true, \"state\": \"on_no_draw\"}", &r) &&
              r.on && r.state == HW_ON_NO_DRAW, "keys only at the top level, never from a nested object");
    return n;
}

static int face_tests(void)
{
    int n = 0;
    hw_view_t v;
    hw_button_t b;
    char s[128];
    bool on;

    v = base(OFF);
    hw_button(&v, &b);
    n++;
    CHECK(!strcmp(b.top, "HOT") && !strcmp(b.bottom, "WATER") && b.enabled && !b.active, "off face");
    n++;
    CHECK(hw_tap(&v, &on) && on, "off: a tap starts");
    hw_notice(&v, s, sizeof(s));
    n++;
    CHECK(s[0] == 0, "off: notice line free");

    v = base(HEATING);
    hw_button(&v, &b);
    n++;
    CHECK(!strcmp(b.bottom, "20 min") && b.active, "heating: 1170 s -> '20 min' (rounded up), heavier");
    v.reply_age_s = 600;
    hw_button(&v, &b);
    n++;
    CHECK(!strcmp(b.bottom, "10 min"), "counts down between polls (%s)", b.bottom);
    v.reply_age_s = 5000;
    hw_button(&v, &b);
    n++;
    CHECK(!strcmp(b.bottom, "1 min"), "never below 1 min while the engine still says on");
    v.reply_age_s = 0;
    n++;
    CHECK(hw_tap(&v, &on) && !on, "on: a second tap stops");
    hw_notice(&v, s, sizeof(s));
    n++;
    CHECK(strstr(s, "heating - 20 min left") && strstr(s, "ends when the tank is hot"), "heating notice: %s", s);

    v.eng.state = HW_HELD;
    hw_button(&v, &b);
    hw_notice(&v, s, sizeof(s));
    n++;
    CHECK(!strcmp(b.bottom, "held") && strstr(s, "held off by the boat"), "held: %s / %s", b.bottom, s);
    v.eng.state = HW_ON_NO_DRAW;
    hw_notice(&v, s, sizeof(s));
    n++;
    CHECK(strstr(s, "not drawing yet"), "on, no draw: %s", s);

    v = base(ENDED_HOT);
    hw_button(&v, &b);
    hw_notice(&v, s, sizeof(s));
    n++;
    CHECK(!strcmp(b.bottom, "READY") && !b.active && strstr(s, "ready - the tank is hot"), "ended hot: %s", s);
    v.reply_age_s = HW_SAY_END_S;            /* 60 s + 10 min: notice gone, READY still on (30 min) */
    hw_button(&v, &b);
    hw_notice(&v, s, sizeof(s));
    n++;
    CHECK(!strcmp(b.bottom, "READY") && s[0] == 0, "after 10 min: READY, notice free");
    v.reply_age_s = HW_READY_S;
    hw_button(&v, &b);
    n++;
    CHECK(!strcmp(b.bottom, "WATER"), "after 30 min: back to HOT WATER");

    v = base(ENDED_HOT);
    v.eng.last = HW_LAST_NO_DRAW;
    hw_button(&v, &b);
    hw_notice(&v, s, sizeof(s));
    n++;
    CHECK(!strcmp(b.bottom, "WATER") && strstr(s, "never drew") && strstr(s, "breaker"), "no draw: %s", s);
    v.eng.last = HW_LAST_STOPPED;
    hw_notice(&v, s, sizeof(s));
    n++;
    CHECK(s[0] == 0, "stopped by a tap: nothing more to say");

    v = base(OFF);
    v.pending = true;
    v.pending_on = true;
    hw_button(&v, &b);
    hw_notice(&v, s, sizeof(s));
    n++;
    CHECK(!strcmp(b.bottom, "...") && strstr(s, "sending start") && !hw_tap(&v, &on), "pending: %s", s);

    v = base(OFF);
    hw_detail(REFUSED, v.msg, sizeof(v.msg));
    hw_notice(&v, s, sizeof(s));
    n++;
    CHECK(!strncmp(s, "HOT WATER: house bank 46 %", 26), "refusal shown: %s", s);

    v = base(HEATING);
    v.link = false;
    hw_button(&v, &b);
    hw_notice(&v, s, sizeof(s));
    n++;
    CHECK(!strcmp(b.bottom, "WATER") && !b.active && s[0] == 0 && !hw_tap(&v, &on),
          "no link: never claims a run, no tap");
    v = base(OFF);
    v.token = false;
    hw_button(&v, &b);
    n++;
    CHECK(!b.enabled && !hw_tap(&v, &on), "no token: button disabled");
    v = base(OFF);
    v.wifi = false;
    hw_button(&v, &b);
    n++;
    CHECK(!b.enabled, "no Wi-Fi: button disabled");
    return n;
}

int main(void)
{
    int n = parse_tests() + face_tests();
    printf("hotwater: %d cases, %d failed\n", n, fails);
    return fails ? 1 : 0;
}
