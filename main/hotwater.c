/**
 * Purpose: HOT WATER tap - reply parsing and the button / notice faces; see hotwater.h (ANC-43, Ship's Automation D-135).
 * Version: v1 (2026-10-11)
 * Scope: PRODUCTION - anchor-box firmware (live on the box at the nav station, installed by signed OTA).
 * Architecture: anchor-box firmware, logic layer. The same small nested-JSON member walker as fill.c (a key is only
 *   taken at its own object's level, never from inside a string or a deeper object) - each logic module keeps its
 *   own copy so it can be host-tested alone. Never shows a state the engine has not reported.
 * Invocation: imported by hotwater_ctl.c, ui_watch.c and test_host/test_hotwater.c.
 */
#include "hotwater.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

static const char *skip_str(const char *p)
{
    for (p++; *p; p++) {
        if (*p == '\\' && p[1]) p++;
        else if (*p == '"') return p + 1;
    }
    return NULL;
}

static const char *skip_val(const char *p)
{
    if (*p == '"') return skip_str(p);
    if (*p == '{' || *p == '[') {
        int depth = 0;
        while (*p) {
            if (*p == '"') {
                p = skip_str(p);
                if (!p) return NULL;
                continue;
            }
            if (*p == '{' || *p == '[') depth++;
            else if (*p == '}' || *p == ']') {
                if (--depth == 0) return p + 1;
            }
            p++;
        }
        return NULL;
    }
    const char *s = p;
    while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
    return p > s ? p : NULL;
}

static const char *member(const char *obj, const char *key)
{
    if (!obj || *obj != '{') return NULL;
    size_t kl = strlen(key);
    const char *p = ws(obj + 1);
    while (*p == '"') {
        const char *k = p + 1, *ke = skip_str(p);
        if (!ke) return NULL;
        p = ws(ke);
        if (*p != ':') return NULL;
        p = ws(p + 1);
        if ((size_t)(ke - 1 - k) == kl && strncmp(k, key, kl) == 0) return p;
        p = skip_val(p);
        if (!p) return NULL;
        p = ws(p);
        if (*p != ',') return NULL;
        p = ws(p + 1);
    }
    return NULL;
}

static bool get_bool(const char *v, bool *out)
{
    if (!v) return false;
    if (strncmp(v, "true", 4) == 0) { *out = true; return true; }
    if (strncmp(v, "false", 5) == 0) { *out = false; return true; }
    return false;
}

static bool get_num(const char *v, float *out)
{
    if (!v || !(*v == '-' || (*v >= '0' && *v <= '9'))) return false;   /* null / string -> unknown */
    char *e;
    double d = strtod(v, &e);
    if (e == v) return false;
    *out = (float)d;
    return true;
}

static void get_str(const char *v, char *out, size_t n)
{
    out[0] = 0;
    if (!v || *v != '"') return;
    size_t k = 0;
    for (v++; *v && *v != '"' && k + 1 < n; v++) {
        if (*v == '\\' && v[1]) v++;
        out[k++] = (*v >= 0x20 && *v < 0x7f) ? *v : '?';
    }
    out[k] = 0;
}

bool hw_parse(const char *body, hw_reply_t *out)
{
    memset(out, 0, sizeof(*out));
    out->last_age_s = -1;
    out->soc_floor = 50;
    if (!body) return false;
    const char *o = ws(body);
    char s[16];
    get_str(member(o, "state"), s, sizeof(s));
    if (!get_bool(member(o, "on"), &out->on)) return false;
    if (strcmp(s, "off") == 0) out->state = HW_OFF;
    else if (strcmp(s, "heating") == 0) out->state = HW_HEATING;
    else if (strcmp(s, "on_no_draw") == 0) out->state = HW_ON_NO_DRAW;
    else if (strcmp(s, "held") == 0) out->state = HW_HELD;
    else return false;
    float f;
    if (get_num(member(o, "remaining_s"), &f) && f > 0) out->remaining_s = (int)f;
    out->has_amps = get_num(member(o, "amps"), &out->amps);
    out->has_soc = get_num(member(o, "soc"), &out->soc);
    if (get_num(member(o, "soc_floor"), &f)) out->soc_floor = f;
    const char *last = member(o, "last");
    if (last && *last == '{') {
        get_str(member(last, "result"), s, sizeof(s));
        if (strcmp(s, "hot") == 0) out->last = HW_LAST_HOT;
        else if (strcmp(s, "no_draw") == 0) out->last = HW_LAST_NO_DRAW;
        else if (strcmp(s, "time") == 0) out->last = HW_LAST_TIME;
        else if (strcmp(s, "stopped") == 0) out->last = HW_LAST_STOPPED;
        if (get_num(member(last, "age_s"), &f) && f >= 0) out->last_age_s = (int)f;
    }
    return true;
}

