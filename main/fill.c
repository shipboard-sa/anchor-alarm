/**
 * Purpose: Tank-fill taps - reply parsing and the FILL page / tab faces; see fill.h (ANC-41, D-129).
 * Version: v1 (2026-10-09)
 * Scope: PRODUCTION - anchor-box firmware (live on the box at the nav station, installed by signed OTA).
 * Architecture: anchor-box firmware, logic layer. A small nested-JSON member walker (the reply is objects inside
 *   objects, so the flat key lookup of nightwatch.c is not enough: a key is only taken at its own object's level,
 *   never from inside a string or a deeper object). Never shows a state the engine has not reported.
 * Invocation: imported by fill_ctl.c, ui_fill.c, ui_watch.c and test_host/test_fill.c.
 */
#include "fill.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *const FL_KEYS[FL_TANKS] = { "water_port", "water_stbd", "fuel_port", "fuel_stbd" };

static const char *ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

/* p at '"': the char after the closing quote, NULL if unterminated */
static const char *skip_str(const char *p)
{
    for (p++; *p; p++) {
        if (*p == '\\' && p[1]) p++;
        else if (*p == '"') return p + 1;
    }
    return NULL;
}

/* p at a value: the char after it, NULL if malformed */
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
    const char *s = p;                         /* number / true / false / null */
    while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
    return p > s ? p : NULL;
}

/* obj at '{': the value of member key at THIS object's level, NULL if absent / malformed */
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
        if (*p != ',') return NULL;            /* '}' (end) or junk */
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
    if (!v || *v != '"') return;               /* null / missing -> "" */
    size_t k = 0;
    for (v++; *v && *v != '"' && k + 1 < n; v++) {
        if (*v == '\\' && v[1]) v++;           /* keep the escaped char, drop the backslash */
        out[k++] = (*v >= 0x20 && *v < 0x7f) ? *v : '?';
    }
    out[k] = 0;
}

bool fl_parse(const char *body, fl_reply_t *out)
{
    *out = (fl_reply_t){ 0 };
    if (!body) return false;
    const char *root = ws(body);
    const char *tanks = member(root, "tanks");
    if (!tanks || *tanks != '{') return false;
    int found = 0;
    for (int i = 0; i < FL_TANKS; i++) {
        const char *t = member(tanks, FL_KEYS[i]);
        if (!t || *t != '{' || !skip_val(t)) continue;   /* missing, or cut off (body buffer full) */
        fl_tank_t *k = &out->t[i];
        k->present = true;
        found++;
        get_str(member(t, "label"), k->label, sizeof(k->label));
        if (!k->label[0]) strncpy(k->label, FL_KEYS[i], sizeof(k->label) - 1);
        k->has_level = get_num(member(t, "level"), &k->level);
        const char *s = member(t, "session");
        if (s && *s == '{') {
            char src[16];
            get_str(member(s, "source"), src, sizeof(src));
            k->src = strcmp(src, "watermaker") == 0 ? FL_SRC_WATERMAKER : FL_SRC_TAP;   /* unknown -> a tap */
            k->active = k->src == FL_SRC_TAP;  /* the engine's default; its "active" overrides */
            get_bool(member(s, "active"), &k->active);
            get_str(member(s, "since"), k->since, sizeof(k->since));
            get_str(member(s, "by"), k->by, sizeof(k->by));
        }
    }
    if (!found) return false;
    const char *wm = member(root, "watermaker");
    if (wm && *wm == '{') {
        get_bool(member(wm, "running"), &out->wm_running);
        out->wm_has_watts = get_num(member(wm, "watts"), &out->wm_watts);
    }
    return true;
}

static void level_txt(const fl_tank_t *t, char *buf, size_t n)
{
    if (t->has_level) snprintf(buf, n, "%.0f %%", t->level);
    else snprintf(buf, n, "level ?");
}

