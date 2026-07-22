/*
 * Board hardware definitions for the supported ESP32-S3 touch panels.
 *
 * Both presets use a 480x480 ST7701 RGB panel and a GT911 capacitive touch
 * controller. The pin numbers below come from the vendor reference designs and
 * community projects linked in README.md. They are centralized here so a single
 * edit adapts the firmware to a wiring revision.
 *
 * ASCII only. See AGENTS.md.
 */
#pragma once

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Shared panel geometry. Both boards are square 480x480 RGB panels. */
#define BOARD_LCD_H_RES 480
#define BOARD_LCD_V_RES 480
#define BOARD_LCD_BITS_PER_PIXEL 16

#if defined(CONFIG_TOUCH_GAMEPAD_BOARD_GUITION)

#define BOARD_NAME "Guition ESP32-S3-4848S040"

/* Backlight control (active high). */
#define BOARD_LCD_BL_GPIO 38
#define BOARD_LCD_BL_ON_LEVEL 1

/* ST7701 3-wire SPI initialization bus (bit-banged by the ST7701 component). */
#define BOARD_LCD_SPI_CS_GPIO 39
#define BOARD_LCD_SPI_SCK_GPIO 48
#define BOARD_LCD_SPI_SDA_GPIO 47

/* RGB parallel control signals. */
#define BOARD_LCD_DE_GPIO 18
#define BOARD_LCD_VSYNC_GPIO 17
#define BOARD_LCD_HSYNC_GPIO 16
#define BOARD_LCD_PCLK_GPIO 21
#define BOARD_LCD_DISP_GPIO (-1)

/*
 * RGB565 data lines in data_gpio_nums[] order: B0..B4, G0..G5, R0..R4.
 * These match the Guition ESP32-S3-4848S040 PCB routing confirmed across the
 * Espressif ESP32_Display_Panel test, the fasmide bootstrap, ESPHome, LovyanGFX
 * and the pljakobs driver (B0 = GPIO4, R0 = GPIO11).
 */
#define BOARD_LCD_DATA_GPIOS { 4, 5, 6, 7, 15, 8, 20, 3, 46, 9, 10, 11, 12, 13, 14, 0 }

/*
 * RGB panel timing. The ST7701 latches pixel data on the rising edge of PCLK,
 * so pclk_active_neg must be 0; driving it as 1 samples every pixel half a clock
 * off and produces scrambled colored lines. 12 MHz with these porches matches
 * the known-good ESPHome/native-IDF configurations for this panel.
 */
#define BOARD_LCD_PCLK_HZ (12 * 1000 * 1000)
#define BOARD_LCD_HSYNC_PULSE_WIDTH 8
#define BOARD_LCD_HSYNC_BACK_PORCH 20
#define BOARD_LCD_HSYNC_FRONT_PORCH 10
#define BOARD_LCD_VSYNC_PULSE_WIDTH 8
#define BOARD_LCD_VSYNC_BACK_PORCH 10
#define BOARD_LCD_VSYNC_FRONT_PORCH 10
#define BOARD_LCD_PCLK_ACTIVE_NEG 0

/* GT911 touch controller on I2C. */
#define BOARD_TOUCH_I2C_SDA_GPIO 19
#define BOARD_TOUCH_I2C_SCL_GPIO 45
#define BOARD_TOUCH_INT_GPIO (-1)
#define BOARD_TOUCH_RST_GPIO (-1)

#elif defined(CONFIG_TOUCH_GAMEPAD_BOARD_WAVESHARE)

#define BOARD_NAME "Waveshare ESP32-S3-Touch-LCD-4"

/*
 * The Waveshare ESP32-S3-Touch-LCD-4 routes the backlight, LCD reset and touch
 * reset through an on-board CH32V003 I/O expander (I2C address 0x24) rather than
 * through dedicated ESP32-S3 GPIOs, so those lines are marked unused (-1) here
 * and released by waveshare_board_bringup() before the display and touch come
 * up. All pins below match the official Waveshare esp32_s3_touch_lcd_4 BSP.
 */
#define BOARD_LCD_BL_GPIO (-1)
#define BOARD_LCD_BL_ON_LEVEL 1

/* ST7701 3-wire SPI initialization bus (bit-banged by the ST7701 component). */
#define BOARD_LCD_SPI_CS_GPIO 42
#define BOARD_LCD_SPI_SCK_GPIO 2
#define BOARD_LCD_SPI_SDA_GPIO 1

/* RGB parallel control signals. */
#define BOARD_LCD_DE_GPIO 40
#define BOARD_LCD_VSYNC_GPIO 39
#define BOARD_LCD_HSYNC_GPIO 38
#define BOARD_LCD_PCLK_GPIO 41
#define BOARD_LCD_DISP_GPIO (-1)

/* RGB565 data lines in data_gpio_nums[] order: B0..B4, G0..G5, R0..R4. */
#define BOARD_LCD_DATA_GPIOS { 5, 45, 48, 47, 21, 14, 13, 12, 11, 10, 9, 46, 3, 8, 18, 17 }

/*
 * RGB panel timing from the Waveshare BSP (ST7701 480x480 60Hz preset). As on
 * the Guition board the ST7701 latches on the rising PCLK edge, so
 * pclk_active_neg must be 0.
 */
#define BOARD_LCD_PCLK_HZ (16 * 1000 * 1000)
#define BOARD_LCD_HSYNC_PULSE_WIDTH 10
#define BOARD_LCD_HSYNC_BACK_PORCH 10
#define BOARD_LCD_HSYNC_FRONT_PORCH 20
#define BOARD_LCD_VSYNC_PULSE_WIDTH 10
#define BOARD_LCD_VSYNC_BACK_PORCH 10
#define BOARD_LCD_VSYNC_FRONT_PORCH 10
#define BOARD_LCD_PCLK_ACTIVE_NEG 0

/* GT911 touch controller on I2C (reset released via the CH32V003 expander). */
#define BOARD_TOUCH_I2C_SDA_GPIO 15
#define BOARD_TOUCH_I2C_SCL_GPIO 7
#define BOARD_TOUCH_INT_GPIO (-1)
#define BOARD_TOUCH_RST_GPIO (-1)

#else
#error "No touch-gamepad board preset selected in Kconfig"
#endif

#ifdef __cplusplus
}
#endif
