/**
 * Purpose: The EXPECTED? screen - see ui_ask.h (ANC-45, Ship's Automation WF-502 / D-128).
 * Version: v2 (2026-10-18) - v2: a press off the buttons does nothing (YES / NO only, 10-18). v1 (2026-10-18)
 * Scope: PRODUCTION - anchor-box firmware (live on the box at the nav station, installed by signed OTA).
 * Architecture: anchor-box UI. Fixed set of objects made once (internal RAM is tight): frame, head, question, YES,
 *   NO, footer. Layout for the 800 x 480 panel: head y 24 (28 pt), question y 74 up to 3 lines (28 pt), buttons
 *   y 214 (340 x 150, 28 pt), footer y 392 up to 2 lines (20 pt). The buttons are enabled only while the box can
 *   answer (rs_tap); a press off them does nothing (10-18: YES / NO only).
 * Invocation: imported by screens.c.
 */
#include "ui_ask.h"

#include <stdio.h>
#include <string.h>

#define RED     0xFF1000          /* = ui_clock RED_DAY: a question must be read at a glance */
#define DIMRED  0x300000
#define BLACK   0x000000
#define W       800
#define H       480

static lv_obj_t *s_scr, *s_head, *s_q, *s_yes, *s_no, *s_yes_l, *s_no_l, *s_foot;
static void (*s_answer)(bool yes);

static lv_obj_t *label(const lv_font_t *f, int x, int y, int w)
{
    lv_obj_t *l = lv_label_create(s_scr);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(RED), 0);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_label_set_text(l, "");
    return l;
}

static void tap_cb(lv_event_t *e)
{
    lv_display_trigger_activity(NULL);
    if (s_answer) s_answer((bool)(intptr_t)lv_event_get_user_data(e));
}

static lv_obj_t *button(int x, bool yes, lv_obj_t **lbl)
{
    lv_obj_t *b = lv_button_create(s_scr);
    lv_obj_set_pos(b, x, 214);
    lv_obj_set_size(b, 340, 150);
    lv_obj_set_style_bg_color(b, lv_color_hex(DIMRED), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x701000), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(b, lv_color_hex(RED), 0);
    lv_obj_set_style_border_width(b, 3, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(BLACK), LV_STATE_DISABLED);       /* never the theme's bright grey */
    lv_obj_set_style_border_color(b, lv_color_hex(DIMRED), LV_STATE_DISABLED);
    lv_obj_set_style_text_color(b, lv_color_hex(0x501000), LV_STATE_DISABLED);
    lv_obj_set_style_text_color(b, lv_color_hex(RED), 0);
    lv_obj_add_event_cb(b, tap_cb, LV_EVENT_CLICKED, (void *)(intptr_t)yes);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(l, yes ? "YES" : "NO");
    lv_obj_center(l);
    *lbl = l;
    return b;
}

lv_obj_t *ui_ask_create(void (*answer)(bool yes))
{
    s_answer = answer;
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *frame = lv_obj_create(s_scr);
    lv_obj_remove_style_all(frame);
    lv_obj_set_pos(frame, 0, 0);
    lv_obj_set_size(frame, W, H);
    lv_obj_set_style_border_width(frame, 4, 0);  /* thin + steady: a question, not an alarm */
    lv_obj_set_style_border_color(frame, lv_color_hex(RED), 0);
    lv_obj_set_style_border_opa(frame, LV_OPA_COVER, 0);
    lv_obj_remove_flag(frame, LV_OBJ_FLAG_CLICKABLE);

    s_head = label(&lv_font_montserrat_28, 40, 24, W - 80);
    lv_label_set_text(s_head, "EXPECTED?");
    s_q = label(&lv_font_montserrat_28, 40, 74, W - 80);
    lv_obj_set_height(s_q, 124);                 /* 3 lines */
    s_yes = button(40, true, &s_yes_l);
    s_no = button(420, false, &s_no_l);
    s_foot = label(&lv_font_montserrat_20, 40, 392, W - 80);
    lv_obj_set_height(s_foot, 60);               /* 2 lines (a refusal from the engine) */
    return s_scr;
}

void ui_ask_show(const rs_view_t *v)
{
    char buf[160];
    int count, box = rs_asking(v, &count);
    if (box < 0 && v->pending) box = v->pending_box;
    rs_question(v, box, buf, sizeof(buf));
    if (!buf[0]) snprintf(buf, sizeof(buf), "%s", v->msg_ok ? "Answered." : "Nothing is asking now.");
    lv_label_set_text(s_q, buf);
    rs_yes_label(v, buf, sizeof(buf));
    lv_label_set_text(s_yes_l, buf);
    rs_no_label(buf, sizeof(buf));
    lv_label_set_text(s_no_l, buf);
    int b;
    bool can = rs_tap(v, &b);
    if (can) {
        lv_obj_remove_state(s_yes, LV_STATE_DISABLED);
        lv_obj_remove_state(s_no, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(s_yes, LV_STATE_DISABLED);
        lv_obj_add_state(s_no, LV_STATE_DISABLED);
    }
    rs_footer(v, count, buf, sizeof(buf));
    lv_label_set_text(s_foot, buf);
}
