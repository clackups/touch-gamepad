/*
 * Waveshare ESP32-S3-Touch-LCD-4 board bring-up implementation.
 *
 * Mirrors the reset sequence used by the official Waveshare esp32_s3_touch_lcd_4
 * BSP: configure the CH32V003 expander outputs, pulse the LCD and touch reset
 * lines low then high, and drive the backlight PWM to full brightness. The
 * expander shares the GT911 I2C bus (SDA/SCL from boards.h). Like the Waveshare
 * BSP and the vendor ioexpander example, this creates a single master bus once
 * and keeps it alive: the GT911 touch driver reuses the same handle through
 * waveshare_board_get_i2c_bus() instead of opening a second bus on the same I2C
 * port. Sharing one bus keeps the recovered bus state and avoids tearing the I2C
 * peripheral down and back up between the expander and the touch controller.
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
 * code bit-bangs a bus-recovery sequence before installing the I2C driver, lets
 * the bus settle, then retries the expander writes on the same persistent bus,
 * mirroring board_i2c_recover() plus the retry loop in the Waveshare
 * ESP32-S3-Touch-LCD-4 ioexpander example. If the expander still never answers,
 * a diagnostic address scan logs whatever does respond on the bus.
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
/*
 * Access the expander at 100 kHz rather than the vendor's 400 kHz. The expander
 * is only touched once at boot, so the lower clock costs nothing but gives the
 * CH32V003's software I2C slave and any marginal bus rise time extra margin to
 * ACK its address on the very first transaction after a reset.
 */
#define WS_EXPANDER_I2C_HZ 100000
#define WS_I2C_PORT I2C_NUM_0

/* Number of times to retry the expander register writes on the shared bus if the
 * first attempt is NACKed while the CH32V003 or the bus settles after a reset. */
#define WS_EXPANDER_WRITE_RETRIES 3

/*
 * Settle delay after the bus is created and before the first expander write. The
 * vendor's working path reaches the expander only after lvgl_port_init() runs,
 * so it has an implicit gap between bus creation and the first transaction; a
 * freshly recovered bus and a just-reset CH32V003 both benefit from the pause.
 */
#define WS_EXPANDER_SETTLE_MS 150

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

/*
 * Persistent shared I2C bus. Created once during bring-up and kept alive for the
 * lifetime of the firmware so the GT911 touch driver can reuse it (see
 * waveshare_board_get_i2c_bus()). NULL until a bus is successfully created.
 */
static i2c_master_bus_handle_t s_i2c_bus = NULL;

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

/*
 * (Re)create the shared I2C master bus after running the bit-bang recovery.
 * Stores the handle in s_i2c_bus on success; leaves it NULL on failure.
 */
static esp_err_t ws_create_bus(void)
{
    /*
     * Clear a bus left stuck by the CH32V003 (or GT911) after a warm reset
     * before the I2C driver takes over, otherwise the first transaction NACKs.
     */
    ws_i2c_bus_recover();

    /*
     * Match the vendor BSP's known-good bus config exactly: only the clock
     * source, the two pins and the port. Do not set enable_internal_pullup or a
     * glitch_ignore_cnt override here. The board carries external pull-ups (the
     * bit-bang recovery above relies on them to read SDA high), and keeping the
     * config identical to the working Waveshare demo removes every code-level
     * difference from the proven path.
     */
    const i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .sda_io_num = BOARD_TOUCH_I2C_SDA_GPIO,
        .scl_io_num = BOARD_TOUCH_I2C_SCL_GPIO,
        .i2c_port = WS_I2C_PORT,
    };
    esp_err_t err = i2c_new_master_bus(&bus_config, &s_i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c bus create failed: %s", esp_err_to_name(err));
        s_i2c_bus = NULL;
    }
    return err;
}

/*
 * Add the expander device to the current bus, run the reset sequence, and remove
 * the temporary device handle again (the expander latches its outputs, so the
 * device only needs to exist for the writes). Returns the transaction error.
 */
