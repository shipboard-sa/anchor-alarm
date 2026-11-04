/**
 * Purpose: The CLOCK screen - see ui_clock.h.
 * Version: v5 (2026-10-07) - v5: rules-engine alerts line under the notice (ANQ-10), tappable to open the list.
 *   v4 (2026-10-07) - v4: the bar is hidden in daylight unless ARMED / nudging (10-07, nw_bar_shown).
 *   v3 (2026-10-06) - v3: night-watch bar under the digits (ANQ-30, ANC-28): solid = ARMED, hollow = OFF,
 *   blinking full-red hollow = re-arm nudge, dim = no link; a tap on it asks the engine to flip (nightwatch_ctl.c).
 *   Digits moved up (TOP 70 -> 14) to make room; notice line now between the digits and the bar.
 *   v2: notice line for a failed monitoring host (ANC-25). v1 (2026-10-02)
 * Scope: PRODUCTION - anchor-box firmware, live on the box at the nav station since 2026-10-02 (installed by signed OTA).
 * Architecture: anchor-box UI. Black background always (10-02); red digits; a coloured full screen is
 *   reserved for ALARM (which always switches to the WATCH screen, so the night-watch bar is never on screen
 *   during an anchor ALARM). Night-watch state from nightwatch_ctl.c / nightwatch.c (the engine's word only).
 * Invocation: imported by screens.c / app_main.c.
 */
#include "ui_clock.h"

#include <string.h>
#include "board.h"
#include "nightwatch_ctl.h"

/* digit geometry: 4 digits of DW x DH, segment thickness T */
#define DW 140
#define DH 300
#define T  28
#define VH ((DH - 3 * T) / 2)
#define TOP 14       /* 70 until v2; moved up for the night-watch bar (ANC-28) */

#define RED_DAY   0xFF1000
#define RED_NIGHT 0x480000
#define DIM_DAY   0x501000     /* greyed (no link) */
#define DIM_NIGHT 0x280000

/* night-watch bar (ANC-28): between the 'M' and the anchor symbol, under the notice line */
#define NW_X 56
#define NW_Y 386
#define NW_W 636
#define NW_H 90

enum { SA, SB, SC, SD, SE, SF, SG };
static const uint8_t DIGITS[10] = {
    0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F,  /* bit n = segment n (a..g) */
};
#define DASH (1u << SG)

static lv_obj_t *s_scr;
static lv_obj_t *s_seg[4][7];
static lv_obj_t *s_colon[2];
static lv_obj_t *s_anchor, *s_anchor_parts[5], *s_bang, *s_manual, *s_notice, *s_alert;
static lv_obj_t *s_nw, *s_nw_big, *s_nw_small;
static nw_face_t s_nw_face;
static bool s_night;
static lv_color_t s_red;
static ui_anchor_t s_anchor_state = UI_ANCHOR_OFF;

