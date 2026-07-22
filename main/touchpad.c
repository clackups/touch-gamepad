/*
 * GT911 capacitive touch controller driver wrapper.
 *
 * Uses the new I2C master driver plus the esp_lcd_touch_gt911 component to read
 * up to two simultaneous touch points, which the gesture engine turns into taps
 * and slides.
 *
 * ASCII only. See AGENTS.md.
 */
#include "touchpad.h"

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"

#include "boards.h"
#include "waveshare_board.h"

static const char *TAG = "touchpad";

#define TOUCHPAD_I2C_PORT I2C_NUM_0
#define TOUCHPAD_I2C_HZ 400000

static esp_lcd_touch_handle_t s_touch = NULL;

esp_err_t touchpad_init(void)
{
    /*
     * Reuse the shared I2C bus the Waveshare bring-up already created for the
     * CH32V003 expander when it is available. That bus lives on the same I2C
     * port and pins as the GT911, so opening a second bus here would fail; it
     * also carries the bit-bang recovery the expander bring-up performed. On
     * boards without an expander (Guition) there is no shared bus, so create a
     * dedicated one here.
     */
    i2c_master_bus_handle_t bus_handle = waveshare_board_get_i2c_bus();
    if (bus_handle == NULL) {
        const i2c_master_bus_config_t bus_config = {
            .i2c_port = TOUCHPAD_I2C_PORT,
            .sda_io_num = BOARD_TOUCH_I2C_SDA_GPIO,
            .scl_io_num = BOARD_TOUCH_I2C_SCL_GPIO,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &bus_handle), TAG, "i2c bus");
    }

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    /*
     * The GT911 answers on its primary address (0x5D) or its backup address
     * (0x14) depending on the INT/RST strapping at power-up. Probe both so the
     * driver binds to whichever the board exposes.
     */
    if (i2c_master_probe(bus_handle, io_config.dev_addr, 1000) != ESP_OK) {
        const uint32_t backup_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP;
        if (i2c_master_probe(bus_handle, backup_addr, 1000) == ESP_OK) {
            io_config.dev_addr = backup_addr;
        }
    }
    ESP_LOGI(TAG, "GT911 I2C address 0x%02x", (unsigned)io_config.dev_addr);
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(bus_handle, &io_config, &io_handle), TAG, "touch io");

    const esp_lcd_touch_config_t touch_config = {
        .x_max = BOARD_LCD_H_RES,
        .y_max = BOARD_LCD_V_RES,
        .rst_gpio_num = BOARD_TOUCH_RST_GPIO,
        .int_gpio_num = BOARD_TOUCH_INT_GPIO,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_gt911(io_handle, &touch_config, &s_touch), TAG, "gt911");

    ESP_LOGI(TAG, "GT911 touch ready");
    return ESP_OK;
}

uint8_t touchpad_read(touch_gamepad_point_t points[TOUCHPAD_MAX_POINTS])
{
    if (s_touch == NULL) {
        return 0;
    }

    uint16_t raw_x[TOUCHPAD_MAX_POINTS] = { 0 };
    uint16_t raw_y[TOUCHPAD_MAX_POINTS] = { 0 };
    uint16_t strength[TOUCHPAD_MAX_POINTS] = { 0 };
    uint8_t count = 0;

    esp_lcd_touch_read_data(s_touch);
    if (!esp_lcd_touch_get_coordinates(s_touch, raw_x, raw_y, strength, &count, TOUCHPAD_MAX_POINTS)) {
        return 0;
    }

    if (count > TOUCHPAD_MAX_POINTS) {
        count = TOUCHPAD_MAX_POINTS;
    }

    for (uint8_t i = 0; i < count; ++i) {
        points[i].x = (int16_t)raw_x[i];
        points[i].y = (int16_t)raw_y[i];
    }

    return count;
}
