/**
 * Purpose: CLOCK / WATCH / ALERT view controller - see screens.h.
 * Version: v10 (2026-10-21) - v10: crew RESET (Ship's Automation D-163): ALERT's RESET button opens a confirm page
 *   (RESET / CANCEL; taps elsewhere do nothing there); it closes itself when the alarm is no longer resettable, after
 *   60 s, or when ALERT goes away. v9 (2026-10-21) - v9: WATCH CHECK-IN view (ANQ-27, Ship's Automation WF-614 / D-161): while the engine's
 *   watchkeeper timer asks for a check-in it takes over from CLOCK at once or from WATCH / FILL / EXPECTED? after 15 s
 *   untouched; a tap anywhere = I'M UP; an engine ALERT and the anchor ALARM win over it; it goes away once nothing
 *   is due (after a 4 s 'taken'). v8 (2026-10-18) - v8: EXPECTED? is YES / NO only (10-18): no 'not now' press, from CLOCK at once.
 *   v7 (2026-10-18) - v7: EXPECTED? view (ANC-45, Ship's Automation WF-502): while the engine asks 'expected?'
 *   about a cold box it takes over from CLOCK or from WATCH / FILL after 15 s untouched; YES / NO answer in one tap;
 *   an engine ALERT (unacked at once, acked after 2 min) and the anchor ALARM both win over it; it goes away
 *   once nothing is asking (after a 5 s 'taken'). v6 (2026-10-09) - v6: FILL page (ANC-41): opened by WATCH's FILL button (10-09), BACK -> WATCH,
 *   2 min untouched -> CLOCK; never opened during an anchor ALARM; an anchor ALARM takes the screen away (WATCH); an unacked engine alarm takes over after 15 s untouched,
 *   as from WATCH. v5 (2026-10-07) - v5: tap-to-ack + panel-LED model (10-07, risk accepted): a tap anywhere on ALERT
 *   acks (unacked) or goes to the CLOCK (all acked); open alarms stay on ALERT until they CLEAR; acked-only ALERT
 *   comes back after 2 min untouched, so the night-watch bar stays reachable. Replaces v4's view-only + snooze.
 *   v4 (2026-10-07): ALERT screen (ANQ-10). v3 (2026-10-06): night-watch bar refresh. v2: screens_alarm()
 * Scope: PRODUCTION - anchor-box firmware, live on the box at the nav station since 2026-10-02 (installed by signed OTA).
 * Architecture: anchor-box UI controller over ui_clock.c / ui_watch.c / ui_alert.c; alert decisions in alerts.c,
 *   data + ACK from alerts_ctl.c. An anchor ALARM always wins (ANC-06 / ANC-28).
 * Invocation: imported by app_main.c.
 */
#include "screens.h"

#include <stdlib.h>
#include <string.h>
#include "alerts_ctl.h"
#include "board.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "fill_ctl.h"
#include "lvgl.h"
#include "restock_ctl.h"
#include "ui_alert.h"
#include "ui_ask.h"
#include "ui_clock.h"
#include "ui_fill.h"
#include "ui_watch.h"
#include "ui_wkup.h"
#include "wkeep_ctl.h"

static const char *TAG = "screens";
typedef enum { V_CLOCK, V_WATCH, V_ALERT, V_FILL, V_ASK, V_WKUP } view_t;
static lv_obj_t *s_clock, *s_watch, *s_alert, *s_fill, *s_ask, *s_wkup;
static wk_view_t s_wk;                       /* LVGL task only (~0.2 KB) */
static char s_confirm[24];                   /* rule on the RESET confirm page, "" = none (LVGL task only) */
static uint32_t s_confirm_ms;
#define CONFIRM_MAX_MS (60 * 1000)
static rs_view_t s_rs;                       /* LVGL task only (~0.3 KB) */
static view_t s_view;
static bool s_alarm;
static al_view_t *s_al;                      /* LVGL task only; PSRAM (1.5 KB - internal RAM is tight) */
static char *s_line, *s_last_clock, *s_last_watch;   /* PSRAM, AL_LINE_MAX each */
#define AL_LINE_MAX (AL_TEXT_MAX + 40)

static void wake_cb(lv_event_t *e)
{
    /* The wake tap only wakes: switch on PRESS and make LVGL ignore the rest of this touch, so the
     * release can never land on a WATCH control (SPEC 2.3). */
    lv_indev_t *indev = lv_indev_active();
    screens_show_watch();
    if (indev) lv_indev_wait_release(indev);
}

static void show_view(view_t v)
{
    if (s_view == V_FILL && v != V_FILL) fill_ctl_fast(false);
    if (v != V_ALERT) s_confirm[0] = 0;      /* the confirm page never outlives ALERT */
    s_view = v;
}

static void show_alert(void)
{
    show_view(V_ALERT);
    if (s_al) ui_alert_show(s_al, s_confirm);
    lv_screen_load(s_alert);
    board_backlight(true);
    if (s_al) ESP_LOGI(TAG, "ALERT (engine seq %lu, %d active, %d not acked)", (unsigned long)s_al->r.seq,
                       s_al->r.total, s_al->r.unacked);
}

