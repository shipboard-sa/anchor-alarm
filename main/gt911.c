/**
 * Purpose: GT911 touch reader - see gt911.h.
 * Version: v1 (2026-10-02)
 * Scope: PRODUCTION - anchor-box firmware, live on the box at the nav station since 2026-10-02 (installed by signed OTA).
 * Architecture: anchor-box board layer.
 * Invocation: imported by board.c and display.c.
 */
#include "gt911.h"

#include <string.h>
#include "esp_check.h"
#include "esp_log.h"

#define GT911_ADDR    0x5D
#define REG_PRODUCT   0x8140
#define REG_STATUS    0x814E
#define REG_POINT1    0x814F
#define TMO_MS        20

static const char *TAG = "gt911";
static i2c_master_dev_handle_t s_dev;
static bool s_pressed;
static uint16_t s_x, s_y;

static esp_err_t rd(uint16_t reg, uint8_t *buf, size_t n)
{
    uint8_t a[2] = {reg >> 8, reg & 0xFF};
    return i2c_master_transmit_receive(s_dev, a, 2, buf, n, TMO_MS);
}

static esp_err_t wr8(uint16_t reg, uint8_t v)
{
    uint8_t a[3] = {reg >> 8, reg & 0xFF, v};
    return i2c_master_transmit(s_dev, a, 3, TMO_MS);
}

esp_err_t gt911_init(i2c_master_bus_handle_t bus)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = GT911_ADDR,
        .scl_speed_hz = 100000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &cfg, &s_dev), TAG, "add");
    uint8_t id[5] = {0};
    esp_err_t err = rd(REG_PRODUCT, id, 4);
    if (err != ESP_OK) {
        /* the box's touch has NEVER worked (OQ-098): say so loudly, keep running without touch */
        ESP_LOGE(TAG, "no answer at 0x%02X (%s) - touch hardware / wiring fault?", GT911_ADDR, esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "product id '%s'", (char *)id);
    return ESP_OK;
}

bool gt911_read(uint16_t *x, uint16_t *y)
{
    uint8_t st = 0;
    if (s_dev && rd(REG_STATUS, &st, 1) == ESP_OK && (st & 0x80)) {
        uint8_t n = st & 0x0F;
        if (n > 0) {
            uint8_t p[5];
            if (rd(REG_POINT1, p, 5) == ESP_OK) {
                s_x = p[1] | (p[2] << 8);
                s_y = p[3] | (p[4] << 8);
                s_pressed = true;
            }
        } else {
            s_pressed = false;
        }
        wr8(REG_STATUS, 0);
    }
    *x = s_x;
    *y = s_y;
    return s_pressed;
}
