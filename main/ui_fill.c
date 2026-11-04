/**
 * Purpose: The FILL page - see ui_fill.h (ANC-41).
 * Version: v1 (2026-10-09)
 * Scope: PRODUCTION - anchor-box firmware (live on the box at the nav station, installed by signed OTA).
 * Architecture: anchor-box UI. Head 'TANK FILLS' + the watermaker line; four rows (frame, engine label, level +
 *   session line, button); footer (what a tap does / why it cannot) + BACK. A row's frame is thicker while its tank
 *   is filling. Rows are redrawn only when their face changed. One tap = one POST via fill_ctl.c (no confirm page:
 *   a fill session only makes the boat watch the tank and chirp; it moves nothing). Caller holds display_lock().
 * Invocation: imported by screens.c.
 */
#include "ui_fill.h"

#include <stdio.h>
#include <string.h>
#include "fill_ctl.h"

#define RED     0xFF1000
#define DIMRED  0x300000
#define DIMTXT  0x501000
#define BLACK   0x000000

#define ROW_Y0 62
#define ROW_PITCH 87
#define ROW_H 80

static lv_obj_t *s_scr, *s_head, *s_foot;
static lv_obj_t *s_row[FL_TANKS], *s_name[FL_TANKS], *s_state[FL_TANKS], *s_btn[FL_TANKS], *s_btn_lbl[FL_TANKS];
static fl_row_t s_face[FL_TANKS];
static bool s_drawn[FL_TANKS];

static lv_obj_t *label(lv_obj_t *p, const lv_font_t *f, int x, int y, int w)
{
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(RED), 0);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_label_set_text(l, "");
    return l;
}

/* same look as the WATCH buttons (ui_watch.c): dark red, red edge; disabled = dark, never bright grey */
static lv_obj_t *button(lv_obj_t *p, int x, int y, int w, int h, const char *txt, lv_event_cb_t cb, intptr_t ud,
                        lv_obj_t **lbl_out)
{
    lv_obj_t *b = lv_button_create(p);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, lv_color_hex(DIMRED), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x701000), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(b, lv_color_hex(RED), 0);
    lv_obj_set_style_border_width(b, 3, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(BLACK), LV_STATE_DISABLED);
    lv_obj_set_style_border_color(b, lv_color_hex(DIMRED), LV_STATE_DISABLED);
    lv_obj_set_style_text_color(b, lv_color_hex(DIMTXT), LV_STATE_DISABLED);
    lv_obj_set_style_text_color(b, lv_color_hex(RED), 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)ud);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
    if (lbl_out) *lbl_out = l;
    return b;
}

static void tap_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (fill_ctl_tap(i)) ui_fill_tick();      /* 'sending ...' at once */
}

static void draw_row(int i, const fl_row_t *f)
{
    if (!f->shown) {
        lv_obj_add_flag(s_row[i], LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(s_row[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_border_width(s_row[i], f->filling ? 6 : 2, 0);
    lv_label_set_text(s_name[i], f->name);
    lv_label_set_text(s_state[i], f->state);
    lv_label_set_text(s_btn_lbl[i], f->btn);
    lv_obj_center(s_btn_lbl[i]);
    if (f->can_tap) lv_obj_remove_state(s_btn[i], LV_STATE_DISABLED);
    else lv_obj_add_state(s_btn[i], LV_STATE_DISABLED);
}

lv_obj_t *ui_fill_create(lv_event_cb_t back_cb)
{
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = label(s_scr, &lv_font_montserrat_28, 24, 16, 260);
    lv_label_set_text(title, "TANK FILLS");
    s_head = label(s_scr, &lv_font_montserrat_28, 300, 16, 476);
    lv_obj_set_style_text_align(s_head, LV_TEXT_ALIGN_RIGHT, 0);

    for (int i = 0; i < FL_TANKS; i++) {
        lv_obj_t *r = lv_obj_create(s_scr);
        lv_obj_remove_style_all(r);
        lv_obj_set_pos(r, 16, ROW_Y0 + i * ROW_PITCH);
        lv_obj_set_size(r, 768, ROW_H);
        lv_obj_set_style_radius(r, 10, 0);
        lv_obj_set_style_border_color(r, lv_color_hex(RED), 0);
        lv_obj_set_style_border_width(r, 2, 0);
        lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(r, LV_OBJ_FLAG_HIDDEN);
        s_row[i] = r;
        s_name[i] = label(r, &lv_font_montserrat_28, 14, 8, 520);
        s_state[i] = label(r, &lv_font_montserrat_20, 14, 46, 520);
        s_btn[i] = button(r, 548, 8, 208, ROW_H - 16, "", tap_cb, i, &s_btn_lbl[i]);
    }

    s_foot = label(s_scr, &lv_font_montserrat_20, 24, 430, 560);
    button(s_scr, 600, 414, 184, 60, "BACK", back_cb, 0, NULL);
    ui_fill_tick();
    return s_scr;
}

void ui_fill_tick(void)
{
    static fl_view_t v;                       /* LVGL task only; off the stack */
    char buf[96];
    fill_ctl_get(&v);
    fl_head(&v, buf, sizeof(buf));
    lv_label_set_text(s_head, buf);
    fl_footer(&v, buf, sizeof(buf));
    lv_label_set_text(s_foot, buf);
    lv_obj_set_style_text_color(s_foot, lv_color_hex(v.link ? RED : DIMTXT), 0);
    for (int i = 0; i < FL_TANKS; i++) {
        fl_row_t f;
        fl_row(&v, i, &f);
        if (s_drawn[i] && !memcmp(&f, &s_face[i], sizeof(f))) continue;
        s_face[i] = f;
        s_drawn[i] = true;
        draw_row(i, &f);
    }
}