static esp_err_t ws_try_expander(void)
{
    /*
     * Do not probe the expander first. The CH32V003 custom I2C-slave firmware
     * does not answer the zero-length transaction issued by i2c_master_probe(),
     * so a probe reports it as absent even when present (this is why the panel
     * stayed dark and GT911 init failed). The Waveshare BSP skips the probe too
     * and drives the registers directly; the register writes are the real
     * presence test.
     */
    const i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = WS_EXPANDER_I2C_ADDR,
        .scl_speed_hz = WS_EXPANDER_I2C_HZ,
    };
    i2c_master_dev_handle_t dev_handle = NULL;
    esp_err_t err = i2c_master_bus_add_device(s_i2c_bus, &dev_config, &dev_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "expander add device failed: %s", esp_err_to_name(err));
        return err;
    }

    err = ws_expander_reset_sequence(dev_handle);
    i2c_master_bus_rm_device(dev_handle);
    return err;
}

/*
 * Diagnostic: scan the 7-bit address space with a harmless single-byte write and
 * log every address that ACKs. Run only when the expander stays unreachable, to
 * tell whether any device answers on the bus at all (an empty scan points at
 * wiring, pull-ups or a wedged CH32V003 that only a power cycle clears; an ACK at
 * an unexpected address points at a different expander address). The GT911 is
 * held in reset here, so normally only the expander can answer. Writing one byte
 * (a register pointer) is non-destructive on this bus.
 */
static void ws_i2c_scan(void)
{
    if (s_i2c_bus == NULL) {
        return;
    }

    ESP_LOGW(TAG, "Scanning I2C bus (SDA=%d SCL=%d) for any responding device...",
             (int)BOARD_TOUCH_I2C_SDA_GPIO, (int)BOARD_TOUCH_I2C_SCL_GPIO);

    int found = 0;
    for (uint8_t addr = 0x08; addr <= 0x77; ++addr) {
        const i2c_device_config_t dev_config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addr,
            .scl_speed_hz = WS_EXPANDER_I2C_HZ,
        };
        i2c_master_dev_handle_t dev_handle = NULL;
        if (i2c_master_bus_add_device(s_i2c_bus, &dev_config, &dev_handle) != ESP_OK) {
            continue;
        }
        const uint8_t reg = 0x04; /* CH32V003 INPUT_REG; harmless pointer set. */
        if (i2c_master_transmit(dev_handle, &reg, sizeof(reg), 100) == ESP_OK) {
            ESP_LOGW(TAG, "  device ACK at 0x%02x", addr);
            ++found;
        }
        i2c_master_bus_rm_device(dev_handle);
    }

    if (found == 0) {
        ESP_LOGW(TAG, "  no devices responded: check the CH32V003 power/wiring or power-cycle the board");
    }
}

esp_err_t waveshare_board_bringup(void)
{
    /*
     * Recover the bus once and create the persistent shared bus once, then retry
     * only the expander writes on that same bus. This mirrors the Waveshare
     * ioexpander example, which calls board_i2c_recover() + i2c_new_master_bus()
     * a single time and retries custom_io_expander_new_i2c_ch32v003() on the same
     * bus. The bus is kept alive so the GT911 touch driver can reuse it through
     * waveshare_board_get_i2c_bus().
     */
    if (ws_create_bus() != ESP_OK) {
        ESP_LOGW(TAG,
                 "CH32V003 expander (0x%02x) unreachable (no I2C bus); continuing without reset release",
                 WS_EXPANDER_I2C_ADDR);
        return ESP_OK;
    }

    /* Let the recovered bus and a just-reset CH32V003 settle before the first write. */
    vTaskDelay(pdMS_TO_TICKS(WS_EXPANDER_SETTLE_MS));

    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < WS_EXPANDER_WRITE_RETRIES; ++attempt) {
        err = ws_try_expander();
        if (err == ESP_OK) {
            break;
        }
        ESP_LOGW(TAG, "expander bring-up attempt %d failed: %s",
                 attempt + 1, esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(20 + attempt * 20));
    }

    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "CH32V003 expander (0x%02x) unreachable (%s); continuing without reset release",
                 WS_EXPANDER_I2C_ADDR, esp_err_to_name(err));
        ws_i2c_scan();
    } else {
        ESP_LOGI(TAG, "CH32V003 released LCD/touch reset lines");
    }

    return ESP_OK;
}

i2c_master_bus_handle_t waveshare_board_get_i2c_bus(void)
{
    return s_i2c_bus;
}

#else /* !CONFIG_TOUCH_GAMEPAD_BOARD_WAVESHARE */

esp_err_t waveshare_board_bringup(void)
{
    return ESP_OK;
}

i2c_master_bus_handle_t waveshare_board_get_i2c_bus(void)
{
    return NULL;
}

#endif
