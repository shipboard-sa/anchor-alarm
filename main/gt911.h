/**
 * Purpose: Minimal GT911 capacitive touch reader for the anchor box (single point is all the UI needs).
 * Version: v1 (2026-10-02)
 * Scope: PRODUCTION - anchor-box firmware, live on the box at the nav station since 2026-10-02 (installed by signed OTA).
 * Architecture: anchor-box board layer. From the Goodix GT911 programming guide: 16-bit big-endian register
 *   addresses; 0x8140 product ID ("911"); 0x814E status (bit 7 = buffer ready, bits 3-0 = touch count, write 0
 *   to release the buffer); point 1 at 0x814F (track id, X lo, X hi, Y lo, Y hi). I2C address 0x5D when INT
 *   is held low while reset is released (board.c does that). Clean room (ANC-01). Called by display.c.
 * Invocation: imported by board.c (gt911_init) and display.c (gt911_read).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

esp_err_t gt911_init(i2c_master_bus_handle_t bus);
/* true while touched; x/y in panel pixels. Keeps the last state when the chip has no new report. */
bool gt911_read(uint16_t *x, uint16_t *y);
