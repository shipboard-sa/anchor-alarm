/**
 * Purpose: The WATCH screen (SPEC 2, 5, 3.5): watch state, distance vs the alarm circle, why it has not locked,
 *   DRAG? / GPS warnings, recent events, and the controls ARM (rode pick, 50 ft steps), DISARM (confirmed), MUTE,
 *   ZONE (pin / AUTO), CLOCK, RE-ARM (same rode, confirmed), PLOT (breadcrumb trail + circle, north up) and FILL
 *   (tank-fill page, ANC-41) and HOT WATER (ANC-43). The six bottom buttons follow the state: OFF = ARM PLOT ZONE
 *   CLOCK FILL HOT; armed = DISARM RE-ARM PLOT CLOCK FILL HOT; ALARM = DISARM MUTE RE-ARM PLOT (5 + 6 empty). In ALARM a red banner covers
 *   the top half, flashing until MUTE. Above the buttons: the monitoring-host notice (Pi / Cerbo down - ANC-25).
 * Version: v8 (2026-10-15) - v8: DOCK / LEAVE (10-15, ANC-44 / Ship's Automation D-151): the ARM page gets a DOCK
 *   button (the engine's boat-mode tap that arms the WF-108 slip fence - not this box's watch); while the engine says
 *   DOCK, WATCH reads 'AT DOCK - slip fence armed' and LEAVE (confirmed -> MOTOR) takes ARM's slot, enabled only while
 *   the engine reports a crew phone aboard (leave_ok). v7 (2026-10-11) - v7: HOT WATER as a sixth bottom button (10-11, ANC-43 / Ship's Automation D-135):
 *   one tap starts a one-off heater run in the engine (a second tap stops it), the button shows 'HOT / 14 min',
 *   'HOT / READY' after it ended hot, 'HOT / held'; the notice line says minutes left, a refusal (bank below the
 *   floor) or how the run ended, below an engine alert / monitoring-host notice; hidden in ALARM; buttons 143 ->
 *   120 px so six fit, labels drop to 20 pt when 28 pt would not fit. v6 (2026-10-09) - v6: FILL as a fifth bottom button (10-09: 'a tap on the clock brings up the other
 *   buttons - another button on that screen is fine'), 'FILLING' while a fill session is on; hidden in ALARM;
 *   buttons 180 -> 143 px wide so five fit. v5 (2026-10-07) - v5: rules-engine alert line (ANQ-10) on the notice line - it outranks the
 *   monitoring-host notice and stays visible under the anchor ALARM banner (banner 350 -> 318 px high).
 *   v4 (2026-10-03) - v4: monitoring-host notice line (hostwatch_ctl.c). v3: RE-ARM + breadcrumb PLOT, state-dependent buttons (10-02, ANQ-16). v2: the real
 *   WATCH screen (step 4). v1: bench placeholder.
 * Scope: PRODUCTION - anchor-box firmware, live on the box at the nav station since 2026-10-02 (installed by signed OTA).
 * Architecture: anchor-box UI over watch_ctl.c (state + commands) and boatclock.c (zone pin + clock label). Built
 *   once; ui_watch_tick() refreshes it at 1 Hz from screens.c's LVGL timer (display lock held). Button callbacks
 *   run in the LVGL task. Bench builds keep the DO0 / DO1 test toggles (CONFIG_ANCHORBOX_BENCH_TESTS).
 * Invocation: imported by screens.c.
 */
#include "ui_watch.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "board.h"
#include "esp_heap_caps.h"
#include "boatmode_ctl.h"
#include "fill_ctl.h"
#include "hotwater_ctl.h"
#include "hostwatch_ctl.h"
#include "boatclock.h"
#include "screens.h"
#include "sdkconfig.h"
#include "watch_ctl.h"

#define RED     0xFF1000
#define DIMRED  0x300000
#define BLACK   0x000000