/* A tap anywhere on ALERT (10-07): ACK if something is unacked, else the CLOCK. Swallowed like the wake tap. */
static void alert_tap_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    lv_display_trigger_activity(NULL);
    if (s_confirm[0]) {                      /* the confirm page: only RESET / CANCEL act */
        if (indev) lv_indev_wait_release(indev);
        return;
    }
    switch (s_al ? al_tap(s_al) : AL_TAP_CLOCK) {
    case AL_TAP_ACK:
        if (alerts_ctl_ack()) {
            alerts_ctl_get(s_al);
            ui_alert_show(s_al, s_confirm);  /* 'sending ACK ...' at once */
        }
        break;
    case AL_TAP_CLOCK: screens_show_clock(); break;
    default: break;                          /* an ACK is on its way */
    }
    if (indev) lv_indev_wait_release(indev);
}

/* ALERT's RESET button: open the confirm page for the first resettable alarm (D-163) */
static void reset_btn_cb(void)
{
    int i = s_al ? al_reset_index(s_al) : -1;
    if (i < 0) return;
    strlcpy(s_confirm, s_al->r.a[i].id, sizeof(s_confirm));
    s_confirm_ms = lv_tick_get();
    ui_alert_show(s_al, s_confirm);
    ESP_LOGI(TAG, "RESET confirm page for %s", s_confirm);
}

/* RESET / CANCEL on the confirm page */
static void reset_confirm_cb(bool yes)
{
    char id[sizeof(s_confirm)];
    strlcpy(id, s_confirm, sizeof(id));
    s_confirm[0] = 0;
    if (yes && s_al && alerts_ctl_reset(id)) ESP_LOGI(TAG, "RESET %s sent", id);
    if (s_al) {
        alerts_ctl_get(s_al);
        ui_alert_show(s_al, s_confirm);          /* 'sending RESET ...' at once */
    }
}

static void show_ask(void)
{
    show_view(V_ASK);
    ui_ask_show(&s_rs);
    lv_screen_load(s_ask);
    board_backlight(true);
    int count, b = rs_asking(&s_rs, &count);
    ESP_LOGI(TAG, "EXPECTED? (%s, %d asking)", b >= 0 ? RS_KEYS[b] : "-", count);
}

/* YES / NO on the EXPECTED? screen: one tap answers, no confirm (10-18) */
static void ask_answer_cb(bool yes)
{
    if (restock_ctl_answer(yes)) {
        restock_ctl_get(&s_rs);
        ui_ask_show(&s_rs);                      /* 'sending YES ...' at once */
    }
}

static void show_wkup(void)
{
    show_view(V_WKUP);
    ui_wkup_show(&s_wk);
    lv_screen_load(s_wkup);
    board_backlight(true);
    ESP_LOGI(TAG, "WATCH CHECK-IN (stage %d, %d s since I'm up)", s_wk.eng.stage, s_wk.eng.since_up_s);
}

/* A tap anywhere on WATCH CHECK-IN = I'M UP (ANQ-27). Swallowed like the wake tap. */
static void wkup_tap_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    lv_display_trigger_activity(NULL);
    if (wkeep_ctl_up()) {
        wkeep_ctl_get(&s_wk);
        ui_wkup_show(&s_wk);                     /* 'sending I'M UP ...' at once */
    }
    if (indev) lv_indev_wait_release(indev);
}

static void fill_back_cb(lv_event_t *e)
{
    screens_show_watch();                        /* back where FILL was tapped */
}

/* the CLOCK alerts line (seen while the CLOCK holds): opens ALERT now */
static void alert_line_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    show_alert();
    if (indev) lv_indev_wait_release(indev);
}

