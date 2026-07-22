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
 * The CH32V003 runs custom I2C-slave firmware that does not acknowledge the
 * zero-length transaction issued by i2c_master_probe(), so probing it reports
 * the device as absent even when it is present. Following the Waveshare BSP,
 * this code does not probe the expander: it adds the device and writes the
 * registers directly, and only warns (returning ESP_OK) if those writes fail so
 * that boards without the expander still boot.
 *
 * Because the CH32V003 keeps running across an ESP32-S3 warm reset, it can leave
 * the shared I2C bus stuck (SDA held low) from an interrupted transaction. This
 * code bit-bangs a bus-recovery sequence before installing the I2C driver and
 * retries the expander writes, mirroring board_i2c_recover() and the retry loop
 * in the Waveshare ESP32-S3-Touch-LCD-4 ioexpander example.
 *
 * ASCII only. See AGENTS.md.
 */
#include "waveshare_board.h"

#include "esp_log.h"

#include "boards.h"

static const char *TAG = "waveshare_board";

#if defined(CONFIG_TOUCH_GAMEPAD_BOARD_WAVESHARE)

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* CH32V003 I/O expander on the shared touch I2C bus. */
#define WS_EXPANDER_I2C_ADDR 0x24
#define WS_EXPANDER_I2C_HZ 400000

/* Number of times to retry the expander register writes if the first attempt
 * is NACKed while the CH32V003 or the bus settles after a warm reset. */
#define WS_EXPANDER_WRITE_RETRIES 3

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

/*
 * Recover a stuck I2C bus before installing the master driver.
 *
 * The CH32V003 expander shares the ESP32-S3 power rail but has its own reset
 * domain, so a warm reset (reflash, EN button, software restart) can restart the
 * ESP32-S3 while the CH32V003 keeps running. If the CH32V003 (or the GT911) was
 * mid-transaction it may still be holding SDA low, waiting for more clocks. The
 * new-master-bus driver then finds the bus stuck and every transaction NACKs
 * (ESP_ERR_INVALID_RESPONSE), which is exactly the observed failure. Bit-bang up
 * to nine SCL pulses to clock the slave past the byte it is stuck on, then issue
 * a STOP so the bus returns to idle. Mirrors board_i2c_recover() from the
 * Waveshare ESP32-S3-Touch-LCD-4 ioexpander example.
 */
static void ws_i2c_bus_recover(void)
{
    const gpio_num_t sda = BOARD_TOUCH_I2C_SDA_GPIO;
    const gpio_num_t scl = BOARD_TOUCH_I2C_SCL_GPIO;

    const gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << sda) | (1ULL << scl),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    esp_rom_delay_us(20);

    /* Drive SCL as an open-drain output and clock out the stuck slave. */
    gpio_set_direction(scl, GPIO_MODE_OUTPUT_OD);
    gpio_set_pull_mode(scl, GPIO_PULLUP_ONLY);
    gpio_set_level(scl, 1);
    esp_rom_delay_us(10);
    for (int i = 0; i < 9 && gpio_get_level(sda) == 0; ++i) {
        gpio_set_level(scl, 0);
        esp_rom_delay_us(10);
        gpio_set_level(scl, 1);
        esp_rom_delay_us(10);
    }

    /* Generate a STOP condition: SDA low while SCL high, then release SDA. */
    gpio_set_direction(sda, GPIO_MODE_OUTPUT_OD);
    gpio_set_pull_mode(sda, GPIO_PULLUP_ONLY);
    gpio_set_level(sda, 0);
    esp_rom_delay_us(10);
    gpio_set_level(scl, 1);
    esp_rom_delay_us(10);
    gpio_set_level(sda, 1);
    esp_rom_delay_us(10);

    /* Release both lines back to inputs for the I2C driver to take over. */
    gpio_set_direction(sda, GPIO_MODE_INPUT);
    gpio_set_direction(scl, GPIO_MODE_INPUT);
    gpio_set_pull_mode(sda, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(scl, GPIO_PULLUP_ONLY);
    vTaskDelay(pdMS_TO_TICKS(20));
}

/*
 * Configure the expander outputs and release the LCD/touch reset lines: hold the
 * resets (and enables) low, wait, then drive them high, and finally set the
 * backlight PWM. Returns the first failing transaction's error so the caller can
 * retry the whole sequence.
 */
static esp_err_t ws_expander_reset_sequence(i2c_master_dev_handle_t dev)
{
    /* Direction: every pin an output except RTC_INT. */
    esp_err_t err = ws_write_reg(dev, WS_REG_DIRECTION, (uint8_t)(0xFF & ~WS_PIN_RTC_INT));
    /* Drive LCD/touch resets (and enables) low, hold, then release high. */
    if (err == ESP_OK) {
        err = ws_write_reg(dev, WS_REG_OUTPUT, 0x00);
    }
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(200));
        err = ws_write_reg(dev, WS_REG_OUTPUT,
                           WS_PIN_SYS_EN | WS_PIN_LCD_RST | WS_PIN_TOUCH_RST);
    }
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(200));
        err = ws_write_reg(dev, WS_REG_PWM, WS_PWM_FULL_BRIGHTNESS);
    }
    return err;
}

esp_err_t waveshare_board_bringup(void)
{
    /*
     * Clear a bus left stuck by the CH32V003 (or GT911) after a warm reset
     * before the I2C driver takes over, otherwise the first transaction NACKs.
     */
    ws_i2c_bus_recover();

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

    /*
     * Do not probe the expander first. The CH32V003 custom I2C-slave firmware
     * does not answer the zero-length transaction issued by i2c_master_probe(),
     * so a probe reports it as absent even when present (this is why the panel
     * stayed dark and GT911 init failed). The Waveshare BSP skips the probe too
     * and drives the registers directly; the register writes below are the real
     * presence test.
     */
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

    /*
     * Retry the whole register sequence a few times: right after a warm reset
     * the first transaction can still NACK while the CH32V003 finishes booting
     * or the bus settles. Mirrors the retry loop in the Waveshare ioexpander
     * example.
     */
    for (int attempt = 0; attempt < WS_EXPANDER_WRITE_RETRIES; ++attempt) {
        err = ws_expander_reset_sequence(dev_handle);
        if (err == ESP_OK) {
            break;
        }
        ESP_LOGW(TAG, "expander write attempt %d failed: %s",
                 attempt + 1, esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(20 + attempt * 20));
    }

    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "CH32V003 expander (0x%02x) unreachable (%s); continuing without reset release",
                 WS_EXPANDER_I2C_ADDR, esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "CH32V003 released LCD/touch reset lines");
    }

    i2c_master_bus_rm_device(dev_handle);
    i2c_del_master_bus(bus_handle);
    return ESP_OK;
}

#else /* !CONFIG_TOUCH_GAMEPAD_BOARD_WAVESHARE */

esp_err_t waveshare_board_bringup(void)
{
    return ESP_OK;
}

#endif
