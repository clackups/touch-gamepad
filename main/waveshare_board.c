/*
 * Waveshare ESP32-S3-Touch-LCD-4 board bring-up implementation.
 *
 * Mirrors the reset sequence used by the official Waveshare esp32_s3_touch_lcd_4
 * BSP: configure the CH32V003 expander outputs, pulse the LCD and touch reset
 * lines low then high, and drive the backlight PWM to full brightness. The
 * expander shares the GT911 I2C bus (SDA/SCL from boards.h), so this creates a
 * short-lived master bus, performs the raw register writes, then releases the
 * bus for the display and touch drivers. The expander latches its output state,
 * so the reset lines stay released after the bus is torn down.
 *
 * ASCII only. See AGENTS.md.
 */
#include "waveshare_board.h"

#include "esp_log.h"

#include "boards.h"

static const char *TAG = "waveshare_board";

#if defined(CONFIG_TOUCH_GAMEPAD_BOARD_WAVESHARE)

#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* CH32V003 I/O expander on the shared touch I2C bus. */
#define WS_EXPANDER_I2C_ADDR 0x24
#define WS_EXPANDER_I2C_HZ 400000

/* CH32V003 register map (from the Waveshare custom_io_expander_ch32v003 driver). */
#define WS_REG_DIRECTION 0x02
#define WS_REG_OUTPUT 0x03
#define WS_REG_PWM 0x05

/*
 * Expander pin bit masks (IO_EXPANDER_PIN_NUM_n == 1 << n). The direction
 * register uses 1 = output (dir_out_bit_zero = 0); all pins default to output
 * except RTC_INT, which is left as an input.
 */
#define WS_PIN_TOUCH_RST (1 << 1)
#define WS_PIN_LCD_RST (1 << 3)
#define WS_PIN_SYS_EN (1 << 5)
#define WS_PIN_BEE_EN (1 << 6)
#define WS_PIN_RTC_INT (1 << 7)

/* Full brightness maps to PWM value 0 in the Waveshare backlight driver. */
#define WS_PWM_FULL_BRIGHTNESS 0x00

static esp_err_t ws_write_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t value)
{
    const uint8_t buf[2] = { reg, value };
    return i2c_master_transmit(dev, buf, sizeof(buf), 1000);
}

esp_err_t waveshare_board_bringup(void)
{
    const i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = BOARD_TOUCH_I2C_SDA_GPIO,
        .scl_io_num = BOARD_TOUCH_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus_handle = NULL;
    esp_err_t err = i2c_new_master_bus(&bus_config, &bus_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c bus create failed: %s", esp_err_to_name(err));
        return err;
    }

    if (i2c_master_probe(bus_handle, WS_EXPANDER_I2C_ADDR, 1000) != ESP_OK) {
        ESP_LOGW(TAG, "CH32V003 expander (0x%02x) not found; skipping reset release",
                 WS_EXPANDER_I2C_ADDR);
        i2c_del_master_bus(bus_handle);
        return ESP_OK;
    }

    const i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = WS_EXPANDER_I2C_ADDR,
        .scl_speed_hz = WS_EXPANDER_I2C_HZ,
    };
    i2c_master_dev_handle_t dev_handle = NULL;
    err = i2c_master_bus_add_device(bus_handle, &dev_config, &dev_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "expander add device failed: %s", esp_err_to_name(err));
        i2c_del_master_bus(bus_handle);
        return err;
    }

    /* Direction: every pin an output except RTC_INT. */
    err = ws_write_reg(dev_handle, WS_REG_DIRECTION,
                       (uint8_t)(0xFF & ~WS_PIN_RTC_INT));
    /* Drive LCD/touch resets (and enables) low, hold, then release high. */
    if (err == ESP_OK) {
        err = ws_write_reg(dev_handle, WS_REG_OUTPUT, 0x00);
    }
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(200));
        err = ws_write_reg(dev_handle, WS_REG_OUTPUT,
                           WS_PIN_SYS_EN | WS_PIN_LCD_RST | WS_PIN_TOUCH_RST);
    }
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(200));
        err = ws_write_reg(dev_handle, WS_REG_PWM, WS_PWM_FULL_BRIGHTNESS);
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "expander reset sequence failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "CH32V003 released LCD/touch reset lines");
    }

    i2c_master_bus_rm_device(dev_handle);
    i2c_del_master_bus(bus_handle);
    return err;
}

#else /* !CONFIG_TOUCH_GAMEPAD_BOARD_WAVESHARE */

esp_err_t waveshare_board_bringup(void)
{
    return ESP_OK;
}

#endif
