/**
 * Purpose: DOCK / LEAVE taps - reply parsing and the WATCH faces; see boatmode.h (ANC-44, Ship's Automation D-151).
 * Version: v1 (2026-10-15)
 * Scope: PRODUCTION - anchor-box firmware (live on the box at the nav station, installed by signed OTA).
 * Architecture: anchor-box firmware, logic layer. The same small nested-JSON member walker as hotwater.c (a key is
 *   only taken at its own object's level, never from inside a string or a deeper object) - each logic module keeps
 *   its own copy so it can be host-tested alone. Never shows a mode the engine has not reported.
 * Invocation: imported by boatmode_ctl.c, ui_watch.c and test_host/test_boatmode.c.
 */
#include "boatmode.h"

#include <stdio.h>
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

/* entries of a JSON array (strings, numbers ...); -1 if v is not an array */
static int array_len(const char *v)
{
    if (!v || *v != '[') return -1;
    const char *p = ws(v + 1);
    if (*p == ']') return 0;
    int n = 0;
    while (*p) {
        p = skip_val(p);
        if (!p) return -1;
        n++;
        p = ws(p);
        if (*p == ']') return n;
        if (*p != ',') return -1;
        p = ws(p + 1);
    }
    return -1;
}

static const char *const NAMES[] = { "", "DOCK", "ANCHOR", "MOTOR", "SAIL", "AWAY" };

const char *bm_name(bm_mode_t m)
{
    return (m >= BM_NONE && m <= BM_AWAY) ? NAMES[m] : "";
}

bool bm_parse(const char *body, bm_reply_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!body) return false;
    const char *o = ws(body);
    if (!get_bool(member(o, "secured"), &out->secured)) return false;
    if (!get_bool(member(o, "leave_ok"), &out->leave_ok)) return false;
    const char *m = member(o, "mode");
    if (!m) return false;
    if (strncmp(m, "null", 4) == 0) {
        out->mode = BM_NONE;                    /* never tapped and nothing inferred */
    } else {
        char s[12];
        get_str(m, s, sizeof(s));
        int k;
        for (k = BM_DOCK; k <= BM_AWAY; k++)
            if (strcmp(s, NAMES[k]) == 0) break;
        if (k > BM_AWAY) return false;          /* a mode this box does not know: not a reply it can trust */
        out->mode = (bm_mode_t)k;
    }
    int c = array_len(member(o, "crew_aboard"));
    out->crew = c > 0 ? c : 0;
    return true;
}

bool bm_detail(const char *body, char *buf, size_t n)
{
    buf[0] = 0;
    if (!body) return false;
    get_str(member(ws(body), "detail"), buf, n);
    return buf[0] != 0;
}

bool bm_at_dock(const bm_view_t *v)
{
    return v->link && v->eng.secured && v->eng.mode == BM_DOCK;
}

bool bm_dock_ok(const bm_view_t *v)
{
    if (!v->link || !v->token || v->pending) return false;
    if (bm_at_dock(v)) return false;            /* already DOCK: nothing to arm */
    return v->eng.leave_ok;                     /* over an armed ANCHOR the engine needs a crew phone aboard too */
}

bool bm_leave_ok(const bm_view_t *v)
{
    return bm_at_dock(v) && v->token && !v->pending && v->eng.leave_ok;
}

void bm_lines(const bm_view_t *v, char *l1, size_t n1, char *l2, size_t n2)
{
    l1[0] = l2[0] = 0;
    bool dock = bm_at_dock(v);
    if (dock) snprintf(l1, n1, "AT DOCK - slip fence armed");
    if (v->pending) {
        if (!dock) snprintf(l1, n1, "%s", v->pending_mode == BM_DOCK ? "DOCK" : "LEAVE");
        snprintf(l2, n2, "sending %s to the boat ...", v->pending_mode == BM_DOCK ? "DOCK" : "LEAVE (MOTOR)");
    } else if (v->msg[0]) {
        if (!dock) snprintf(l1, n1, "boat mode not changed");
        snprintf(l2, n2, "%s", v->msg);
    } else if (dock) {
        if (!v->token) snprintf(l2, n2, "end it from the phone or the cockpit tablet (no token on this box)");
        else if (v->eng.leave_ok) snprintf(l2, n2, "LEAVE ends it (boat mode -> MOTOR)");
        else snprintf(l2, n2, "LEAVE needs one of our phones aboard - or end it from the phone / tablet");
    }
}

void bm_post_body(bm_mode_t m, char *buf, size_t n)
{
    snprintf(buf, n, "{\"mode\": \"%s\", \"by\": \"anchor-box\"}", bm_name(m));
}
