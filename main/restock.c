/**
 * Purpose: Cold-box 'EXPECTED?' question - reply parsing and what the screen shows; see restock.h (ANC-45, Ship's
 *   Automation WF-502 / D-128).
 * Version: v2 (2026-10-18) - v2: YES / NO only (10-18): from CLOCK at once, no 'tap elsewhere' in the footer. v1 (2026-10-18)
 * Scope: PRODUCTION - anchor-box firmware (live on the box at the nav station, installed by signed OTA).
 * Architecture: anchor-box firmware, logic layer. The same small nested-JSON member walker as boatmode.c (a key is
 *   only taken at its own object's level, never from inside a string or a deeper object) - each logic module keeps
 *   its own copy so it can be host-tested alone. Never asks a question the engine has not reported.
 * Invocation: imported by restock_ctl.c, ui_ask.c, screens.c and test_host/test_restock.c.
 */
#include "restock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *const RS_KEYS[RS_BOXES] = { "fridge", "engel", "fz_aft", "fz_fwd" };

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
    if (!v || !((*v >= '0' && *v <= '9') || *v == '-')) return false;
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

bool rs_parse(const char *body, rs_reply_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!body) return false;
    const char *o = ws(body);
    const char *boxes = member(o, "boxes");
    if (!boxes || *boxes != '{') return false;
    if (!get_num(member(o, "hold_h"), &out->hold_h) || out->hold_h <= 0) return false;
    int known = 0;
    for (int i = 0; i < RS_BOXES; i++) {
        const char *b = member(boxes, RS_KEYS[i]);
        rs_box_t *x = &out->b[i];
        if (!b || *b != '{') continue;
        if (!get_bool(member(b, "asking"), &x->asking) || !get_bool(member(b, "alarm"), &x->alarm)) {
            x->asking = x->alarm = false;          /* half a box is no box: never ask from it */
            continue;
        }
        get_str(member(b, "label"), x->label, sizeof(x->label));
        if (!x->label[0]) strncpy(x->label, RS_KEYS[i], sizeof(x->label) - 1);
        const char *m = member(b, "mark");
        if (m && *m == '{') {
            char kind[12];
            get_str(member(m, "kind"), kind, sizeof(kind));
            x->mark = strcmp(kind, "denied") == 0 ? RS_MARK_DENIED : RS_MARK_HOLD;   /* any other mark = answered */
        }
        x->known = true;
        known++;
    }
    return known > 0;
}

bool rs_detail(const char *body, char *buf, size_t n)
{
    buf[0] = 0;
    if (!body) return false;
    get_str(member(ws(body), "detail"), buf, n);
    return buf[0] != 0;
}

int rs_asking(const rs_view_t *v, int *count)
{
    int first = -1, c = 0;
    if (v->link) {
        for (int i = 0; i < RS_BOXES; i++) {
            const rs_box_t *b = &v->eng.b[i];
            if (b->known && b->asking && !b->alarm && b->mark == RS_MARK_NONE) {
                if (first < 0) first = i;
                c++;
            }
        }
    }
    if (count) *count = c;
    return first;
}

bool rs_take_over(const rs_view_t *v, bool anchor_alarm, bool on_clock, uint32_t idle_ms)
{
    if (anchor_alarm || rs_asking(v, NULL) < 0) return false;
    return on_clock || idle_ms >= RS_FROM_WATCH_MS;
}

bool rs_release(const rs_view_t *v)
{
    if (!v->link) return true;
    return rs_asking(v, NULL) < 0 && !v->pending && !v->msg[0];
}

bool rs_tap(const rs_view_t *v, int *box)
{
    if (!v->token || v->pending) return false;
    int b = rs_asking(v, NULL);
    if (b < 0) return false;
    *box = b;
    return true;
}

void rs_question(const rs_view_t *v, int box, char *buf, size_t n)
{
    if (box < 0 || box >= RS_BOXES) {
        buf[0] = 0;
        return;
    }
    snprintf(buf, n, "%s warm 10 min - warm food just went in?", v->eng.b[box].label);
}

void rs_yes_label(const rs_view_t *v, char *buf, size_t n)
{
    snprintf(buf, n, "YES\ngive it %g h", v->eng.hold_h > 0 ? v->eng.hold_h : 2.0);
}

void rs_no_label(char *buf, size_t n)
{
    snprintf(buf, n, "NO\nalarm now");
}

void rs_footer(const rs_view_t *v, int count, char *buf, size_t n)
{
    if (v->pending) {
        const char *l = v->pending_box >= 0 && v->pending_box < RS_BOXES ? v->eng.b[v->pending_box].label : "";
        snprintf(buf, n, "sending %s for the %s ...", v->pending_yes ? "YES" : "NO", l);
    } else if (v->msg[0]) {
        snprintf(buf, n, "%s", v->msg);
    } else if (!v->link) {
        snprintf(buf, n, "no link to the rules engine");
    } else if (!v->token) {
        snprintf(buf, n, "no token on this box - answer on the cockpit page");
    } else if (count > 1) {
        snprintf(buf, n, "one tap answers  -  %d more box%s asking after this", count - 1,
                 count > 2 ? "es" : "");
    } else {
        snprintf(buf, n, "one tap answers  -  no answer: the alarm comes at 30 min as before");
    }
}

void rs_post_body(int box, bool yes, char *buf, size_t n)
{
    snprintf(buf, n, "{\"box\": \"%s\", \"answer\": \"%s\", \"by\": \"anchor-box\"}",
             box >= 0 && box < RS_BOXES ? RS_KEYS[box] : "", yes ? "yes" : "no");
}
