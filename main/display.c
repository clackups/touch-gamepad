/*
 * ST7701 480x480 RGB panel bring-up and LVGL port integration.
 *
 * The ST7701 controller is initialized over a 3-wire SPI bus while pixel data
 * travels over the 16-bit RGB parallel bus. LVGL is driven through the
 * esp_lvgl_port helper which owns the flush task and tick source.
 *
 * ASCII only. See AGENTS.md.
 */
#include "display.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_lcd_panel_io_additions.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_st7701.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"

#include "boards.h"

static const char *TAG = "display";

/*
 * Height (in scan lines) of the internal SRAM bounce buffer used by the RGB
 * panel. Without a bounce buffer the LCD DMA reads pixels straight from PSRAM
 * over the shared AHB bus; under heavy PSRAM traffic those reads stall and the
 * panel shows random colorful parallel lines instead of the framebuffer. A
 * small bounce buffer (a handful of lines) is copied from PSRAM in an ISR and
 * fed to the panel from fast internal RAM, which removes the artifacts. Ten
 * lines evenly divide the 480-line panel and fit comfortably in internal RAM.
 */
#define BOARD_LCD_BOUNCE_BUFFER_LINES 10

static esp_lcd_panel_handle_t s_panel = NULL;
static lv_display_t *s_display = NULL;

static const esp_lcd_rgb_timing_t s_panel_timing = {
    .pclk_hz = BOARD_LCD_PCLK_HZ,
    .h_res = BOARD_LCD_H_RES,
    .v_res = BOARD_LCD_V_RES,
    .hsync_pulse_width = BOARD_LCD_HSYNC_PULSE_WIDTH,
    .hsync_back_porch = BOARD_LCD_HSYNC_BACK_PORCH,
    .hsync_front_porch = BOARD_LCD_HSYNC_FRONT_PORCH,
    .vsync_pulse_width = BOARD_LCD_VSYNC_PULSE_WIDTH,
    .vsync_back_porch = BOARD_LCD_VSYNC_BACK_PORCH,
    .vsync_front_porch = BOARD_LCD_VSYNC_FRONT_PORCH,
    .flags.pclk_active_neg = BOARD_LCD_PCLK_ACTIVE_NEG,
};

#if defined(CONFIG_TOUCH_GAMEPAD_BOARD_GUITION)
/*
 * Vendor-specific ST7701 initialization sequence for the Guition
 * ESP32-S3-4848S040 panel (the "type9" sequence shared by the Espressif
 * ESP32_Display_Panel test, the fasmide bootstrap, ESPHome, LovyanGFX and the
 * pljakobs driver). The esp_lcd_st7701 component default targets a different
 * panel variant (different power/gamma/GIP tables) and leaves this panel showing
 * scrambled colored lines, so the correct sequence must be supplied explicitly.
 * The sequence ends with sleep-out (0x11) and display-on (0x29) because
 * auto_del_panel_io deletes the 3-wire SPI IO right after these commands are
 * sent, leaving no way to issue display-on later.
 */