static void idle_timer_cb(lv_timer_t *t)
{
    uint32_t idle = lv_display_get_inactive_time(NULL);
    if (s_al) {
        alerts_ctl_get(s_al);
        al_clock_line(s_al, s_line, AL_LINE_MAX);
        if (strcmp(s_line, s_last_clock)) {
            ui_clock_set_alert_line(s_line, s_al->link && s_al->r.total > 0);
            strlcpy(s_last_clock, s_line, AL_LINE_MAX);
        }
        al_watch_line(s_al, s_line, AL_LINE_MAX);
        if (strcmp(s_line, s_last_watch)) {
            ui_watch_set_engine_line(s_line);
            strlcpy(s_last_watch, s_line, AL_LINE_MAX);
        }
    }

    restock_ctl_get(&s_rs);
    wkeep_ctl_get(&s_wk);

    if (s_view == V_ALERT) {
        if (s_confirm[0] && (!s_al || !al_reset_ok(s_al, s_confirm) || lv_tick_elaps(s_confirm_ms) > CONFIRM_MAX_MS))
            s_confirm[0] = 0;                /* no longer resettable (cleared / input active again) or left alone */
        if (s_al) ui_alert_show(s_al, s_confirm);
        if (s_alarm) return;                 /* screens_set_alarm already moved to WATCH */
        if (!s_al || al_release(s_al)) screens_show_clock();   /* all CLEARED (or no link): back to the clock */
        return;
    }
    if (s_view == V_WKUP) {
        if (s_alarm) return;                 /* screens_set_alarm already moved to WATCH */
        if (s_al && al_take_over(s_al, s_alarm, true, idle)) {   /* an engine alarm wins, as over the CLOCK */
            show_alert();
            return;
        }
        ui_wkup_show(&s_wk);
        if (wk_release(&s_wk)) screens_show_clock();   /* I'm up taken (after 'taken'), not due, or no link */
        return;
    }
    if (s_view == V_ASK) {
        if (s_alarm) return;                 /* screens_set_alarm already moved to WATCH */
        if (s_al && al_take_over(s_al, s_alarm, true, idle)) {   /* an engine alarm wins, as over the CLOCK */
            show_alert();
            return;
        }
        if (wk_take_over(&s_wk, s_alarm, false, idle)) {   /* a due check-in wins over a cold-box question */
            show_wkup();
            return;
        }
        ui_ask_show(&s_rs);
        if (rs_release(&s_rs)) screens_show_clock();   /* answered (after 'taken'), no longer asking, or no link */
        return;
    }
    if (s_al && al_take_over(s_al, s_alarm, s_view == V_CLOCK, idle)) {   /* from FILL: as from WATCH */
        show_alert();
        return;
    }
    if (wk_take_over(&s_wk, s_alarm, s_view == V_CLOCK, idle)) {   /* from FILL: as from WATCH */
        show_wkup();
        return;
    }
    if (rs_take_over(&s_rs, s_alarm, s_view == V_CLOCK, idle)) {   /* from FILL: as from WATCH */
        show_ask();
        return;
    }
    if (s_view == V_FILL && (s_alarm || idle >= SCREENS_WATCH_TIMEOUT_MS)) {
        if (s_alarm) screens_show_watch();    /* belt and braces: screens_set_alarm already did */
        else screens_show_clock();
        return;
    }
    if (s_view == V_WATCH && !s_alarm && idle >= SCREENS_WATCH_TIMEOUT_MS) screens_show_clock();
    if (s_view == V_WATCH) {
        ui_watch_tick();
    } else if (s_view == V_FILL) {
        ui_fill_tick();
    } else {
        ui_clock_nightwatch_tick();          /* the bar lives on CLOCK only; WATCH / ALARM never show it */
    }
}

void screens_init(void)
{
    s_clock = ui_clock_create();
    s_watch = ui_watch_create();
    s_alert = ui_alert_create(reset_btn_cb, reset_confirm_cb);
    s_fill = ui_fill_create(fill_back_cb);
    s_ask = ui_ask_create(ask_answer_cb);
    s_wkup = ui_wkup_create();
    s_al = heap_caps_calloc(1, sizeof(al_view_t), MALLOC_CAP_SPIRAM);
    char *lines = heap_caps_calloc(3, AL_LINE_MAX, MALLOC_CAP_SPIRAM);
    if (!s_al || !lines) {
        ESP_LOGE(TAG, "no PSRAM for the alert view - engine alerts not shown");
        free(s_al);
        free(lines);
        s_al = NULL;
    } else {
        s_line = lines;
        s_last_clock = lines + AL_LINE_MAX;
        s_last_watch = lines + 2 * AL_LINE_MAX;
        s_last_clock[0] = s_last_watch[0] = 'x';   /* first tick always draws */
    }
    lv_obj_add_flag(s_clock, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_clock, wake_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_flag(s_alert, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_alert, alert_tap_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_flag(s_wkup, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_wkup, wkup_tap_cb, LV_EVENT_PRESSED, NULL);
    /* EXPECTED?: YES / NO only (10-18) - a press off the buttons does nothing */
    ui_clock_on_alert_line(alert_line_cb);
    lv_timer_create(idle_timer_cb, 1000, NULL);
    screens_show_clock();
    board_backlight(true);                       /* panel is running: light it */
}

void screens_show_clock(void)
{
    show_view(V_CLOCK);
    lv_screen_load(s_clock);
    ESP_LOGI(TAG, "CLOCK");
}

void screens_show_watch(void)
{
    show_view(V_WATCH);
    lv_display_trigger_activity(NULL);           /* the 2-min timer starts now */
    lv_screen_load(s_watch);
    ESP_LOGI(TAG, "WATCH");
}

void screens_show_fill(void)
{
    if (s_alarm) return;                         /* never during an anchor ALARM (ANC-41) */
    show_view(V_FILL);
    lv_display_trigger_activity(NULL);           /* the 2-min timer starts now */
    fill_ctl_fast(true);                         /* fresh levels while the page is open */
    ui_fill_tick();
    lv_screen_load(s_fill);
    ESP_LOGI(TAG, "FILL");
}

void screens_set_alarm(bool on)
{
    s_alarm = on;
    if (on) {
        ui_clock_set_night(false);               /* full brightness (SPEC 2.5) */
        board_backlight(true);
        screens_show_watch();                    /* also away from an engine ALERT: the anchor alarm wins */
    }
}

bool screens_on_watch(void)
{
    return s_view == V_WATCH;
}

bool screens_alarm(void)
{
    return s_alarm;
}
