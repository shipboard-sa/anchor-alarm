/**
 * Purpose: CH422G I/O expander driver - see ch422g.h.
 * Version: v1 (2026-10-02)
 * Scope: PRODUCTION - anchor-box firmware, live on the box at the nav station since 2026-10-02 (installed by signed OTA).
 * Architecture: anchor-box board layer; one I2C "device" per CH422G command address (datasheet s.6).
 * Invocation: imported by board.c.
 */
#include "ch422g.h"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define ADDR_SYS  0x24   /* 48H >> 1 */
#define ADDR_OC   0x23   /* 46H >> 1 */
#define ADDR_IO_W 0x38   /* 70H >> 1 */
#define I2C_HZ    100000 /* conservative; shares the bus with GT911 + RTC */
#define TMO_MS    50

static const char *TAG = "ch422g";
static i2c_master_dev_handle_t s_sys, s_oc, s_io;
static uint8_t s_sys_val, s_io_val, s_oc_val;
static SemaphoreHandle_t s_mux;

static esp_err_t add(i2c_master_bus_handle_t bus, uint16_t addr, i2c_master_dev_handle_t *out)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = I2C_HZ,
    };
    return i2c_master_bus_add_device(bus, &cfg, out);
}

static esp_err_t put(i2c_master_dev_handle_t dev, uint8_t v)
{
    return i2c_master_transmit(dev, &v, 1, TMO_MS);
}

esp_err_t ch422g_init(i2c_master_bus_handle_t bus, uint8_t sys, uint8_t io, uint8_t oc)
{
    s_mux = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_mux, ESP_ERR_NO_MEM, TAG, "mutex");
    ESP_RETURN_ON_ERROR(add(bus, ADDR_OC, &s_oc), TAG, "add OC");
    ESP_RETURN_ON_ERROR(add(bus, ADDR_SYS, &s_sys), TAG, "add SYS");
    ESP_RETURN_ON_ERROR(add(bus, ADDR_IO_W, &s_io), TAG, "add IO");
    s_oc_val = oc & 0x0F;
    s_sys_val = sys;
    s_io_val = io;
    ESP_RETURN_ON_ERROR(ch422g_refresh(), TAG, "first write");
    ESP_LOGI(TAG, "sys 0x%02x io 0x%02x oc 0x%x", s_sys_val, s_io_val, s_oc_val);
    return ESP_OK;
}

esp_err_t ch422g_refresh(void)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    esp_err_t err = put(s_oc, s_oc_val);           /* outputs first: never a pulse */
    if (err == ESP_OK) err = put(s_sys, s_sys_val);
    if (err == ESP_OK) err = put(s_io, s_io_val);
    xSemaphoreGive(s_mux);
    return err;
}

esp_err_t ch422g_set_io_bit(int bit, bool level)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_io_val = level ? (s_io_val | (1u << bit)) : (s_io_val & ~(1u << bit));
    esp_err_t err = put(s_io, s_io_val);
    xSemaphoreGive(s_mux);
    return err;
}

esp_err_t ch422g_set_oc_bit(int bit, bool level)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_oc_val = level ? (s_oc_val | (1u << bit)) : (s_oc_val & ~(1u << bit));
    s_oc_val &= 0x0F;
    esp_err_t err = put(s_oc, s_oc_val);
    xSemaphoreGive(s_mux);
    return err;
}

uint8_t ch422g_oc(void)
{
    return s_oc_val;
}