static const st7701_lcd_init_cmd_t s_st7701_init_cmds[] = {
    { 0xFF, (uint8_t []){ 0x77, 0x01, 0x00, 0x00, 0x10 }, 5, 0 },
    { 0xC0, (uint8_t []){ 0x3B, 0x00 }, 2, 0 },
    { 0xC1, (uint8_t []){ 0x0D, 0x02 }, 2, 0 },
    { 0xC2, (uint8_t []){ 0x31, 0x05 }, 2, 0 },
    { 0xCD, (uint8_t []){ 0x00 }, 1, 0 },
    { 0xB0, (uint8_t []){ 0x00, 0x11, 0x18, 0x0E, 0x11, 0x06, 0x07, 0x08,
                          0x07, 0x22, 0x04, 0x12, 0x0F, 0xAA, 0x31, 0x18 }, 16, 0 },
    { 0xB1, (uint8_t []){ 0x00, 0x11, 0x19, 0x0E, 0x12, 0x07, 0x08, 0x08,
                          0x08, 0x22, 0x04, 0x11, 0x11, 0xA9, 0x32, 0x18 }, 16, 0 },
    { 0xFF, (uint8_t []){ 0x77, 0x01, 0x00, 0x00, 0x11 }, 5, 0 },
    { 0xB0, (uint8_t []){ 0x60 }, 1, 0 },
    { 0xB1, (uint8_t []){ 0x32 }, 1, 0 },
    { 0xB2, (uint8_t []){ 0x07 }, 1, 0 },
    { 0xB3, (uint8_t []){ 0x80 }, 1, 0 },
    { 0xB5, (uint8_t []){ 0x49 }, 1, 0 },
    { 0xB7, (uint8_t []){ 0x85 }, 1, 0 },
    { 0xB8, (uint8_t []){ 0x21 }, 1, 0 },
    { 0xC1, (uint8_t []){ 0x78 }, 1, 0 },
    { 0xC2, (uint8_t []){ 0x78 }, 1, 0 },
    { 0xE0, (uint8_t []){ 0x00, 0x1B, 0x02 }, 3, 0 },
    { 0xE1, (uint8_t []){ 0x08, 0xA0, 0x00, 0x00, 0x07, 0xA0, 0x00, 0x00,
                          0x00, 0x44, 0x44 }, 11, 0 },
    { 0xE2, (uint8_t []){ 0x11, 0x11, 0x44, 0x44, 0xED, 0xA0, 0x00, 0x00,
                          0xEC, 0xA0, 0x00, 0x00 }, 12, 0 },
    { 0xE3, (uint8_t []){ 0x00, 0x00, 0x11, 0x11 }, 4, 0 },
    { 0xE4, (uint8_t []){ 0x44, 0x44 }, 2, 0 },
    { 0xE5, (uint8_t []){ 0x0A, 0xE9, 0xD8, 0xA0, 0x0C, 0xEB, 0xD8, 0xA0,
                          0x0E, 0xED, 0xD8, 0xA0, 0x10, 0xEF, 0xD8, 0xA0 }, 16, 0 },
    { 0xE6, (uint8_t []){ 0x00, 0x00, 0x11, 0x11 }, 4, 0 },
    { 0xE7, (uint8_t []){ 0x44, 0x44 }, 2, 0 },
    { 0xE8, (uint8_t []){ 0x09, 0xE8, 0xD8, 0xA0, 0x0B, 0xEA, 0xD8, 0xA0,
                          0x0D, 0xEC, 0xD8, 0xA0, 0x0F, 0xEE, 0xD8, 0xA0 }, 16, 0 },
    { 0xEB, (uint8_t []){ 0x02, 0x00, 0xE4, 0xE4, 0x88, 0x00, 0x40 }, 7, 0 },
    { 0xEC, (uint8_t []){ 0x3C, 0x00 }, 2, 0 },
    { 0xED, (uint8_t []){ 0xAB, 0x89, 0x76, 0x54, 0x02, 0xFF, 0xFF, 0xFF,
                          0xFF, 0xFF, 0xFF, 0x20, 0x45, 0x67, 0x98, 0xBA }, 16, 0 },
    { 0xFF, (uint8_t []){ 0x77, 0x01, 0x00, 0x00, 0x13 }, 5, 0 },
    { 0xE5, (uint8_t []){ 0xE4 }, 1, 0 },
    { 0xFF, (uint8_t []){ 0x77, 0x01, 0x00, 0x00, 0x00 }, 5, 0 },
    { 0x11, NULL, 0, 120 },
    { 0x29, NULL, 0, 0 },
};
#define BOARD_HAS_ST7701_INIT_CMDS 1
#elif defined(CONFIG_TOUCH_GAMEPAD_BOARD_WAVESHARE)
/*
 * Vendor-specific ST7701 initialization sequence for the Waveshare
 * ESP32-S3-Touch-LCD-4 panel, transcribed from the official Waveshare
 * esp32_s3_touch_lcd_4 BSP lcd_init_cmds[]. It differs from the Guition variant
 * in several power/GIP registers (0xC2, 0xCD, 0xB1/0xB2 in bank 1) and sets the
 * pixel format explicitly (0x3A). Sleep-out (0x11) leads and display-on (0x29)
 * trails because auto_del_panel_io deletes the 3-wire SPI IO right after these
 * commands are sent, leaving no way to issue display-on later.
 */