static lv_obj_t *rect(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, 8, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(RED_DAY), 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static void make_digit(int i, int x0)
{
    const int g[7][4] = {
        {T, 0, DW - 2 * T, T},                 /* a */
        {DW - T, T, T, VH},                    /* b */
        {DW - T, 2 * T + VH, T, VH},           /* c */
        {T, 2 * T + 2 * VH, DW - 2 * T, T},    /* d */
        {0, 2 * T + VH, T, VH},                /* e */
        {0, T, T, VH},                         /* f */
        {T, T + VH, DW - 2 * T, T},            /* g */
    };
    for (int s = 0; s < 7; s++) {
        s_seg[i][s] = rect(s_scr, x0 + g[s][0], TOP + g[s][1], g[s][2], g[s][3]);
    }
}

static void show_pattern(int i, uint8_t bits)
{
    for (int s = 0; s < 7; s++) {
        if (bits & (1u << s)) lv_obj_remove_flag(s_seg[i][s], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_seg[i][s], LV_OBJ_FLAG_HIDDEN);
    }
}

/* anchor symbol, ~56 x 70 px, bottom-right corner: ring, shank, stock, crown */
static lv_obj_t *arc(lv_obj_t *parent, int x, int y, int d, int a0, int a1, int w)
{
    lv_obj_t *o = lv_arc_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, d, d);
    lv_arc_set_bg_angles(o, a0, a1);
    lv_obj_set_style_arc_width(o, w, LV_PART_MAIN);
    lv_obj_set_style_arc_color(o, lv_color_hex(RED_DAY), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(o, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(o, true, LV_PART_MAIN);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static void make_anchor(void)
{
    s_anchor = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_anchor);
    lv_obj_set_size(s_anchor, 60, 74);
    lv_obj_set_pos(s_anchor, BOARD_LCD_H_RES - 60 - 16, BOARD_LCD_V_RES - 74 - 12);
    lv_obj_remove_flag(s_anchor, LV_OBJ_FLAG_CLICKABLE);
    s_anchor_parts[0] = arc(s_anchor, 22, 0, 16, 0, 360, 4);       /* ring */
    s_anchor_parts[1] = rect(s_anchor, 27, 15, 6, 52);              /* shank */
    s_anchor_parts[2] = rect(s_anchor, 14, 22, 32, 6);              /* stock */
    s_anchor_parts[3] = arc(s_anchor, 2, 18, 56, 20, 160, 6);       /* crown */
    for (int i = 1; i <= 2; i++) lv_obj_set_style_radius(s_anchor_parts[i], 3, 0);
    s_anchor_parts[4] = NULL;

    s_bang = lv_label_create(s_scr);
    lv_label_set_text(s_bang, "!");
    lv_obj_set_style_text_font(s_bang, &lv_font_montserrat_28, 0);
    lv_obj_set_pos(s_bang, BOARD_LCD_H_RES - 60 - 16 - 22, BOARD_LCD_V_RES - 74);

    s_manual = lv_label_create(s_scr);
    lv_label_set_text(s_manual, "M");
    lv_obj_set_style_text_font(s_manual, &lv_font_montserrat_28, 0);
    lv_obj_set_pos(s_manual, 16, BOARD_LCD_V_RES - 44);
    lv_obj_add_flag(s_manual, LV_OBJ_FLAG_HIDDEN);
}

static void blink_cb(void *obj, int32_t v);

/* draw the night-watch bar from s_nw_face (solid = ARMED, hollow = OFF, blinking = nudge, dim = no link) */
static void nw_apply(void)
{
    if (!s_nw) return;
    const nw_face_t *f = &s_nw_face;
    lv_color_t dim = lv_color_hex(s_night ? DIM_NIGHT : DIM_DAY);
    lv_color_t full = lv_color_hex(RED_DAY);           /* the nudge is never dimmed - it must be seen */
    lv_color_t fg = s_red, edge = s_red;
    lv_opa_t bg = LV_OPA_TRANSP;
    int border = 4;
    bool big_font = true;
    switch (f->kind) {
    case NWF_ARMED: bg = LV_OPA_COVER; border = 0; fg = lv_color_black(); break;
    case NWF_OFF: break;
    case NWF_NUDGE: border = 8; fg = edge = full; break;
    default: border = 2; fg = edge = dim; big_font = false; break;      /* WAIT / NO_LINK: greyed */
    }
    lv_obj_set_style_bg_color(s_nw, s_red, 0);
    lv_obj_set_style_bg_opa(s_nw, bg, 0);
    lv_obj_set_style_border_width(s_nw, border, 0);
    lv_obj_set_style_border_color(s_nw, edge, 0);
    lv_obj_set_style_text_color(s_nw_big, fg, 0);
    lv_obj_set_style_text_color(s_nw_small, fg, 0);
    lv_obj_set_style_text_font(s_nw_big, big_font ? &lv_font_montserrat_48 : &lv_font_montserrat_28, 0);
    lv_label_set_text(s_nw_big, f->big);
    lv_label_set_text(s_nw_small, f->small);
    if (f->small[0]) {
        lv_obj_remove_flag(s_nw_small, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(s_nw_big, LV_ALIGN_TOP_MID, 0, big_font ? -4 : 6);
    } else {
        lv_obj_add_flag(s_nw_small, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(s_nw_big, LV_ALIGN_CENTER, 0, 0);
    }
    /* not tappable -> not clickable, so a tap there wakes WATCH like the rest of the clock face */
    if (f->tappable) lv_obj_add_flag(s_nw, LV_OBJ_FLAG_CLICKABLE);
    else lv_obj_remove_flag(s_nw, LV_OBJ_FLAG_CLICKABLE);
    lv_anim_delete(s_nw, blink_cb);
    lv_obj_set_style_opa(s_nw, LV_OPA_COVER, 0);
    if (!nw_bar_shown(f->kind, s_night)) {       /* daylight + OFF / no link: no bar (hidden = no tap) */
        lv_obj_add_flag(s_nw, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(s_nw, LV_OBJ_FLAG_HIDDEN);
    if (f->kind == NWF_NUDGE) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_nw);
        lv_anim_set_exec_cb(&a, blink_cb);
        lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_20);
        lv_anim_set_duration(&a, 700);
        lv_anim_set_playback_duration(&a, 700);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
    }
}

static void nw_click_cb(lv_event_t *e)
{
    if (nightwatch_ctl_toggle()) ui_clock_nightwatch_tick();   /* show 'sending ...' at once */
}

static void make_nightwatch(void)
{
    s_nw = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_nw);
    lv_obj_set_pos(s_nw, NW_X, NW_Y);
    lv_obj_set_size(s_nw, NW_W, NW_H);
    lv_obj_set_style_radius(s_nw, 10, 0);
    lv_obj_remove_flag(s_nw, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_nw, nw_click_cb, LV_EVENT_CLICKED, NULL);
    s_nw_big = lv_label_create(s_nw);
    s_nw_small = lv_label_create(s_nw);
    lv_obj_set_style_text_font(s_nw_small, &lv_font_montserrat_20, 0);
    lv_obj_align(s_nw_small, LV_ALIGN_BOTTOM_MID, 0, -4);
    nw_view_t v;
    nightwatch_ctl_get(&v);
    nw_face(&v, &s_nw_face);
}

void ui_clock_nightwatch_tick(void)
{
    nw_view_t v;
    nw_face_t f;
    nightwatch_ctl_get(&v);
    nw_face(&v, &f);
    if (f.kind == s_nw_face.kind && f.tappable == s_nw_face.tappable && !strcmp(f.big, s_nw_face.big) &&
        !strcmp(f.small, s_nw_face.small))
        return;
    s_nw_face = f;
    nw_apply();
}

static void paint(void)
{
    for (int i = 0; i < 4; i++)
        for (int s = 0; s < 7; s++) lv_obj_set_style_bg_color(s_seg[i][s], s_red, 0);
    for (int i = 0; i < 2; i++) lv_obj_set_style_bg_color(s_colon[i], s_red, 0);
    lv_obj_set_style_arc_color(s_anchor_parts[0], s_red, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_anchor_parts[3], s_red, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_anchor_parts[1], s_red, 0);
    lv_obj_set_style_bg_color(s_anchor_parts[2], s_red, 0);
    lv_obj_set_style_border_color(s_anchor_parts[1], s_red, 0);
    lv_obj_set_style_border_color(s_anchor_parts[2], s_red, 0);
    lv_obj_set_style_text_color(s_bang, s_red, 0);
    lv_obj_set_style_text_color(s_manual, s_red, 0);
    lv_obj_set_style_text_color(s_notice, s_red, 0);
    lv_obj_set_style_text_color(s_alert, s_red, 0);
    nw_apply();
}

lv_obj_t *ui_clock_create(void)
{
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    /* wide gap inside each pair so a 1 never reads as part of its neighbour (mock-up check 10-02) */
    const int gap = 48, colon_w = 60, cgap = 22;
    int x = (BOARD_LCD_H_RES - (4 * DW + 2 * gap + colon_w + 2 * cgap)) / 2;
    make_digit(0, x);
    make_digit(1, x + DW + gap);
    int cx = x + 2 * DW + gap + cgap;
    s_colon[0] = rect(s_scr, cx + (colon_w - T) / 2, TOP + DH / 3 - T / 2, T, T);
    s_colon[1] = rect(s_scr, cx + (colon_w - T) / 2, TOP + 2 * DH / 3 - T / 2, T, T);
    make_digit(2, cx + colon_w + cgap);
    make_digit(3, cx + colon_w + cgap + DW + gap);
    make_anchor();

    /* monitoring-host notice (ANC-25): under the digits, between the 'M' and the anchor symbol; red like the
     * rest - a coloured screen stays reserved for the anchor ALARM (ANC-06) */
    s_notice = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_notice, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_align(s_notice, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_notice, 620);
    lv_obj_set_pos(s_notice, (BOARD_LCD_H_RES - 620) / 2, TOP + DH + 8);   /* two lines end above the bar */
    lv_label_set_text(s_notice, "");
    lv_obj_add_flag(s_notice, LV_OBJ_FLAG_HIDDEN);

    /* rules-engine alerts line (ANQ-10): under the notice; placed by place_alert() */
    s_alert = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_alert, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_align(s_alert, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_alert, 620);
    lv_label_set_long_mode(s_alert, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_alert, "");
    lv_obj_add_flag(s_alert, LV_OBJ_FLAG_HIDDEN);

    make_nightwatch();

    s_red = lv_color_hex(RED_DAY);
    paint();
    ui_clock_set_time(-1, 0);
    ui_clock_set_anchor(UI_ANCHOR_OFF);
    return s_scr;
}

void ui_clock_set_time(int hh, int mm)
{
    if (hh < 0 || hh > 23 || mm < 0 || mm > 59) {
        for (int i = 0; i < 4; i++) show_pattern(i, DASH);
        return;
    }
    show_pattern(0, DIGITS[hh / 10]);
    show_pattern(1, DIGITS[hh % 10]);
    show_pattern(2, DIGITS[mm / 10]);
    show_pattern(3, DIGITS[mm % 10]);
}

static void blink_cb(void *obj, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

void ui_clock_set_anchor(ui_anchor_t st)
{
    s_anchor_state = st;
    lv_anim_delete(s_anchor, blink_cb);
    lv_obj_set_style_opa(s_anchor, LV_OPA_COVER, 0);
    if (st == UI_ANCHOR_OFF) {
        lv_obj_add_flag(s_anchor, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_bang, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(s_anchor, LV_OBJ_FLAG_HIDDEN);
    bool outline = st == UI_ANCHOR_ACQUIRING;
    for (int i = 1; i <= 2; i++) {     /* outline = hollow shank + stock */
        lv_obj_set_style_bg_opa(s_anchor_parts[i], outline ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_anchor_parts[i], outline ? 2 : 0, 0);
    }
    if (st == UI_ANCHOR_CONSERVATIVE) lv_obj_remove_flag(s_bang, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_bang, LV_OBJ_FLAG_HIDDEN);
    if (outline) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_anchor);
        lv_anim_set_exec_cb(&a, blink_cb);
        lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_20);
        lv_anim_set_duration(&a, 1000);
        lv_anim_set_playback_duration(&a, 1000);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
    }
}

void ui_clock_set_manual_zone(bool on)
{
    if (on) lv_obj_remove_flag(s_manual, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_manual, LV_OBJ_FLAG_HIDDEN);
}

/* the alerts line sits right under the notice: line 1 when there is no notice, else after its lines */
static void place_alert(void)
{
    int lines = 0;
    if (!lv_obj_has_flag(s_notice, LV_OBJ_FLAG_HIDDEN)) {
        lines = 1;
        for (const char *p = lv_label_get_text(s_notice); *p; p++) lines += *p == '\n';
    }
    lv_obj_set_pos(s_alert, (BOARD_LCD_H_RES - 620) / 2, TOP + DH + 8 + 34 * lines);
}

void ui_clock_set_notice(const char *txt)
{
    lv_label_set_text(s_notice, txt ? txt : "");
    if (txt && txt[0]) lv_obj_remove_flag(s_notice, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_notice, LV_OBJ_FLAG_HIDDEN);
    place_alert();
}

void ui_clock_set_alert_line(const char *txt, bool tappable)
{
    lv_label_set_text(s_alert, txt ? txt : "");
    if (txt && txt[0]) lv_obj_remove_flag(s_alert, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_alert, LV_OBJ_FLAG_HIDDEN);
    /* not tappable -> not clickable, so a tap there wakes WATCH like the rest of the clock face */
    if (tappable && txt && txt[0]) lv_obj_add_flag(s_alert, LV_OBJ_FLAG_CLICKABLE);
    else lv_obj_remove_flag(s_alert, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_alert, 20);
    place_alert();
}

void ui_clock_on_alert_line(lv_event_cb_t cb)
{
    lv_obj_add_event_cb(s_alert, cb, LV_EVENT_PRESSED, NULL);
}

void ui_clock_set_night(bool night)
{
    s_night = night;
    s_red = lv_color_hex(night ? RED_NIGHT : RED_DAY);
    paint();
}