static lv_obj_t *s_scr, *s_state, *s_line1, *s_line2, *s_warn, *s_log[3], *s_clock, *s_notice;
static char s_engine[128];                   /* ui_watch_set_engine_line (LVGL task only) */
static lv_obj_t *s_banner, *s_banner_txt;
static lv_obj_t *s_rode_ov, *s_disarm_ov, *s_zone_ov, *s_rearm_ov, *s_rearm_txt;
static lv_obj_t *s_leave_ov, *s_dock_btn;   /* ANC-44: LEAVE confirm page, the ARM page's DOCK button */
static lv_obj_t *s_plot_ov, *s_plot, *s_plot_info;
static watch_crumb_t *s_crumbs;           /* PSRAM copy of the track for drawing */
static bool s_flash;

/* the four bottom buttons change with the state */
typedef enum { F_NONE = 0, F_ARM, F_DISARM, F_MUTE, F_REARM, F_PLOT, F_ZONE, F_CLOCK, F_FILL, F_HOT, F_LEAVE } slot_fn_t;
static const char *FN_TXT[] = { "", "ARM", "DISARM", "MUTE", "RE-ARM", "PLOT", "ZONE", "CLOCK", "FILL",
                                "HOT\nWATER", "LEAVE" };
#define SLOTS 6                                  /* 5 = FILL, 6 = HOT WATER; both empty + hidden in ALARM */
#define SLOT_W 120                               /* six across 800 px: 16 + 6 x 120 + 5 x 8 = 776 */
#define SLOT_STEP 128
static lv_obj_t *s_slot[SLOTS], *s_slot_lbl[SLOTS];
static slot_fn_t s_slot_fn[SLOTS];

static lv_obj_t *label(lv_obj_t *p, const lv_font_t *f, int x, int y, const char *txt)
{
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(RED), 0);
    lv_obj_set_pos(l, x, y);
    lv_label_set_text(l, txt);
    return l;
}

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
    /* disabled (e.g. MUTE outside an ALARM): dark, never the theme's bright grey - night vision */
    lv_obj_set_style_bg_color(b, lv_color_hex(BLACK), LV_STATE_DISABLED);
    lv_obj_set_style_border_color(b, lv_color_hex(DIMRED), LV_STATE_DISABLED);
    lv_obj_set_style_text_color(b, lv_color_hex(0x501000), LV_STATE_DISABLED);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)ud);
    lv_obj_set_style_text_color(b, lv_color_hex(RED), 0);      /* the label inherits it, per button state */
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
    if (lbl_out) *lbl_out = l;
    return b;
}