static const st7701_lcd_init_cmd_t s_st7701_init_cmds[] = {
    { 0x11, NULL, 0, 120 },
    { 0xFF, (uint8_t []){ 0x77, 0x01, 0x00, 0x00, 0x10 }, 5, 0 },
    { 0xC0, (uint8_t []){ 0x3B, 0x00 }, 2, 0 },
    { 0xC1, (uint8_t []){ 0x0D, 0x02 }, 2, 0 },
    { 0xC2, (uint8_t []){ 0x21, 0x08 }, 2, 0 },
    { 0xCD, (uint8_t []){ 0x08 }, 1, 0 },
    { 0xB0, (uint8_t []){ 0x00, 0x11, 0x18, 0x0E, 0x11, 0x06, 0x07, 0x08,
                          0x07, 0x22, 0x04, 0x12, 0x0F, 0xAA, 0x31, 0x18 }, 16, 0 },
    { 0xB1, (uint8_t []){ 0x00, 0x11, 0x19, 0x0E, 0x12, 0x07, 0x08, 0x08,
                          0x08, 0x22, 0x04, 0x11, 0x11, 0xA9, 0x32, 0x18 }, 16, 0 },
    { 0xFF, (uint8_t []){ 0x77, 0x01, 0x00, 0x00, 0x11 }, 5, 0 },
    { 0xB0, (uint8_t []){ 0x60 }, 1, 0 },
    { 0xB1, (uint8_t []){ 0x30 }, 1, 0 },
    { 0xB2, (uint8_t []){ 0x87 }, 1, 0 },
    { 0xB3, (uint8_t []){ 0x80 }, 1, 0 },
    { 0xB5, (uint8_t []){ 0x49 }, 1, 0 },
    { 0xB7, (uint8_t []){ 0x85 }, 1, 0 },
    { 0xB8, (uint8_t []){ 0x21 }, 1, 0 },
    { 0xC1, (uint8_t []){ 0x78 }, 1, 0 },
    { 0xC2, (uint8_t []){ 0x78 }, 1, 20 },
    { 0xE0, (uint8_t []){ 0x00, 0x1B, 0x02 }, 3, 0 },
    { 0xE1, (uint8_t []){ 0x08, 0xA0, 0x00, 0x00, 0x07, 0xA0, 0x00, 0x00,
                          0x00, 0x44, 0x44 }, 11, 0 },
    { 0xE2, (uint8_t []){ 0x11, 0x11, 0x44, 0x44, 0xED, 0xA0, 0x00, 0x00,
                          0xEC, 0xA0, 0x00, 0x00 }, 12, 0 },
    { 0xE3, (uint8_t []){ 0x00, 0x00, 0x11, 0x11 }, 4, 0 },
    { 0xE4, (uint8_t []){ 0x44, 0x44 }, 2, 0 },
    { 0xE5, (uint8_t []){ 0x0A, 0xE9, 0xD8, 0xA0, 0x0C, 0xEB, 0xD8, 0xA0,
                          0x0E, 0xED, 0xD8, 0xA0, 0x10, 0xEF, 0xD8, 0xA0 }, 16, 0 },
    { 0xE6, (uint8_t []){ 0x00, 0x00, 0x11, 0x11 }, 4, 0 },
    { 0xE7, (uint8_t []){ 0x44, 0x44 }, 2, 0 },
    { 0xE8, (uint8_t []){ 0x09, 0xE8, 0xD8, 0xA0, 0x0B, 0xEA, 0xD8, 0xA0,
                          0x0D, 0xEC, 0xD8, 0xA0, 0x0F, 0xEE, 0xD8, 0xA0 }, 16, 0 },
    { 0xEB, (uint8_t []){ 0x02, 0x00, 0xE4, 0xE4, 0x88, 0x00, 0x40 }, 7, 0 },
    { 0xEC, (uint8_t []){ 0x3C, 0x00 }, 2, 0 },
    { 0xED, (uint8_t []){ 0xAB, 0x89, 0x76, 0x54, 0x02, 0xFF, 0xFF, 0xFF,
                          0xFF, 0xFF, 0xFF, 0x20, 0x45, 0x67, 0x98, 0xBA }, 16, 0 },
    { 0xFF, (uint8_t []){ 0x77, 0x01, 0x00, 0x00, 0x00 }, 5, 0 },
    { 0x36, (uint8_t []){ 0x00 }, 1, 0 },
    { 0x3A, (uint8_t []){ 0x66 }, 1, 0 },
    { 0x21, NULL, 0, 120 },
    { 0x29, NULL, 0, 0 },
};
#define BOARD_HAS_ST7701_INIT_CMDS 1
#endif

static esp_err_t display_backlight_on(void)
{
#if BOARD_LCD_BL_GPIO >= 0
    const gpio_config_t bl_cfg = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << BOARD_LCD_BL_GPIO,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&bl_cfg), TAG, "backlight gpio");
    ESP_RETURN_ON_ERROR(gpio_set_level(BOARD_LCD_BL_GPIO, BOARD_LCD_BL_ON_LEVEL), TAG, "backlight level");
#endif
    return ESP_OK;
}

