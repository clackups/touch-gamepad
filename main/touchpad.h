/*
 * GT911 capacitive touch input.
 *
 * ASCII only. See AGENTS.md.
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

#include "touch_gamepad.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TOUCHPAD_MAX_POINTS 2

/* Initialize the I2C bus and the GT911 controller. */
esp_err_t touchpad_init(void);

/*
 * Read the currently pressed points. Returns the number of active touch points
 * (0..TOUCHPAD_MAX_POINTS) and fills points[] with their coordinates.
 */
uint8_t touchpad_read(touch_gamepad_point_t points[TOUCHPAD_MAX_POINTS]);

#ifdef __cplusplus
}
#endif