static lv_obj_t *overlay(const char *title)
{
    lv_obj_t *o = lv_obj_create(s_scr);
    lv_obj_set_pos(o, 0, 0);
    lv_obj_set_size(o, 800, 480);
    lv_obj_set_style_bg_color(o, lv_color_hex(BLACK), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    label(o, &lv_font_montserrat_48, 24, 20, title);
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    return o;
}

/* a slot label in 28 pt if every line fits the button, else 20 pt (six 120 px slots, ANC-43) */
static void slot_text(int i, const char *txt)
{
    lv_point_t sz;
    lv_text_get_size(&sz, txt, &lv_font_montserrat_28, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    const lv_font_t *f = sz.x <= SLOT_W - 14 ? &lv_font_montserrat_28 : &lv_font_montserrat_20;
    lv_obj_set_style_text_font(s_slot_lbl[i], f, 0);
    lv_obj_set_style_text_align(s_slot_lbl[i], LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_slot_lbl[i], txt);
    lv_obj_center(s_slot_lbl[i]);
}

/* ---------------------------------------------------------------- callbacks */

static void hide_cb(lv_event_t *e) { lv_obj_add_flag((lv_obj_t *)lv_event_get_user_data(e), LV_OBJ_FLAG_HIDDEN); }

static void rode_cb(lv_event_t *e)
{
    watch_ctl_arm((double)(intptr_t)lv_event_get_user_data(e));
    lv_obj_add_flag(s_rode_ov, LV_OBJ_FLAG_HIDDEN);
    ui_watch_tick();
}

static void disarm_yes_cb(lv_event_t *e)
{
    watch_ctl_disarm();
    lv_obj_add_flag(s_disarm_ov, LV_OBJ_FLAG_HIDDEN);
    ui_watch_tick();
}

/* ANC-44: DOCK on the ARM page = the boat-mode tap that arms the slip fence (the engine's, not this box's watch) */
static void dock_cb(lv_event_t *e)
{
    boatmode_ctl_tap(BM_DOCK);                  /* refused quietly if not allowed - the button is disabled then */
    lv_obj_add_flag(s_rode_ov, LV_OBJ_FLAG_HIDDEN);
    ui_watch_tick();
}

static void leave_yes_cb(lv_event_t *e)
{
    boatmode_ctl_tap(BM_MOTOR);                 /* the engine decides (D-151: a crew phone must be aboard) */
    lv_obj_add_flag(s_leave_ov, LV_OBJ_FLAG_HIDDEN);
    ui_watch_tick();
}

static void rearm_yes_cb(lv_event_t *e)
{
    watch_ctl_rearm();
    lv_obj_add_flag(s_rearm_ov, LV_OBJ_FLAG_HIDDEN);
    ui_watch_tick();
}

static void show(lv_obj_t *o)
{
    lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(o);
}

static void slot_cb(lv_event_t *e)
{
    switch (s_slot_fn[(intptr_t)lv_event_get_user_data(e)]) {
    case F_ARM: show(s_rode_ov); break;
    case F_DISARM: show(s_disarm_ov); break;
    case F_MUTE: watch_ctl_mute(); ui_watch_tick(); break;
    case F_REARM: {
        watch_view_t v;
        watch_ctl_get(&v);
        char t[200];
        if (v.rode_ft > 0)
            snprintf(t, sizeof(t), "Drops the old circle and starts a fresh watch from here,\nrode %.0f ft as before. "
                     "Your call - check the breadcrumbs first.", v.rode_ft);
        else
            snprintf(t, sizeof(t), "Drops the old circle and starts a fresh watch from here,\nno rode given. "
                     "Your call - check the breadcrumbs first.");
        lv_label_set_text(s_rearm_txt, t);
        show(s_rearm_ov);
        break;
    }
    case F_PLOT: show(s_plot_ov); lv_obj_invalidate(s_plot); break;
    case F_ZONE: show(s_zone_ov); break;
    case F_CLOCK: screens_show_clock(); break;
    case F_FILL: screens_show_fill(); break;
    case F_HOT: hotwater_ctl_tap(); ui_watch_tick(); break;   /* one tap starts / stops (ANC-43); the engine decides */
    case F_LEAVE: show(s_leave_ov); break;     /* confirmed: it ends the theft fence (ANC-44) */
    default: break;
    }
}

static const char *ZONE_IDS[] = { "", "CT", "ET", "BS", "AT", "N-4", "N-5", "N-6" };
static const char *ZONE_TXT[] = { "AUTO", "US Central", "US Eastern", "Bahamas", "Atlantic", "UTC-4", "UTC-5",
                                  "UTC-6" };

static void zone_cb(lv_event_t *e)
{
    const char *zid = ZONE_IDS[(intptr_t)lv_event_get_user_data(e)];
    boatclock_pin(zid[0] ? zid : NULL);
    lv_obj_add_flag(s_zone_ov, LV_OBJ_FLAG_HIDDEN);
}

#if CONFIG_ANCHORBOX_BENCH_TESTS
static void toggle_cb(lv_event_t *e)
{
    board_do_t which = (board_do_t)(intptr_t)lv_event_get_user_data(e);
    board_output(which, !board_output_state(which));
}
#endif

/* ---------------------------------------------------------------- breadcrumb plot */

#define PLOT_PX 470                 /* square plot area */
#define M_PER_DEG 111195.0

static double s_plot_edge_m;
static int s_plot_trail_n;

static void draw_line(lv_layer_t *layer, int x1, int y1, int x2, int y2, uint32_t col, int w)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = lv_color_hex(col);
    d.width = w;
    d.round_start = d.round_end = 1;
    d.p1.x = x1;
    d.p1.y = y1;
    d.p2.x = x2;
    d.p2.y = y2;
    lv_draw_line(layer, &d);
}

static void draw_ring(lv_layer_t *layer, int x, int y, int r, uint32_t col, int w)
{
    if (r < 1) r = 1;
    if (r > 30000) return;
    lv_draw_arc_dsc_t a;
    lv_draw_arc_dsc_init(&a);
    a.color = lv_color_hex(col);
    a.width = w;
    a.start_angle = 0;
    a.end_angle = 360;
    a.center.x = x;
    a.center.y = y;
    a.radius = (uint16_t)r;
    lv_draw_arc(layer, &a);
}

/* north up, east right; centred on the circle centre (or the boat when there is none) */
static void plot_draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_obj_t *obj = lv_event_get_target(e);
    lv_area_t ar;
    lv_obj_get_coords(obj, &ar);
    int cx = (ar.x1 + ar.x2) / 2, cy = (ar.y1 + ar.y2) / 2;

    watch_view_t v;
    watch_ctl_get(&v);
    double lat0, lon0;
    int n = s_crumbs ? watch_ctl_track(s_crumbs, WATCH_TRACK_N, &lat0, &lon0) : 0;
    double rlat, rlon;
    if (v.have_centre) { rlat = v.centre_lat; rlon = v.centre_lon; }
    else if (v.have_fix) { rlat = v.lat; rlon = v.lon; }
    else if (n) { rlat = lat0 + s_crumbs[n - 1].lat_off; rlon = lon0 + s_crumbs[n - 1].lon_off; }
    else return;
    double kx = M_PER_DEG * cos(rlat * M_PI / 180.0), ky = M_PER_DEG;

    /* scale: the circle and the whole trail fit, never tighter than +-30 m */
    double R = 30.0;
    if (v.have_alarm_radius && v.alarm_radius_m > R) R = v.alarm_radius_m;
    for (int i = 0; i < n; i++) {
        double dx = (lon0 + s_crumbs[i].lon_off - rlon) * kx, dy = (lat0 + s_crumbs[i].lat_off - rlat) * ky;
        double d = sqrt(dx * dx + dy * dy);
        if (d > R && d < 3000) R = d;
    }
    R *= 1.1;
    double s = (PLOT_PX / 2 - 6) / R;

    draw_line(layer, cx, ar.y1 + 4, cx, ar.y1 + 24, DIMRED + 0x200000, 2);              /* north tick */
    if (v.have_alarm_radius && v.state != AW_OFF) draw_ring(layer, cx, cy, (int)(v.alarm_radius_m * s), RED, 3);
    if (v.have_centre && v.state != AW_OFF) {                                            /* anchor / centre */
        draw_line(layer, cx - 9, cy, cx + 9, cy, RED, 3);
        draw_line(layer, cx, cy - 9, cx, cy + 9, RED, 3);
    }
    int step = n > 700 ? n / 700 : 1;                                                    /* <= ~700 segments */
    int px = 0, py = 0;
    bool have_prev = false;
    for (int i = 0; i < n; i += step) {
        double dx = (lon0 + s_crumbs[i].lon_off - rlon) * kx, dy = (lat0 + s_crumbs[i].lat_off - rlat) * ky;
        int x = cx + (int)lround(dx * s), y = cy - (int)lround(dy * s);
        if (have_prev) draw_line(layer, px, py, x, y, 0x902000, 2);
        px = x;
        py = y;
        have_prev = true;
    }
    if (v.have_fix) {                                                                    /* the boat now */
        double dx = (v.lon - rlon) * kx, dy = (v.lat - rlat) * ky;
        draw_ring(layer, cx + (int)lround(dx * s), cy - (int)lround(dy * s), 5, RED, 10);
    }

    s_plot_edge_m = R;           /* the info label is set in ui_watch_tick(), never while drawing */
    s_plot_trail_n = n;
}

