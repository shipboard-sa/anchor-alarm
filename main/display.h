/**
 * Purpose: LVGL glue for the anchor box - display driver (RGB panel), touch input, tick, and the UI task + lock.
 * Version: v1 (2026-10-02)
 * Scope: PRODUCTION - anchor-box firmware, live on the box at the nav station since 2026-10-02 (installed by signed OTA).
 * Architecture: anchor-box firmware, between board.c (panel, GT911) and the screens (screens.c, ui_*.c).
 *   LVGL v9 (MIT) from the Espressif component registry. Every LVGL call outside the UI task must hold
 *   display_lock().
 * Invocation: imported by app_main.c (display_start) and any module touching LVGL objects.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

void display_start(void);
bool display_lock(uint32_t timeout_ms);
void display_unlock(void);
/* last raw touch (for the bench touch test on the WATCH placeholder) */
bool display_last_touch(uint16_t *x, uint16_t *y);
