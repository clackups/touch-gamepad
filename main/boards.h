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

/* RGB565 data lines: 5 blue, 6 green, 5 red. */
#define BOARD_LCD_DATA_GPIOS { 11, 12, 13, 14, 0, 8, 20, 3, 46, 9, 10, 4, 5, 6, 7, 15 }

/* RGB panel timing. */
#define BOARD_LCD_PCLK_HZ (14 * 1000 * 1000)
#define BOARD_LCD_HSYNC_PULSE_WIDTH 8
#define BOARD_LCD_HSYNC_BACK_PORCH 10
#define BOARD_LCD_HSYNC_FRONT_PORCH 10
#define BOARD_LCD_VSYNC_PULSE_WIDTH 8
#define BOARD_LCD_VSYNC_BACK_PORCH 10
#define BOARD_LCD_VSYNC_FRONT_PORCH 10
#define BOARD_LCD_PCLK_ACTIVE_NEG 1

/* GT911 touch controller on I2C. */
#define BOARD_TOUCH_I2C_SDA_GPIO 19
#define BOARD_TOUCH_I2C_SCL_GPIO 45
#define BOARD_TOUCH_INT_GPIO (-1)
#define BOARD_TOUCH_RST_GPIO (-1)

#elif defined(CONFIG_TOUCH_GAMEPAD_BOARD_WAVESHARE)

#define BOARD_NAME "Waveshare ESP32-S3-Touch-LCD-4"

/*
 * NOTE: The Waveshare ESP32-S3-Touch-LCD-4 drives the backlight, LCD reset and
 * touch reset lines through an on-board CH422G I/O expander rather than through
 * dedicated ESP32-S3 GPIOs, and its exact RGB data mapping must be confirmed
 * against the Waveshare schematic. The values below mirror the common ST7701
 * 480x480 reference layout as a provisional default; verify them on hardware
 * before a production build. Reset lines routed through the expander are marked
 * unused (-1) so the drivers rely on the power-on reset.
 */
#define BOARD_LCD_BL_GPIO (-1)
#define BOARD_LCD_BL_ON_LEVEL 1

#define BOARD_LCD_SPI_CS_GPIO 39
#define BOARD_LCD_SPI_SCK_GPIO 48
#define BOARD_LCD_SPI_SDA_GPIO 47

#define BOARD_LCD_DE_GPIO 18
#define BOARD_LCD_VSYNC_GPIO 17
#define BOARD_LCD_HSYNC_GPIO 16
#define BOARD_LCD_PCLK_GPIO 21
#define BOARD_LCD_DISP_GPIO (-1)

#define BOARD_LCD_DATA_GPIOS { 11, 12, 13, 14, 0, 8, 20, 3, 46, 9, 10, 4, 5, 6, 7, 15 }

#define BOARD_LCD_PCLK_HZ (16 * 1000 * 1000)
#define BOARD_LCD_HSYNC_PULSE_WIDTH 8
#define BOARD_LCD_HSYNC_BACK_PORCH 10
#define BOARD_LCD_HSYNC_FRONT_PORCH 10
#define BOARD_LCD_VSYNC_PULSE_WIDTH 8
#define BOARD_LCD_VSYNC_BACK_PORCH 10
#define BOARD_LCD_VSYNC_FRONT_PORCH 10
#define BOARD_LCD_PCLK_ACTIVE_NEG 1

#define BOARD_TOUCH_I2C_SDA_GPIO 19
#define BOARD_TOUCH_I2C_SCL_GPIO 45
#define BOARD_TOUCH_INT_GPIO (-1)
#define BOARD_TOUCH_RST_GPIO (-1)

#else
#error "No touch-gamepad board preset selected in Kconfig"
#endif

#ifdef __cplusplus
}
#endif