/* ---------------------------------------------------------------- build */

lv_obj_t *ui_watch_create(void)
{
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(BLACK), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    s_state = label(s_scr, &lv_font_montserrat_48, 24, 14, "OFF");
    s_clock = label(s_scr, &lv_font_montserrat_20, 520, 26, "");
    s_line1 = label(s_scr, &lv_font_montserrat_28, 24, 86, "");
    s_line2 = label(s_scr, &lv_font_montserrat_20, 24, 130, "");
    lv_obj_set_width(s_line2, 760);
    lv_label_set_long_mode(s_line2, LV_LABEL_LONG_DOT);
    s_warn = label(s_scr, &lv_font_montserrat_28, 24, 166, "");
    for (int i = 0; i < 3; i++) {
        s_log[i] = label(s_scr, &lv_font_montserrat_20, 24, 218 + 34 * i, "");
        lv_obj_set_width(s_log[i], 760);
        lv_label_set_long_mode(s_log[i], LV_LABEL_LONG_DOT);
    }

    s_notice = label(s_scr, &lv_font_montserrat_20, 24, 324, "");     /* monitoring host down (ANC-25) */
    lv_obj_set_width(s_notice, 760);
    lv_label_set_long_mode(s_notice, LV_LABEL_LONG_DOT);

    for (int i = 0; i < SLOTS; i++)             /* labels + jobs set per state in ui_watch_tick() */
        s_slot[i] = button(s_scr, 16 + SLOT_STEP * i, 362, SLOT_W, 104, "", slot_cb, i, &s_slot_lbl[i]);
    s_crumbs = heap_caps_malloc(sizeof(watch_crumb_t) * WATCH_TRACK_N, MALLOC_CAP_SPIRAM);
#if CONFIG_ANCHORBOX_BENCH_TESTS
    button(s_scr, 520, 160, 120, 60, "DO0", toggle_cb, BOARD_DO0, NULL);
    button(s_scr, 660, 160, 120, 60, "DO1", toggle_cb, BOARD_DO1, NULL);
#endif

    /* ALARM banner: full-width red, black text (a coloured screen is reserved for ALARM - ANC-06) */
    s_banner = lv_obj_create(s_scr);
    lv_obj_set_pos(s_banner, 0, 0);
    lv_obj_set_size(s_banner, 800, 318);        /* ends above the notice line (y 324): an engine alert stays visible */
    lv_obj_set_style_bg_color(s_banner, lv_color_hex(RED), 0);
    lv_obj_set_style_bg_opa(s_banner, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_banner, 0, 0);
    lv_obj_set_style_radius(s_banner, 0, 0);
    lv_obj_remove_flag(s_banner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_banner, LV_OBJ_FLAG_CLICKABLE);
    s_banner_txt = lv_label_create(s_banner);
    lv_obj_set_style_text_font(s_banner_txt, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_banner_txt, lv_color_hex(BLACK), 0);
    lv_obj_set_style_text_align(s_banner_txt, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_banner_txt);
    lv_obj_add_flag(s_banner, LV_OBJ_FLAG_HIDDEN);

    /* ARM: rode out, 50 ft steps (no chain counter - SPEC 5.1, 5.4) */
    s_rode_ov = overlay("ARM - rode out?");
    label(s_rode_ov, &lv_font_montserrat_20, 24, 84, "Arm after the anchor is set (backed down, neutral). Rode is a guess - 50 ft steps.");
    /* 100 m (328 ft) of chain aboard, all chain (10-02): 50..300 ft, then ALL OUT */
    for (int i = 0; i < 7; i++) {
        char t[20];
        int ft = i < 6 ? 50 * (i + 1) : 328;
        if (i < 6) snprintf(t, sizeof(t), "%d ft", ft);
        else snprintf(t, sizeof(t), "ALL 328 ft");
        button(s_rode_ov, 16 + 196 * (i % 4), 130 + 112 * (i / 4), i < 6 ? 180 : 376, 96, t, rode_cb, ft, NULL);
    }
    button(s_rode_ov, 16, 370, 248, 96, "don't know", rode_cb, 0, NULL);
    s_dock_btn = button(s_rode_ov, 276, 370, 248, 96, "DOCK\nslip fence", dock_cb, 0, NULL);   /* ANC-44: in a slip, arm the fence instead */
    button(s_rode_ov, 536, 370, 248, 96, "CANCEL", hide_cb, (intptr_t)s_rode_ov, NULL);

    s_rearm_ov = overlay("RE-ARM here?");
    s_rearm_txt = label(s_rearm_ov, &lv_font_montserrat_28, 24, 110, "");
    button(s_rearm_ov, 16, 300, 376, 140, "YES - RE-ARM", rearm_yes_cb, 0, NULL);
    button(s_rearm_ov, 408, 300, 376, 140, "CANCEL", hide_cb, (intptr_t)s_rearm_ov, NULL);

    /* breadcrumb PLOT: square plot left, notes + BACK right */
    s_plot_ov = overlay("");
    s_plot = lv_obj_create(s_plot_ov);
    lv_obj_set_pos(s_plot, 5, 5);
    lv_obj_set_size(s_plot, PLOT_PX, PLOT_PX);
    lv_obj_set_style_bg_color(s_plot, lv_color_hex(BLACK), 0);
    lv_obj_set_style_border_color(s_plot, lv_color_hex(DIMRED), 0);
    lv_obj_set_style_border_width(s_plot, 2, 0);
    lv_obj_set_style_radius(s_plot, 0, 0);
    lv_obj_remove_flag(s_plot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_plot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_plot, plot_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    s_plot_info = label(s_plot_ov, &lv_font_montserrat_20, 495, 20, "");
    button(s_plot_ov, 495, 362, 290, 104, "BACK", hide_cb, (intptr_t)s_plot_ov, NULL);

    s_disarm_ov = overlay("DISARM the anchor watch?");
    label(s_disarm_ov, &lv_font_montserrat_28, 24, 120, "The watch stops and the panel output is released.");
    button(s_disarm_ov, 16, 300, 376, 140, "YES - DISARM", disarm_yes_cb, 0, NULL);
    button(s_disarm_ov, 408, 300, 376, 140, "CANCEL", hide_cb, (intptr_t)s_disarm_ov, NULL);

    s_leave_ov = overlay("LEAVE the dock?");
    label(s_leave_ov, &lv_font_montserrat_28, 24, 120, "Ends the slip fence - the boat mode goes to MOTOR.\n"
          "Allowed while one of our phones is aboard.");
    button(s_leave_ov, 16, 300, 376, 140, "YES - LEAVE", leave_yes_cb, 0, NULL);
    button(s_leave_ov, 408, 300, 376, 140, "CANCEL", hide_cb, (intptr_t)s_leave_ov, NULL);

    s_zone_ov = overlay("Clock time zone");
    for (int i = 0; i < 8; i++)
        button(s_zone_ov, 16 + 196 * (i % 4), 110 + 112 * (i / 4), 180, 96, ZONE_TXT[i], zone_cb, i, NULL);
    button(s_zone_ov, 16, 360, 768, 100, "CANCEL", hide_cb, (intptr_t)s_zone_ov, NULL);

    ui_watch_tick();
    return s_scr;
}

/* ---------------------------------------------------------------- refresh */

void ui_watch_tick(void)
{
    watch_view_t v;
    watch_ctl_get(&v);
    char buf[200];

    int el = (int)v.elapsed_s;
    switch (v.state) {
    case AW_OFF: snprintf(buf, sizeof(buf), "WATCH OFF"); break;
    case AW_ACQUIRING: snprintf(buf, sizeof(buf), "ACQUIRING  %d:%02d", el / 60, el % 60); break;
    case AW_LOCKED: snprintf(buf, sizeof(buf), "LOCKED"); break;
    case AW_CONSERVATIVE: snprintf(buf, sizeof(buf), "CONSERVATIVE"); break;
    case AW_ALARM: snprintf(buf, sizeof(buf), "ALARM"); break;
    }
    lv_label_set_text(s_state, buf);

    char clk[48];
    boatclock_label(clk, sizeof(clk));
    lv_label_set_text(s_clock, clk);

    static bm_view_t bv;                         /* LVGL task only; off the stack (DOCK / LEAVE, ANC-44) */
    boatmode_ctl_get(&bv);
    char bl1[64], bl2[112];
    bm_lines(&bv, bl1, sizeof(bl1), bl2, sizeof(bl2));
    bool dock_lines = v.state == AW_OFF && bl1[0];   /* the box's own watch, when on, keeps its lines */

    if (dock_lines)
        snprintf(buf, sizeof(buf), "%s", bl1);
    else if (v.state == AW_OFF)
        snprintf(buf, sizeof(buf), "Tap ARM once the anchor is set.");
    else if (v.have_distance && v.have_alarm_radius)
        snprintf(buf, sizeof(buf), "%.0f m from the %s - alarm at %.0f m", v.distance_m,
                 v.state == AW_CONSERVATIVE ? "centre" : "anchor", v.alarm_radius_m);
    else if (v.have_distance)
        snprintf(buf, sizeof(buf), "%.0f m from the provisional centre", v.distance_m);
    else
        snprintf(buf, sizeof(buf), "%s", v.have_pos ? "observing the swing..." : "waiting for GPS position");
    lv_label_set_text(s_line1, buf);

    char rode[24];
    if (v.rode_ft > 0) snprintf(rode, sizeof(rode), "rode %.0f ft", v.rode_ft);
    else snprintf(rode, sizeof(rode), "rode not given");
    if (v.state == AW_ACQUIRING || v.state == AW_CONSERVATIVE)
        snprintf(buf, sizeof(buf), "%s | not locked: %s", rode, v.reason[0] ? v.reason : "-");
    else if (v.state != AW_OFF)
        snprintf(buf, sizeof(buf), "%s", rode);
    else
        buf[0] = 0;
    if (dock_lines) snprintf(buf, sizeof(buf), "%s", bl2);
    lv_label_set_text(s_line2, buf);

    buf[0] = 0;
    if (v.drag) snprintf(buf, sizeof(buf), "DRAG?  distance keeps rising");
    if (v.stale) snprintf(buf, sizeof(buf), "GPS STALE - the watch is blind");
    if (!v.have_pos && v.state != AW_OFF) snprintf(buf, sizeof(buf), "NO GPS POSITION");
    lv_label_set_text(s_warn, buf);

    for (int i = 0; i < 3; i++) lv_label_set_text(s_log[i], i < v.n_log ? v.log[i] : "");

    static hw_view_t hv;                         /* LVGL task only; off the stack (HOT WATER, ANC-43) */
    hotwater_ctl_get(&hv);
    char nt[96], line[128] = "";
    hostwatch_ctl_notice(nt, sizeof(nt));
    if (s_engine[0]) {
        strlcpy(line, s_engine, sizeof(line));  /* an unacked engine alarm outranks a monitoring-host notice */
    } else if (nt[0]) {
        char *nl = strchr(nt, '\n');           /* two problems -> one line here */
        if (nl) *nl = 0;
        snprintf(line, sizeof(line), "AUTOMATION: %s%s%s", nt, nl ? "  |  " : "", nl ? nl + 1 : "");
    } else {
        hw_notice(&hv, line, sizeof(line));     /* HOT WATER minutes left / refusal / how it ended (ANC-43) */
    }
    lv_label_set_text(s_notice, line);

    /* bottom buttons follow the state (10-02): OFF / armed / ALARM */
    /* + FILL as the fifth (10-09, ANC-41: 'another button on that screen is fine'); never during ALARM */
    /* + HOT WATER as the sixth (10-11, ANC-43: the box is the only touchscreen); never during ALARM */
    static const slot_fn_t OFF_FN[SLOTS] = { F_ARM, F_PLOT, F_ZONE, F_CLOCK, F_FILL, F_HOT };
    static const slot_fn_t ARMED_FN[SLOTS] = { F_DISARM, F_REARM, F_PLOT, F_CLOCK, F_FILL, F_HOT };
    static const slot_fn_t ALARM_FN[SLOTS] = { F_DISARM, F_MUTE, F_REARM, F_PLOT, F_NONE, F_NONE };
    /* + at DOCK (the engine says so) LEAVE takes ARM's place - no anchor watch in a slip (10-15, ANC-44) */
    static const slot_fn_t DOCK_FN[SLOTS] = { F_LEAVE, F_PLOT, F_ZONE, F_CLOCK, F_FILL, F_HOT };
    const slot_fn_t *fn = v.state == AW_OFF ? (bm_at_dock(&bv) ? DOCK_FN : OFF_FN)
                        : v.state == AW_ALARM ? ALARM_FN : ARMED_FN;
    static fl_view_t fv;                         /* LVGL task only; off the stack */
    char fsmall[16];
    fill_ctl_get(&fv);
    bool filling = fl_tab(&fv, fsmall, sizeof(fsmall)) > 0;
    hw_button_t hb;
    hw_button(&hv, &hb);
    char hot_txt[24];
    snprintf(hot_txt, sizeof(hot_txt), "%s\n%s", hb.top, hb.bottom);
    for (int i = 0; i < SLOTS; i++) {
        s_slot_fn[i] = fn[i];
        slot_text(i, fn[i] == F_HOT ? hot_txt : fn[i] == F_FILL && filling ? "FILLING" : FN_TXT[fn[i]]);
        lv_obj_set_style_border_width(s_slot[i], fn[i] == F_HOT && hb.active ? 6 : 3, 0);   /* a run on: heavier */
        if (fn[i] == F_NONE) lv_obj_add_flag(s_slot[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(s_slot[i], LV_OBJ_FLAG_HIDDEN);
        if ((fn[i] == F_MUTE && v.muted) || (fn[i] == F_FILL && !fv.wifi) || (fn[i] == F_HOT && !hb.enabled) ||
            (fn[i] == F_LEAVE && !bm_leave_ok(&bv)))
            lv_obj_add_state(s_slot[i], LV_STATE_DISABLED);
        else lv_obj_remove_state(s_slot[i], LV_STATE_DISABLED);
    }

    if (bm_dock_ok(&bv)) lv_obj_remove_state(s_dock_btn, LV_STATE_DISABLED);
    else lv_obj_add_state(s_dock_btn, LV_STATE_DISABLED);
    if (!lv_obj_has_flag(s_leave_ov, LV_OBJ_FLAG_HIDDEN) && !bm_leave_ok(&bv))
        lv_obj_add_flag(s_leave_ov, LV_OBJ_FLAG_HIDDEN);   /* the last phone left / the cockpit ended DOCK meanwhile */

    if (s_plot_ov && !lv_obj_has_flag(s_plot_ov, LV_OBJ_FLAG_HIDDEN)) {
        snprintf(buf, sizeof(buf), "N up, E right\n\nplot edge %.0f m\n%s\n+ = anchor\ndot = boat now\n\ntrail %d min%s",
                 s_plot_edge_m, v.have_alarm_radius && v.state != AW_OFF ? "ring = alarm circle" : "no circle (OFF)",
                 s_plot_trail_n / 6, v.state == AW_ALARM ? "\n\nALARM - see the trail" : "");
        lv_label_set_text(s_plot_info, buf);
        lv_obj_invalidate(s_plot);           /* redraw the trail once a second */
    }

    /* the banner never covers an open PLOT / RE-ARM / DISARM / ARM / ZONE page - that is where the decision is made */
    bool overlay_open = !lv_obj_has_flag(s_plot_ov, LV_OBJ_FLAG_HIDDEN) ||
                        !lv_obj_has_flag(s_rearm_ov, LV_OBJ_FLAG_HIDDEN) ||
                        !lv_obj_has_flag(s_disarm_ov, LV_OBJ_FLAG_HIDDEN) ||
                        !lv_obj_has_flag(s_rode_ov, LV_OBJ_FLAG_HIDDEN) || !lv_obj_has_flag(s_zone_ov, LV_OBJ_FLAG_HIDDEN) ||
                        !lv_obj_has_flag(s_leave_ov, LV_OBJ_FLAG_HIDDEN);
    if (v.state == AW_ALARM && !overlay_open) {
        if (v.have_distance)
            snprintf(buf, sizeof(buf), "ANCHOR ALARM\n%.0f m - circle %.0f m%s", v.distance_m,
                     v.have_alarm_radius ? v.alarm_radius_m : 0, v.muted ? "\nMUTED - panel stays on" : "");
        else
            snprintf(buf, sizeof(buf), "ANCHOR ALARM%s", v.muted ? "\nMUTED" : "");
        lv_label_set_text(s_banner_txt, buf);
        lv_obj_center(s_banner_txt);
        s_flash = v.muted ? true : !s_flash;     /* flash at 1 Hz until MUTE, then steady */
        lv_obj_set_style_bg_opa(s_banner, s_flash ? LV_OPA_COVER : LV_OPA_30, 0);
        lv_obj_remove_flag(s_banner, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_banner);
    } else {
        lv_obj_add_flag(s_banner, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_watch_set_engine_line(const char *txt)
{
    strlcpy(s_engine, txt ? txt : "", sizeof(s_engine));
}
