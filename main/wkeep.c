/**
 * Purpose: Watchkeeper check-in on the box - reply parsing and what to show; see wkeep.h (ANQ-27, Ship's Automation
 *   WF-614 / D-161).
 * Version: v1 (2026-10-21)
 * Scope: PRODUCTION - anchor-box firmware (live on the box at the nav station, installed by signed OTA).
 * Architecture: anchor-box firmware, logic layer. A small member lookup for the engine's one flat reply object
 *   (same reader shape as restock.c); never shows a state the engine has not reported.
 * Invocation: imported by wkeep_ctl.c, ui_wkup.c, screens.c, test_host/test_wkeep.c.
 */
#include "wkeep.h"

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

/* a whole number; null / missing -> dflt, returns false only for a malformed value */
static bool get_int(const char *v, int dflt, int *out)
{
    *out = dflt;
    if (!v || strncmp(v, "null", 4) == 0) return true;
    if (!((*v >= '0' && *v <= '9') || *v == '-')) return false;
    char *e;
    long x = strtol(v, &e, 10);
    if (e == v) return false;
    *out = (int)x;
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

bool wk_parse(const char *body, wk_reply_t *out)
{
    memset(out, 0, sizeof(*out));
    out->since_up_s = out->next_prompt_s = -1;
    if (!body) return false;
    const char *o = ws(body);
    if (*o != '{') return false;
    if (!get_bool(member(o, "on"), &out->on)) return false;
    const char *st = member(o, "stage");
    if (!st || !get_int(st, 0, &out->stage) || out->stage < 0 || out->stage > 3) return false;
    if (!get_int(member(o, "since_up_s"), -1, &out->since_up_s)) return false;
    if (!get_int(member(o, "next_prompt_s"), -1, &out->next_prompt_s)) return false;
    if (!get_int(member(o, "alarm_s"), 1500, &out->alarm_s)) return false;
    get_str(member(o, "mode"), out->mode, sizeof(out->mode));
    if (!out->on) out->stage = 0;                /* a stopped timer asks nothing, whatever it says */
    return true;
}

bool wk_detail(const char *body, char *buf, size_t n)
{
    buf[0] = 0;
    if (!body) return false;
    get_str(member(ws(body), "detail"), buf, n);
    return buf[0] != 0;
}

bool wk_due(const wk_view_t *v)
{
    return v->link && v->eng.on && v->eng.stage >= 1;
}

bool wk_take_over(const wk_view_t *v, bool anchor_alarm, bool on_clock, uint32_t idle_ms)
{
    if (anchor_alarm || !wk_due(v)) return false;
    return on_clock || idle_ms >= WK_FROM_WATCH_MS;
}

bool wk_release(const wk_view_t *v)
{
    if (!v->link) return true;
    return !wk_due(v) && !v->pending && !v->msg[0];
}

bool wk_tap(const wk_view_t *v)
{
    return v->link && v->token && v->eng.on && !v->pending;
}

void wk_head(const wk_view_t *v, char *buf, size_t n)
{
    int s = v->eng.stage;
    if (s >= 3) snprintf(buf, n, "WATCHKEEPER NOT CHECKING IN");
    else if (s == 2) snprintf(buf, n, "WATCH CHECK-IN - SECOND CALL");
    else snprintf(buf, n, "WATCH CHECK-IN");
}

void wk_text(const wk_view_t *v, char *buf, size_t n)
{
    int m = v->eng.since_up_s >= 0 ? v->eng.since_up_s / 60 : 0;
    int alarm_m = (v->eng.alarm_s > 0 ? v->eng.alarm_s : 1500) / 60;
    if (v->eng.stage >= 3)
        snprintf(buf, n, "%d min since the last I'm up. Check the cockpit and the watchkeeper NOW.", m);
    else
        snprintf(buf, n, "%d min since the last I'm up.\nAt %d min the alarm wakes everyone.", m, alarm_m);
}

void wk_footer(const wk_view_t *v, char *buf, size_t n)
{
    if (v->pending) snprintf(buf, n, "sending I'M UP ...");
    else if (v->msg[0]) snprintf(buf, n, "%s", v->msg);
    else if (!v->link) snprintf(buf, n, "no link to the rules engine");
    else if (!v->token) snprintf(buf, n, "no token on this box - tap I'M UP on the cockpit tablet");
    else snprintf(buf, n, "TAP ANYWHERE: I'M UP");
}

void wk_post_body(char *buf, size_t n)
{
    snprintf(buf, n, "{\"action\": \"up\", \"by\": \"anchor-box\"}");
}