void fl_row(const fl_view_t *v, int i, fl_row_t *r)
{
    *r = (fl_row_t){ 0 };
    if (i < 0 || i >= FL_TANKS || !v->link || !v->eng.t[i].present) return;
    const fl_tank_t *t = &v->eng.t[i];
    char lv[16];
    level_txt(t, lv, sizeof(lv));
    r->shown = true;
    snprintf(r->name, sizeof(r->name), "%s", t->label);
    /* the row holds ~32 capitals at 28 px: a longer label loses its trailing "(...)" first
     * ("STARBOARD FRESH WATER (starboard string)" -> "STARBOARD FRESH WATER") */
    char *paren = strrchr(r->name, '(');
    size_t len = strlen(r->name);
    if (len > FL_NAME_FIT && paren && paren > r->name && r->name[len - 1] == ')') {
        while (paren > r->name && paren[-1] == ' ') paren--;
        *paren = 0;
    }
    switch (t->src) {
    case FL_SRC_NONE:
        snprintf(r->state, sizeof(r->state), "%s   not filling", lv);
        break;
    case FL_SRC_TAP:
        r->filling = true;
        snprintf(r->state, sizeof(r->state), "%s   FILLING - started by %s", lv, t->by[0] ? t->by : "the crew");
        break;
    case FL_SRC_WATERMAKER:
        r->filling = t->active;
        snprintf(r->state, sizeof(r->state), "%s   %s", lv,
                 t->active ? "FILLING from the watermaker" : "watermaker running - not rising here");
        break;
    }
    bool on;
    if (v->pending && v->pending_tank == i) {
        snprintf(r->btn, sizeof(r->btn), "sending ...");
    } else if (t->src == FL_SRC_WATERMAKER) {
        snprintf(r->btn, sizeof(r->btn), "watermaker");
    } else if (fl_tap(v, i, &on)) {
        r->can_tap = true;
        r->tap_on = on;
        snprintf(r->btn, sizeof(r->btn), on ? "FILLING" : "STOP");
    } else {
        snprintf(r->btn, sizeof(r->btn), "%s", t->src == FL_SRC_TAP ? "STOP" : "FILLING");   /* drawn dark */
    }
}

bool fl_tap(const fl_view_t *v, int i, bool *on)
{
    if (i < 0 || i >= FL_TANKS || !v->link || !v->token || v->pending) return false;
    const fl_tank_t *t = &v->eng.t[i];
    if (!t->present || t->src == FL_SRC_WATERMAKER) return false;
    *on = t->src == FL_SRC_NONE;               /* the opposite of what the ENGINE last said */
    return true;
}

void fl_head(const fl_view_t *v, char *buf, size_t n)
{
    buf[0] = 0;
    if (!v->link) return;
    if (!v->eng.wm_running) snprintf(buf, n, "Watermaker off");
    else if (v->eng.wm_has_watts) snprintf(buf, n, "Watermaker RUNNING %.0f W", v->eng.wm_watts);
    else snprintf(buf, n, "Watermaker RUNNING");
}

void fl_footer(const fl_view_t *v, char *buf, size_t n)
{
    if (!v->wifi) snprintf(buf, n, "No link: no Wi-Fi on this box");
    else if (!v->polled) snprintf(buf, n, "asking the rules engine ...");
    else if (!v->link && v->get_http == 404) snprintf(buf, n, "No link: the rules engine has no fill sessions yet (404)");
    else if (!v->link) snprintf(buf, n, "No link: the rules engine is not answering");
    else if (v->pending) snprintf(buf, n, "sending %s ...", v->pending_on ? "FILLING" : "STOP");
    else if (v->err[0]) snprintf(buf, n, "%s", v->err);
    else if (!v->token) snprintf(buf, n, "no token - can't change it here (cockpit page)");
    else snprintf(buf, n, "FILLING: chirps at 90 %% and full (95 %%) - ends by itself");
}

int fl_tab(const fl_view_t *v, char *small, size_t n)
{
    small[0] = 0;
    if (!v->link) return 0;
    int on = 0;
    for (int i = 0; i < FL_TANKS; i++)
        if (v->eng.t[i].present && v->eng.t[i].src != FL_SRC_NONE && v->eng.t[i].active) on++;
    if (on == 1) snprintf(small, n, "ON");
    else if (on > 1) snprintf(small, n, "%d ON", on);
    return on;
}
