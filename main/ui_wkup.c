/**
 * Purpose: The WATCH CHECK-IN screen - see ui_wkup.h (ANQ-27, Ship's Automation WF-614 / D-161).
 * Version: v1 (2026-10-21)
 * Scope: PRODUCTION - anchor-box firmware (live on the box at the nav station, installed by signed OTA).
 * Architecture: anchor-box UI. Fixed set of objects made once (internal RAM is tight): frame, head, text, footer.
 *   Layout for the 800 x 480 panel: head y 40 (28 pt), text y 120 up to 4 lines (28 pt), footer y 400 (28 pt).
 * Invocation: imported by screens.c.
 */
#include "ui_wkup.h"

#include <stdio.h>

#define RED   0xFF1000            /* = ui_clock RED_DAY: never dimmed here */
#define DIM   0x802000
#define W     800
#define H     480

static lv_obj_t *s_scr, *s_frame, *s_head, *s_text, *s_foot;
static bool s_blinking;

static lv_obj_t *label(const lv_font_t *f, int y, int h)
{
    lv_obj_t *l = lv_label_create(s_scr);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(RED), 0);
    lv_obj_set_pos(l, 40, y);
    lv_obj_set_width(l, W - 80);
    if (h > 0) lv_obj_set_height(l, h);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_label_set_text(l, "");
    return l;
}

static void blink_cb(void *obj, int32_t v)
{
    lv_obj_set_style_border_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

static void set_blink(bool on)
{
    if (on == s_blinking) return;
    s_blinking = on;
    lv_anim_delete(s_frame, blink_cb);
    lv_obj_set_style_border_opa(s_frame, LV_OPA_COVER, 0);
    if (!on) return;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_frame);
    lv_anim_set_exec_cb(&a, blink_cb);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_20);
    lv_anim_set_duration(&a, 600);
    lv_anim_set_playback_duration(&a, 600);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}

lv_obj_t *ui_wkup_create(void)
{
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    s_frame = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_frame);
    lv_obj_set_pos(s_frame, 0, 0);
    lv_obj_set_size(s_frame, W, H);
    lv_obj_set_style_border_width(s_frame, 12, 0);
    lv_obj_set_style_border_color(s_frame, lv_color_hex(RED), 0);
    lv_obj_set_style_border_opa(s_frame, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_frame, LV_OBJ_FLAG_CLICKABLE);

    s_head = label(&lv_font_montserrat_28, 40, 0);
    s_text = label(&lv_font_montserrat_28, 120, 200);
    s_foot = label(&lv_font_montserrat_28, 400, 0);
    return s_scr;
}

void ui_wkup_show(const wk_view_t *v)
{
    char buf[160];
    if (!v) return;
    wk_head(v, buf, sizeof(buf));
    lv_label_set_text(s_head, buf);
    if (wk_due(v)) wk_text(v, buf, sizeof(buf));
    else buf[0] = 0;                              /* answered: only the footer's 'I'M UP taken' */
    lv_label_set_text(s_text, buf);
    wk_footer(v, buf, sizeof(buf));
    lv_label_set_text(s_foot, buf);
    lv_obj_set_style_text_color(s_foot, lv_color_hex(v->link ? RED : DIM), 0);
    set_blink(wk_due(v) && v->eng.stage >= 2);   /* steady at the 15-min call, blinking from the second call */
}