bool hw_detail(const char *body, char *buf, size_t n)
{
    buf[0] = 0;
    if (!body) return false;
    get_str(member(ws(body), "detail"), buf, n);
    return buf[0] != 0;
}

/* minutes left, counted down between polls; never below 1 while the engine still says on */
static int mins_left(const hw_view_t *v)
{
    int s = v->eng.remaining_s - (v->reply_age_s > 0 ? v->reply_age_s : 0);
    int m = (s + 59) / 60;
    return m < 1 ? 1 : m;
}

static bool ready(const hw_view_t *v)
{
    return v->link && !v->eng.on && v->eng.last == HW_LAST_HOT && v->eng.last_age_s >= 0 &&
           v->eng.last_age_s + (v->reply_age_s > 0 ? v->reply_age_s : 0) < HW_READY_S;
}

void hw_button(const hw_view_t *v, hw_button_t *b)
{
    memset(b, 0, sizeof(*b));
    snprintf(b->top, sizeof(b->top), "HOT");
    snprintf(b->bottom, sizeof(b->bottom), "WATER");
    b->enabled = v->wifi && v->token;
    if (v->pending) {
        snprintf(b->bottom, sizeof(b->bottom), "...");
        return;
    }
    if (!v->link) return;
    if (v->eng.on) {
        b->active = true;
        if (v->eng.state == HW_HELD) snprintf(b->bottom, sizeof(b->bottom), "held");
        else snprintf(b->bottom, sizeof(b->bottom), "%d min", mins_left(v));
    } else if (ready(v)) {
        snprintf(b->bottom, sizeof(b->bottom), "READY");
    }
}

bool hw_tap(const hw_view_t *v, bool *on)
{
    if (!v->link || !v->token || v->pending) return false;
    *on = !v->eng.on;                          /* the opposite of what the ENGINE last said */
    return true;
}

void hw_notice(const hw_view_t *v, char *buf, size_t n)
{
    buf[0] = 0;
    if (v->pending) {
        snprintf(buf, n, "HOT WATER: sending %s ...", v->pending_on ? "start" : "stop");
    } else if (v->msg[0]) {
        snprintf(buf, n, "HOT WATER: %s", v->msg);
    } else if (!v->link) {
        return;
    } else if (!v->eng.on) {                   /* how the last run ended, for 10 min; then the line is free */
        int age = v->eng.last_age_s < 0 ? -1 : v->eng.last_age_s + (v->reply_age_s > 0 ? v->reply_age_s : 0);
        if (age < 0 || age >= HW_SAY_END_S) return;
        if (v->eng.last == HW_LAST_HOT) snprintf(buf, n, "HOT WATER ready - the tank is hot");
        else if (v->eng.last == HW_LAST_NO_DRAW)
            snprintf(buf, n, "HOT WATER: heater never drew - tank already hot, or the heater breaker is off");
        else if (v->eng.last == HW_LAST_TIME) snprintf(buf, n, "HOT WATER: run ended at its time limit");
    } else if (v->eng.state == HW_HELD) {
        snprintf(buf, n, "HOT WATER held off by the boat (high water / inverter load) - %d min left", mins_left(v));
    } else if (v->eng.state == HW_HEATING) {
        snprintf(buf, n, "HOT WATER heating - %d min left, ends when the tank is hot", mins_left(v));
    } else {
        snprintf(buf, n, "HOT WATER on, heater not drawing yet - %d min left", mins_left(v));
    }
}
