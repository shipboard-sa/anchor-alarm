/**
 * Purpose: LVGL glue - see display.h.
 * Version: v2 (2026-10-02) - v2: 20-line draw buffers (2 x 32 KB internal DMA RAM) so Wi-Fi fits
 * Scope: PRODUCTION - anchor-box firmware, live on the box at the nav station since 2026-10-02 (installed by signed OTA).
 * Architecture: anchor-box firmware. Partial-render buffers in internal RAM; the flush copies into the RGB
 *   panel's PSRAM frame buffer via esp_lcd_panel_draw_bitmap (synchronous for RGB panels).
 * Invocation: imported by app_main.c and the UI modules.
 */
#include "display.h"

#include "board.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gt911.h"
#include "lvgl.h"

#define BUF_LINES 20   /* 2 x 32 KB internal DMA RAM; 40 lines left too little for Wi-Fi (10-02) */

static const char *TAG = "display";
static SemaphoreHandle_t s_lock;
static volatile bool s_touch;
static volatile uint16_t s_tx, s_ty;

static uint32_t tick_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void flush_cb(lv_display_t *disp, const lv_area_t *a, uint8_t *px)
{
    esp_lcd_panel_draw_bitmap(board_panel(), a->x1, a->y1, a->x2 + 1, a->y2 + 1, px);
    lv_display_flush_ready(disp);
}

static void touch_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    uint16_t x, y;
    bool p = gt911_read(&x, &y);
    s_touch = p;
    s_tx = x;
    s_ty = y;
    data->point.x = x;
    data->point.y = y;
    data->state = p ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

bool display_last_touch(uint16_t *x, uint16_t *y)
{
    *x = s_tx;
    *y = s_ty;
    return s_touch;
}

bool display_lock(uint32_t timeout_ms)
{
    return xSemaphoreTakeRecursive(s_lock, timeout_ms ? pdMS_TO_TICKS(timeout_ms) : portMAX_DELAY) == pdTRUE;
}

void display_unlock(void)
{
    xSemaphoreGiveRecursive(s_lock);
}

static void ui_task(void *arg)
{
    for (;;) {
        display_lock(0);
        uint32_t d = lv_timer_handler();
        display_unlock();
        if (d < 5) d = 5;
        if (d > 30) d = 30;
        vTaskDelay(pdMS_TO_TICKS(d));
    }
}

void display_start(void)
{
    s_lock = xSemaphoreCreateRecursiveMutex();
    lv_init();
    lv_tick_set_cb(tick_ms);

    lv_display_t *disp = lv_display_create(BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    size_t sz = BOARD_LCD_H_RES * BUF_LINES * 2;
    void *b1 = heap_caps_malloc(sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    void *b2 = heap_caps_malloc(sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!b1 || !b2) {
        ESP_LOGE(TAG, "no internal RAM for draw buffers");
        abort();
    }
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, b1, b2, sz, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_cb);

    xTaskCreatePinnedToCore(ui_task, "ui", 8192, NULL, 4, NULL, 1);
}
