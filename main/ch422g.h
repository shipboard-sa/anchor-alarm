/**
 * Purpose: Minimal driver for the WCH CH422G I/O expander on the anchor box (Waveshare ESP32-S3-Touch-LCD-4.3B).
 * Version: v1 (2026-10-02)
 * Scope: PRODUCTION - anchor-box firmware, live on the box at the nav station since 2026-10-02 (installed by signed OTA).
 * Architecture: anchor-box firmware, board layer. Written from the WCH CH422 datasheet (CH422DS1 EN, s.5-6):
 *   four "commands" whose first byte is an I2C address - 48H set system params, 46H write OC3-0,
 *   70H write IO7-0, 4DH read IO7-0 (7-bit 0x24 / 0x23 / 0x38 / 0x26). Caches every register so the whole
 *   chip can be rewritten periodically (datasheet s.8.3, anti-interference). Clean room: nothing from
 *   the original proprietary firmware (ANC-01). Called by board.c only.
 * Invocation: imported by board.c; ch422g_init() after the I2C bus exists.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

/* System parameter bits (datasheet 6.1: [SLEEP]00[OD_EN]0[A_SCAN]0[IO_OE]) */
#define CH422G_SYS_IO_OE  0x01   /* IO7-0 are outputs */
#define CH422G_SYS_A_SCAN 0x04   /* LED scan mode - never used here */
#define CH422G_SYS_OD_EN  0x10   /* OC3-0 open drain: 0 = sink (low), 1 = no output */

/* Power-on order matters: the OC register is written FIRST (all 1 = no output) so the isolated outputs
 * never pulse on, then the mode, then the IO pins. */
esp_err_t ch422g_init(i2c_master_bus_handle_t bus, uint8_t sys, uint8_t io, uint8_t oc);
esp_err_t ch422g_set_io_bit(int bit, bool level);
esp_err_t ch422g_set_oc_bit(int bit, bool level);    /* level 1 = high / no output (open drain) */
uint8_t   ch422g_oc(void);
esp_err_t ch422g_refresh(void);                     /* rewrite OC, SYS, IO from the cache */
