/**
 * Purpose: Engine alerts on the box - reply parsing and what to show; see alerts.h (ANQ-10).
 * Version: v3 (2026-10-21) - v3: `rst` parsed; al_reset_index / al_reset_ok / al_detail; footer says 'sending RESET'
 *   (D-163). v2 (2026-10-07) - v2: tap-to-ack + panel-LED model (10-07). v1 (2026-10-07)
 * Scope: PRODUCTION - anchor-box firmware (live on the box at the nav station, installed by signed OTA).
 * Architecture: anchor-box firmware, logic layer. A small JSON reader for the engine's one reply shape (an object
 *   with scalars and one array of flat objects); strings honour escapes, so a quote or brace inside a message
 *   cannot derail it. Never shows a state the engine has not reported (no link -> no list, a NO LINK line).
 * Invocation: imported by alerts_ctl.c, ui_alert.c, ui_clock.c, ui_watch.c, screens.c, test_host/test_alerts.c.
 */
#include "alerts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

/* p at a '"': copy the string (escapes resolved, control / non-ASCII -> '?', \n -> ' ') and return past it */
static const char *str(const char *p, char *out, size_t n)
{
    size_t k = 0;
    if (*p != '"') return NULL;
    for (p++; *p && *p != '"'; p++) {
        char c = *p;
        if (c == '\\') {
            p++;
            if (!*p) return NULL;
            switch (*p) {
            case 'n': case 'r': case 't': c = ' '; break;
            case 'u': c = '?'; for (int i = 0; i < 4 && p[1]; i++) p++; break;
            default: c = *p; break;     /* \" \\ \/ */
            }
        } else if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7e) {
            c = '?';
        }
        if (out && k + 1 < n) out[k++] = c;
    }
    if (out && n) out[k] = 0;
    return *p == '"' ? p + 1 : NULL;
}

/* skip any JSON value starting at p (string, number, literal, object, array) */
static const char *skip(const char *p)
{
    p = ws(p);
    if (*p == '"') return str(p, NULL, 0);
    if (*p == '{' || *p == '[') {
        int depth = 0;
        while (*p) {
            if (*p == '"') { p = str(p, NULL, 0); if (!p) return NULL; continue; }
            if (*p == '{' || *p == '[') depth++;
            else if (*p == '}' || *p == ']') { if (--depth == 0) return p + 1; }
            p++;
        }
        return NULL;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']') p++;
    return p;
}

static bool lit_bool(const char *p, bool *out)
{
    if (!strncmp(p, "true", 4)) { *out = true; return true; }
    if (!strncmp(p, "false", 5)) { *out = false; return true; }
    return false;
}

/* one alert object {...}; returns past it */
static const char *item(const char *p, al_item_t *it)
{
    memset(it, 0, sizeof(*it));
    p = ws(p);
    if (*p != '{') return NULL;
    p++;
    for (;;) {
        p = ws(p);
        if (*p == '}') return p + 1;
        char key[16];
        p = str(p, key, sizeof(key));
        if (!p) return NULL;
        p = ws(p);
        if (*p != ':') return NULL;
        p = ws(p + 1);
        if (!strcmp(key, "id") && *p == '"') p = str(p, it->id, sizeof(it->id));
        else if (!strcmp(key, "sev") && *p == '"') p = str(p, it->sev, sizeof(it->sev));
        else if (!strcmp(key, "text") && *p == '"') p = str(p, it->text, sizeof(it->text));
        else if (!strcmp(key, "age_s")) { it->age_s = atoi(p); p = skip(p); }
        else if (!strcmp(key, "acked")) { lit_bool(p, &it->acked); p = skip(p); }
        else if (!strcmp(key, "sim")) { lit_bool(p, &it->sim); p = skip(p); }
        else if (!strcmp(key, "rst")) { lit_bool(p, &it->rst); p = skip(p); }
        else p = skip(p);
        if (!p) return NULL;
        p = ws(p);
        if (*p == ',') p++;
    }
}

bool al_parse(const char *body, al_reply_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!body) return false;
    const char *p = ws(body);
    if (*p != '{') return false;
    p++;
    bool have_seq = false, have_list = false;
    for (;;) {
        p = ws(p);
        if (*p == '}') break;
        char key[16];
        p = str(p, key, sizeof(key));
        if (!p) return false;
        p = ws(p);
        if (*p != ':') return false;
        p = ws(p + 1);
        if (!strcmp(key, "seq")) { out->seq = (uint32_t)strtoul(p, NULL, 10); have_seq = true; p = skip(p); }
        else if (!strcmp(key, "total")) { out->total = atoi(p); p = skip(p); }
        else if (!strcmp(key, "unacked")) { out->unacked = atoi(p); p = skip(p); }
        else if (!strcmp(key, "sounding")) { lit_bool(p, &out->sounding); p = skip(p); }
        else if (!strcmp(key, "alerts") && *p == '[') {
            p = ws(p + 1);
            while (*p && *p != ']') {
                al_item_t it;
                p = item(p, &it);
                if (!p) return false;
                if (out->n < AL_MAX && it.id[0]) out->a[out->n++] = it;
                p = ws(p);
                if (*p == ',') p = ws(p + 1);
            }
            if (*p != ']') return false;
            p++;
            have_list = true;
        } else {
            p = skip(p);
        }
        if (!p) return false;
        p = ws(p);
        if (*p == ',') p++;
    }
    return have_seq && have_list;
}