static esp_err_t display_panel_init(void)
{
    esp_lcd_panel_io_handle_t io_handle = NULL;

    const esp_lcd_panel_io_3wire_spi_config_t io_config = {
        .line_config = {
            .cs_io_type = IO_TYPE_GPIO,
            .cs_gpio_num = BOARD_LCD_SPI_CS_GPIO,
            .scl_io_type = IO_TYPE_GPIO,
            .scl_gpio_num = BOARD_LCD_SPI_SCK_GPIO,
            .sda_io_type = IO_TYPE_GPIO,
            .sda_gpio_num = BOARD_LCD_SPI_SDA_GPIO,
        },
        .expect_clk_speed = PANEL_IO_3WIRE_SPI_CLK_MAX,
        .spi_mode = 0,
        .lcd_cmd_bytes = 1,
        .lcd_param_bytes = 1,
        .flags = {
            .use_dc_bit = 1,
            .dc_zero_on_data = 0,
            .lsb_first = 0,
            .cs_high_active = 0,
            .del_keep_cs_inactive = 1,
        },
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_3wire_spi(&io_config, &io_handle), TAG, "3-wire spi io");

    const int data_gpios[] = BOARD_LCD_DATA_GPIOS;
    esp_lcd_rgb_panel_config_t rgb_config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .dma_burst_size = 64,
        .data_width = 16,
        .de_gpio_num = BOARD_LCD_DE_GPIO,
        .pclk_gpio_num = BOARD_LCD_PCLK_GPIO,
        .vsync_gpio_num = BOARD_LCD_VSYNC_GPIO,
        .hsync_gpio_num = BOARD_LCD_HSYNC_GPIO,
        .disp_gpio_num = BOARD_LCD_DISP_GPIO,
        .num_fbs = 2,
        .bounce_buffer_size_px = BOARD_LCD_H_RES * BOARD_LCD_BOUNCE_BUFFER_LINES,
        .timings = s_panel_timing,
        .flags.fb_in_psram = 1,
    };
    for (size_t i = 0; i < sizeof(data_gpios) / sizeof(data_gpios[0]); ++i) {
        rgb_config.data_gpio_nums[i] = data_gpios[i];
    }

    st7701_vendor_config_t vendor_config = {
        .rgb_config = &rgb_config,
#ifdef BOARD_HAS_ST7701_INIT_CMDS
        .init_cmds = s_st7701_init_cmds,
        .init_cmds_size = sizeof(s_st7701_init_cmds) / sizeof(s_st7701_init_cmds[0]),
#endif
        .flags = {
            .auto_del_panel_io = 1,
        },
    };

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = BOARD_LCD_BITS_PER_PIXEL,
        .vendor_config = &vendor_config,
    };

    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7701(io_handle, &panel_config, &s_panel), TAG, "st7701 panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init");
    /*
     * Do not call esp_lcd_panel_disp_on_off() here. With auto_del_panel_io set,
     * the ST7701 driver sends the init sequence (which already includes the
     * display-on command 0x29) and then deletes the 3-wire SPI IO. Because
     * disp_gpio_num is -1 the driver routes display on/off through that IO, so a
     * later disp_on_off call would fail with "Panel IO is deleted". The panel is
     * already on once the RGB signals are running.
     */
    return ESP_OK;
}

static esp_err_t display_lvgl_init(void)
{
    const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl port");

    const lvgl_port_display_cfg_t disp_cfg = {
        .panel_handle = s_panel,
        .buffer_size = BOARD_LCD_H_RES * BOARD_LCD_V_RES,
        .double_buffer = true,
        .hres = BOARD_LCD_H_RES,
        .vres = BOARD_LCD_V_RES,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = false,
            .buff_spiram = true,
            .swap_bytes = false,
            .full_refresh = true,
        },
    };
    const lvgl_port_display_rgb_cfg_t rgb_cfg = {
        .flags = {
            .bb_mode = true,
            .avoid_tearing = true,
        },
    };

    s_display = lvgl_port_add_disp_rgb(&disp_cfg, &rgb_cfg);
    return (s_display != NULL) ? ESP_OK : ESP_FAIL;
}

esp_err_t display_init(lv_display_t **out_display)
{
    ESP_RETURN_ON_ERROR(display_panel_init(), TAG, "panel");
    ESP_RETURN_ON_ERROR(display_backlight_on(), TAG, "backlight");
    ESP_RETURN_ON_ERROR(display_lvgl_init(), TAG, "lvgl");

    if (out_display != NULL) {
        *out_display = s_display;
    }

    ESP_LOGI(TAG, "Display ready (%dx%d)", BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    return ESP_OK;
}

bool display_lock(int timeout_ms)
{
    /*
     * esp_lvgl_port treats a timeout of 0 as "wait forever", so map a negative
     * request (block indefinitely) onto 0 and pass positive timeouts through.
     */
    return lvgl_port_lock(timeout_ms < 0 ? 0 : (uint32_t)timeout_ms);
}

void display_unlock(void)
{
    lvgl_port_unlock();
}
