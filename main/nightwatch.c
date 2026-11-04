/**
 * Purpose: Night-watch remote control - reply parsing and the bar's face; see nightwatch.h (ANQ-30, ANC-28).
 * Version: v2 (2026-10-07) - v2: nw_bar_shown (night only, unless ARMED / nudge). v1 (2026-10-06)
 * Scope: PRODUCTION - anchor-box firmware (live on the box at the nav station, installed by signed OTA).
 * Architecture: anchor-box firmware, logic layer. Minimal JSON key lookup (the engine's reply is one flat object);
 *   never shows a state the engine has not reported (no link -> no state at all).
 * Invocation: imported by nightwatch_ctl.c, ui_clock.c and test_host/test_nightwatch.c.
 */
#include "nightwatch.h"

#include <stdio.h>
#include <string.h>

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

/* the value after "key": (top-level flat object - a key inside a string value would be quoted differently) */
static const char *find_key(const char *body, const char *key)
{
    size_t kl = strlen(key);
    for (const char *p = body; (p = strchr(p, '"')) != NULL; p++) {
        if (strncmp(p + 1, key, kl) == 0 && p[1 + kl] == '"') {
            const char *q = skip_ws(p + 2 + kl);
            if (*q == ':') return skip_ws(q + 1);
        }
    }
    return NULL;
}

static bool get_bool(const char *body, const char *key, bool *out)
{
    const char *p = find_key(body, key);
    if (!p) return false;
    if (strncmp(p, "true", 4) == 0) { *out = true; return true; }
    if (strncmp(p, "false", 5) == 0) { *out = false; return true; }
    return false;
}

static void get_str(const char *body, const char *key, char *out, size_t n)
{
    out[0] = 0;
    const char *p = find_key(body, key);
    if (!p || *p != '"') return;               /* null / missing -> "" */
    size_t k = 0;
    for (p++; *p && *p != '"' && k + 1 < n; p++) {
        if (*p == '\\' && p[1]) p++;          /* keep the escaped char, drop the backslash */
        out[k++] = (*p >= 0x20 && *p < 0x7f) ? *p : '?';
    }
    out[k] = 0;
}

bool nw_parse(const char *body, nw_reply_t *out)
{
    *out = (nw_reply_t){ 0 };
    if (!body) return false;
    if (!get_bool(body, "armed", &out->armed)) return false;
    get_bool(body, "nudge", &out->nudge);
    get_bool(body, "night", &out->night);
    get_str(body, "since", out->since, sizeof(out->since));
    get_str(body, "by", out->by, sizeof(out->by));
    return true;
}

void nw_face(const nw_view_t *v, nw_face_t *f)
{
    *f = (nw_face_t){ 0 };
    if (!v->wifi) {
        f->kind = NWF_NO_LINK;
        snprintf(f->big, sizeof(f->big), "Night watch: no link");
        snprintf(f->small, sizeof(f->small), "no Wi-Fi on this box");
        return;
    }
    if (!v->polled) {
        f->kind = NWF_WAIT;
        snprintf(f->big, sizeof(f->big), "Night watch: ...");
        snprintf(f->small, sizeof(f->small), "asking the engine");
        return;
    }
    if (!v->link) {
        f->kind = NWF_NO_LINK;
        snprintf(f->big, sizeof(f->big), "Night watch: no link");
        snprintf(f->small, sizeof(f->small), "engine not answering");
        return;
    }
    bool armed = v->eng.armed;
    f->kind = armed ? NWF_ARMED : v->eng.nudge ? NWF_NUDGE : NWF_OFF;
    snprintf(f->big, sizeof(f->big), armed ? "NIGHT WATCH ARMED" : "NIGHT WATCH OFF");
    if (v->pending) {
        snprintf(f->small, sizeof(f->small), "sending %s ...", v->pending_arm ? "ARM" : "OFF");
        return;                                /* not tappable until the engine answers */
    }
    if (!v->token) {
        snprintf(f->small, sizeof(f->small), "%sno token - can't change it here",
                 f->kind == NWF_NUDGE ? "re-arm? " : "");
        return;
    }
    f->tappable = true;
    if (v->err[0])
        snprintf(f->small, sizeof(f->small), "%s", v->err);
    else if (f->kind == NWF_NUDGE)
        snprintf(f->small, sizeof(f->small), "re-arm?  tap to arm");
    else
        snprintf(f->small, sizeof(f->small), armed ? "tap to turn off" : "tap to arm");
}

bool nw_bar_shown(nw_kind_t kind, bool dark)
{
    return dark || kind == NWF_ARMED || kind == NWF_NUDGE;
}
