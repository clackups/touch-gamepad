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
        .bits_per_pixel = BOARD_LCD_BITS_PER_PIXEL,
        .de_gpio_num = BOARD_LCD_DE_GPIO,
        .pclk_gpio_num = BOARD_LCD_PCLK_GPIO,
        .vsync_gpio_num = BOARD_LCD_VSYNC_GPIO,
        .hsync_gpio_num = BOARD_LCD_HSYNC_GPIO,
        .disp_gpio_num = BOARD_LCD_DISP_GPIO,
        .num_fbs = 2,
        .timings = s_panel_timing,
        .flags.fb_in_psram = 1,
    };
    for (size_t i = 0; i < sizeof(data_gpios) / sizeof(data_gpios[0]); ++i) {
        rgb_config.data_gpio_nums[i] = data_gpios[i];
    }

    st7701_vendor_config_t vendor_config = {
        .rgb_config = &rgb_config,
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
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "panel on");
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
            .bb_mode = false,
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
    return lvgl_port_lock(timeout_ms < 0 ? 0 : (uint32_t)timeout_ms);
}

void display_unlock(void)
{
    lvgl_port_unlock();
}
