/**
 * Purpose: The ALERT screen - see ui_alert.h (ANQ-10).
 * Version: v3 (2026-10-21) - v3: crew RESET (Ship's Automation D-163 / OQ-135): a RESET button for the first alert the
 *   box may reset now (engine `rst`), and a confirm page (RESET / CANCEL) before anything is sent. v2 (2026-10-07) - v2:
 *   footer from al_footer (TAP ANYWHERE TO ACK, large); frame steady once acked (panel LED). v1 (2026-10-07)
 * Scope: PRODUCTION - anchor-box firmware (live on the box at the nav station, installed by signed OTA).
 * Architecture: anchor-box UI. Fixed set of objects made once (internal RAM is tight): frame, head, text, 3 more
 *   lines, footer, the RESET button and the two confirm buttons. Layout for the 800 x 480 panel: head y 22 (28 pt),
 *   text y 66 up to 8 lines (28 pt, ~45 chars a line = the engine's 340-char cap), more lines y 336.. (20 pt), footer
 *   y 438 (20 pt; 480 px wide while the RESET button sits at x 552 y 394, 220 x 70). Confirm page: head + text, then
 *   RESET / CANCEL (340 x 120, y 300). Buttons take their own press (they are not the screen's tap-to-ack).
 * Invocation: imported by screens.c.
 */
#include "ui_alert.h"

#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"

#define RED     0xFF1000          /* = ui_clock RED_DAY: never dimmed here */
#define DIM     0x802000
#define DIMRED  0x300000
#define BLACK   0x000000
#define W       800
#define H       480

static lv_obj_t *s_scr, *s_frame, *s_head, *s_text, *s_more[3], *s_foot, *s_rbtn, *s_yes, *s_no;
static bool s_blinking;
static void (*s_on_reset)(void), (*s_on_confirm)(bool yes);

static lv_obj_t *label(const lv_font_t *f, int x, int y, int w)
{
    lv_obj_t *l = lv_label_create(s_scr);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(RED), 0);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
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

static void rbtn_cb(lv_event_t *e)
{
    lv_display_trigger_activity(NULL);
    if (s_on_reset) s_on_reset();
}

static void confirm_cb(lv_event_t *e)
{
    lv_display_trigger_activity(NULL);
    if (s_on_confirm) s_on_confirm((bool)(intptr_t)lv_event_get_user_data(e));
}

static lv_obj_t *button(int x, int y, int w, int h, const char *text, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_button_create(s_scr);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, lv_color_hex(DIMRED), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x701000), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(b, lv_color_hex(RED), 0);
    lv_obj_set_style_border_width(b, 3, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_text_color(b, lv_color_hex(RED), 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
    return b;
}

lv_obj_t *ui_alert_create(void (*on_reset)(void), void (*on_confirm)(bool yes))
{
    s_on_reset = on_reset;
    s_on_confirm = on_confirm;
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

    s_head = label(&lv_font_montserrat_28, 28, 22, W - 56);
    s_text = label(&lv_font_montserrat_28, 28, 66, W - 56);
    lv_label_set_long_mode(s_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(s_text, 262);              /* 8 lines; the engine caps a text at 340 chars */
    for (int i = 0; i < 3; i++) s_more[i] = label(&lv_font_montserrat_20, 28, 336 + 30 * i, W - 56);
    s_foot = label(&lv_font_montserrat_20, 28, 438, W - 56);
    s_rbtn = button(552, 394, 220, 70, "RESET", rbtn_cb, NULL);
    s_yes = button(40, 300, 340, 120, "RESET", confirm_cb, (void *)(intptr_t)true);
    s_no = button(420, 300, 340, 120, "CANCEL", confirm_cb, (void *)(intptr_t)false);
    return s_scr;
}

static void show_hide(lv_obj_t *o, bool show)
{
    if (show) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

void ui_alert_show(const al_view_t *v, const char *confirm_id)
{
    static char *buf;                            /* PSRAM - internal RAM is tight */
    if (!buf) buf = heap_caps_malloc(AL_TEXT_MAX + 64, MALLOC_CAP_SPIRAM);
    if (!buf || !v) return;
    const size_t BUF = AL_TEXT_MAX + 64;
    char age[24];
    const al_reply_t *r = &v->r;
    int extra = v->answer_age_s > 0 ? v->answer_age_s : 0;   /* ages were taken at the engine's answer */

    if (confirm_id && confirm_id[0]) {           /* the confirm page (D-163): nothing is sent without RESET here */
        snprintf(buf, BUF, "RESET %s?", confirm_id);
        lv_label_set_text(s_head, buf);
        lv_label_set_text(s_text, "The Pi checked: its input reads normal again. RESET clears this alarm and releases "
                                  "what it switched off. Only if you have looked at the cause.");
        for (int i = 0; i < 3; i++) lv_label_set_text(s_more[i], "");
        lv_label_set_text(s_foot, "");
        show_hide(s_rbtn, false);
        show_hide(s_yes, true);
        show_hide(s_no, true);
        set_blink(false);
        return;
    }
    show_hide(s_yes, false);
    show_hide(s_no, false);

    if (r->n == 0) {
        lv_label_set_text(s_head, v->link ? "No engine alerts" : "Alerts: NO LINK to the rules engine");
        lv_label_set_text(s_text, v->link ? "Everything the rules engine raised has cleared." : "");
        for (int i = 0; i < 3; i++) lv_label_set_text(s_more[i], "");
    } else {
        const al_item_t *a = &r->a[0];
        al_age(a->age_s + extra, age, sizeof(age));
        snprintf(buf, BUF, "%s%s  %s  %s%s", a->sev, a->sim ? " TEST" : "", a->id, age,
                 a->acked ? "  (acked)" : "");
        lv_label_set_text(s_head, buf);
        lv_label_set_text(s_text, a->text);
        int listed = r->total > r->n ? 2 : 3;   /* more than fits: the third line counts the rest */
        for (int i = 0; i < 3; i++) {
            if (i < listed && i + 1 < r->n) {
                const al_item_t *b = &r->a[i + 1];
                snprintf(buf, BUF, "%s%s %s%s: %s", b->sev, b->sim ? " TEST" : "", b->id,
                         b->acked ? " (acked)" : "", b->text);
            } else if (i == listed && r->total > 1 + listed) {
                snprintf(buf, BUF, "+ %d more - see the cockpit", r->total - 1 - listed);
            } else {
                buf[0] = 0;
            }
            lv_label_set_text(s_more[i], buf);
        }
    }
    int ri = al_reset_index(v);
    if (ri >= 0) lv_label_set_text(lv_obj_get_child(s_rbtn, 0), ri == 0 ? "RESET" : "RESET ...");
    show_hide(s_rbtn, ri >= 0);
    lv_obj_set_width(s_foot, ri >= 0 ? 510 : W - 56);
    al_footer(v, buf, BUF);
    lv_label_set_text(s_foot, buf);
    lv_obj_set_style_text_color(s_foot, lv_color_hex(v->link ? RED : DIM), 0);
    lv_obj_set_style_text_font(s_foot, v->link && r->unacked > 0 && v->token && !v->ack_pending && !v->err[0]
                                           ? &lv_font_montserrat_28 : &lv_font_montserrat_20, 0);
    set_blink(v->link && r->unacked > 0);        /* panel-LED model: flashing = unacked, steady = acked, still active */
}
