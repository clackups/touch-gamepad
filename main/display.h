/*
 * Display bring-up: ST7701 480x480 RGB panel plus the LVGL port.
 *
 * ASCII only. See AGENTS.md.
 */
#pragma once

#include "esp_err.h"
#include "esp_lcd_types.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Initialize the RGB panel, its backlight and the LVGL port. On success the
 * caller receives the active LVGL display handle. LVGL calls must be guarded by
 * display_lock()/display_unlock() when made outside the LVGL task.
 */
esp_err_t display_init(lv_display_t **out_display);

/* Grab the LVGL port mutex. timeout_ms < 0 waits forever. */
bool display_lock(int timeout_ms);

/* Release the LVGL port mutex. */
void display_unlock(void);

#ifdef __cplusplus
}
#endif
