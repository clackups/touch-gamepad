/*
 * Waveshare ESP32-S3-Touch-LCD-4 board bring-up.
 *
 * The Waveshare board holds the ST7701 reset, the GT911 touch reset and the
 * backlight behind an on-board CH32V003 I2C I/O expander (address 0x24). Until
 * the expander releases those lines the panel stays in reset and the GT911 does
 * not answer on the I2C bus, so this must run before display and touch init.
 *
 * ASCII only. See AGENTS.md.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Release the ST7701 and GT911 reset lines and enable the backlight through the
 * CH32V003 I/O expander. On non-Waveshare boards this is a no-op that returns
 * ESP_OK. If the expander cannot be reached the function logs a warning and
 * returns ESP_OK so boards without it (or older hardware revisions) still boot.
 */
esp_err_t waveshare_board_bringup(void);

#ifdef __cplusplus
}
#endif
