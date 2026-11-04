/**
 * Purpose: Board support for the anchor box - see board.h.
 * Version: v1 (2026-10-02)
 * Scope: PRODUCTION - anchor-box firmware, live on the box at the nav station since 2026-10-02 (installed by signed OTA).
 * Architecture: anchor-box board layer. Pin / net sources:
 *   Waveshare wiki ESP32-S3-Touch-LCD-4.3B (pin tables) + schematic nets: EXIO0 = DI0, EXIO1 = CTP_RST,
 *   EXIO2 = DISP (-> AP3032 backlight driver CTRL), EXIO3 = LCD_RST, EXIO4 = SD_CS, EXIO5 = DI1;
 *   DO0 / DO1 = nets OD0 / OD1 from the CH422G OC pins into PC817 optocouplers; I2C SDA 8 / SCL 9;
 *   touch INT GPIO 4; RTC INT GPIO 6. Panel timings: ESPHome rpi_dpi_rgb example for this board.
 * Invocation: imported by app_main.c, display.c, screens.c.
 */
#include "board.h"

#include "ch422g.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gt911.h"

static const char *TAG = "board";

#define PIN_SDA     8
#define PIN_SCL     9
#define PIN_TP_INT  4

/* CH422G IO (EXIO) bits */
#define EXIO_DI0    0
#define EXIO_TP_RST 1
#define EXIO_DISP   2
#define EXIO_LCD_RST 3
#define EXIO_SD_CS  4
#define EXIO_DI1    5

/* CH422G OC bits -> DO0 / DO1. Open drain: writing 0 SINKS the optocoupler LED = output ON; 1 = no
 * output = OFF. The chip powers up with OC high (datasheet 5.6) = OFF. Mapping OC0 -> DO0, OC1 -> DO1 and
 * the polarity were read from the schematic text and PROVEN by the 10-02 wiring test (DO0 -> panel AL-12). */
#define OC_DO0      0
#define OC_DO1      1
#define DO_ACTIVE_LEVEL 0

static i2c_master_bus_handle_t s_bus;
static esp_lcd_panel_handle_t s_panel;

i2c_master_bus_handle_t board_i2c(void) { return s_bus; }
esp_lcd_panel_handle_t board_panel(void) { return s_panel; }

static esp_err_t panel_init(void)
{
    esp_lcd_rgb_panel_config_t cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = 16 * 1000 * 1000,
            .h_res = BOARD_LCD_H_RES,
            .v_res = BOARD_LCD_V_RES,
            .hsync_pulse_width = 30,
            .hsync_back_porch = 30,
            .hsync_front_porch = 210,
            .vsync_pulse_width = 4,
            .vsync_back_porch = 4,
            .vsync_front_porch = 4,
            .flags.pclk_active_neg = 1,      /* if the image shimmers on the bench, flip this */
        },
        .data_width = 16,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = 1,
        .bounce_buffer_size_px = BOARD_LCD_H_RES * 10,   /* DMA from internal RAM, PSRAM fb behind it */
        .hsync_gpio_num = 46,
        .vsync_gpio_num = 3,
        .de_gpio_num = 5,
        .pclk_gpio_num = 7,
        .disp_gpio_num = -1,                 /* DISP is on the expander, not a GPIO */
        .data_gpio_nums = {
            14, 38, 18, 17, 10,              /* D0-D4  = B3-B7 */
            39, 0, 45, 48, 47, 21,           /* D5-D10 = G2-G7 */
            1, 2, 42, 41, 40,                /* D11-D15 = R3-R7 */
        },
        .flags.fb_in_psram = 1,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_rgb_panel(&cfg, &s_panel), TAG, "rgb panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init");
    return ESP_OK;
}

static void touch_reset(void)
{
    /* GT911 address select: INT held LOW while RESET is released -> 0x5D (Goodix guide) */
    gpio_config_t io = {.pin_bit_mask = 1ULL << PIN_TP_INT, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&io);
    gpio_set_level(PIN_TP_INT, 0);
    ch422g_set_io_bit(EXIO_TP_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    ch422g_set_io_bit(EXIO_TP_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    io.mode = GPIO_MODE_INPUT;               /* release INT: the GT911 drives it from now on */
    gpio_config(&io);
    vTaskDelay(pdMS_TO_TICKS(60));
}

esp_err_t board_init(void)
{
    i2c_master_bus_config_t bc = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_SDA,
        .scl_io_num = PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bc, &s_bus), TAG, "i2c bus");

    /* Expander: OC all 1 (both outputs OFF) written before anything else; OC open drain; IO as outputs.
     * IO byte: DI pins and SD_CS high, TP_RST high, LCD_RST high, DISP (backlight) LOW until the panel runs.
     * NOTE: IO_OE drives all 8 IO pins, including the DI0 / DI1 optocoupler inputs - this firmware does not
     * read DIs; whether anything is wired to the box's DI terminals is ASSUMED none - check your wiring (SPEC s.4). */
    uint8_t io = (1u << EXIO_DI0) | (1u << EXIO_TP_RST) | (1u << EXIO_LCD_RST) | (1u << EXIO_SD_CS) |
                 (1u << EXIO_DI1) | 0xC0;
    ESP_RETURN_ON_ERROR(ch422g_init(s_bus, CH422G_SYS_IO_OE | CH422G_SYS_OD_EN, io, 0x0F), TAG, "ch422g");

    ch422g_set_io_bit(EXIO_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    ch422g_set_io_bit(EXIO_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(20));

    touch_reset();
    if (gt911_init(s_bus) != ESP_OK) {
        ESP_LOGW(TAG, "continuing WITHOUT touch");
    }
    ESP_RETURN_ON_ERROR(panel_init(), TAG, "panel");
    return ESP_OK;
}

esp_err_t board_backlight(bool on)
{
    /* AP3032 CTRL: high = on. Dimming by PWM would need fast toggling over I2C - not done (SPEC 2.7). */
    return ch422g_set_io_bit(EXIO_DISP, on);
}

esp_err_t board_output(board_do_t which, bool active)
{
    int bit = which == BOARD_DO0 ? OC_DO0 : OC_DO1;
    return ch422g_set_oc_bit(bit, active ? DO_ACTIVE_LEVEL : !DO_ACTIVE_LEVEL);
}

bool board_output_state(board_do_t which)
{
    int bit = which == BOARD_DO0 ? OC_DO0 : OC_DO1;
    return ((ch422g_oc() >> bit) & 1) == DO_ACTIVE_LEVEL;
}

esp_err_t board_refresh(void)
{
    return ch422g_refresh();
}