bool al_take_over(const al_view_t *v, bool anchor_alarm, bool on_clock, uint32_t idle_ms)
{
    if (anchor_alarm || !v->link || v->r.total <= 0) return false;
    if (v->r.unacked > 0) return on_clock || idle_ms >= AL_FROM_WATCH_MS;
    return on_clock && idle_ms >= AL_CLOCK_HOLD_MS;
}

bool al_release(const al_view_t *v)
{
    return !v->link || v->r.total <= 0;
}

al_tap_t al_tap(const al_view_t *v)
{
    if (v->link && v->r.unacked > 0 && v->token) return v->ack_pending ? AL_TAP_NONE : AL_TAP_ACK;
    return AL_TAP_CLOCK;
}

void al_footer(const al_view_t *v, char *buf, size_t n)
{
    if (!v->link) snprintf(buf, n, "NO LINK to the rules engine - shown as last known.   Tap: clock");
    else if (v->ack_pending) snprintf(buf, n, "sending ACK ...");
    else if (v->reset_pending) snprintf(buf, n, "sending RESET %s ...", v->reset_id);
    else if (v->err[0]) snprintf(buf, n, "%s", v->err);
    else if (v->r.unacked > 0 && v->token) snprintf(buf, n, "TAP ANYWHERE TO ACK");
    else if (v->r.unacked > 0) snprintf(buf, n, "ACK: the ACK / MUTE button beside this screen (no token on this box)");
    else snprintf(buf, n, "All acked - shown until it clears.   Tap: clock");
}

void al_clock_line(const al_view_t *v, char *buf, size_t n)
{
    buf[0] = 0;
    if (!v->wifi || !v->polled) return;
    if (!v->link) {
        snprintf(buf, n, "Alerts: NO LINK to the rules engine");
        return;
    }
    int t = v->r.total;
    if (t <= 0) return;
    if (v->r.unacked > 0)
        snprintf(buf, n, "%d ALERT%s - %d not acked - tap here", t, t == 1 ? "" : "S", v->r.unacked);
    else
        snprintf(buf, n, "%d alert%s (acked) - tap here", t, t == 1 ? "" : "s");
}

void al_watch_line(const al_view_t *v, char *buf, size_t n)
{
    buf[0] = 0;
    if (!v->link || v->r.unacked <= 0) return;
    for (int i = 0; i < v->r.n; i++) {
        const al_item_t *a = &v->r.a[i];
        if (a->acked) continue;
        snprintf(buf, n, "ENGINE %s%s %s: %s", a->sev, a->sim ? " TEST" : "", a->id, a->text);
        return;
    }
    snprintf(buf, n, "ENGINE: %d alert%s not acked", v->r.unacked, v->r.unacked == 1 ? "" : "s");
}

int al_reset_index(const al_view_t *v)
{
    if (!v->link || !v->token || v->ack_pending || v->reset_pending) return -1;
    for (int i = 0; i < v->r.n; i++)
        if (v->r.a[i].rst) return i;
    return -1;
}

bool al_reset_ok(const al_view_t *v, const char *id)
{
    if (!v->link || !v->token || !id || !id[0]) return false;
    for (int i = 0; i < v->r.n; i++)
        if (v->r.a[i].rst && !strcmp(v->r.a[i].id, id)) return true;
    return false;
}

bool al_detail(const char *body, char *buf, size_t n)
{
    buf[0] = 0;
    if (!body) return false;
    const char *p = ws(body);
    if (*p != '{') return false;
    p++;
    for (;;) {
        p = ws(p);
        if (*p != '"') return false;
        char key[16];
        p = str(p, key, sizeof(key));
        if (!p) return false;
        p = ws(p);
        if (*p != ':') return false;
        p = ws(p + 1);
        if (!strcmp(key, "detail") && *p == '"') return str(p, buf, n) != NULL && buf[0];
        p = skip(p);
        if (!p) return false;
        p = ws(p);
        if (*p != ',') return false;
        p++;
    }
}

void al_age(int s, char *buf, size_t n)
{
    if (s < 60) snprintf(buf, n, "%d s ago", s < 0 ? 0 : s);
    else if (s < 3600) snprintf(buf, n, "%d min ago", s / 60);
    else snprintf(buf, n, "%d h %02d min ago", s / 3600, (s / 60) % 60);
}
