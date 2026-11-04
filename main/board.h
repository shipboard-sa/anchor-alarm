/**
 * Purpose: Board support for the anchor box (Waveshare ESP32-S3-Touch-LCD-4.3B): I2C bus, CH422G, RGB panel,
 *   backlight, touch reset, and the two isolated outputs DO0 / DO1 - OFF from power-up.
 * Version: v1 (2026-10-02)
 * Scope: PRODUCTION - anchor-box firmware, live on the box at the nav station since 2026-10-02 (installed by signed OTA).
 * Architecture: anchor-box firmware, bottom layer. Hardware facts ONLY from Waveshare's wiki + schematic
 *   (ESP32-S3-Touch-LCD-4.3B-Sch.pdf, 2024-10-23), the CH422 / GT911 / AP3032 datasheets and ESPHome's
 *   published panel timings for this board - clean room, nothing from the original proprietary firmware (ANC-01).
 *   Used by app_main.c (init, refresh), display.c (panel, touch), screens.c (backlight).
 * Invocation: imported; board_init() once at boot before anything else.
 */
#pragma once

#include <stdbool.h>
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_lcd_types.h"

#define BOARD_LCD_H_RES 800
#define BOARD_LCD_V_RES 480

/* Isolated outputs (SPEC 1.5, wiki Outputs-and-Alarm). DO0 -> panel Anchor Drag input (AL-12, ANC-17, as wired
 * 10-02); DO1 -> panel Automation input (AL-13, monitoring host down - ANC-25; owner wires it). */
typedef enum { BOARD_DO0 = 0, BOARD_DO1 = 1 } board_do_t;

esp_err_t board_init(void);
i2c_master_bus_handle_t board_i2c(void);
esp_lcd_panel_handle_t board_panel(void);
esp_err_t board_backlight(bool on);
esp_err_t board_output(board_do_t which, bool active);
bool board_output_state(board_do_t which);
esp_err_t board_refresh(void);   /* rewrite the expander every few seconds (CH422 datasheet 8.3) */
