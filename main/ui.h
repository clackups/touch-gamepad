/*
 * LVGL user interface: tap zones, slide area, status bar and the configuration
 * menu overlay. Rendering respects the selected color theme.
 *
 * ASCII only. See AGENTS.md.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "touch_gamepad.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Build the main screen. Must be called after display_init(). */
esp_err_t ui_init(const touch_gamepad_config_t *config,
                  const touch_gamepad_board_preset_t *preset);

/* Apply a color theme to every widget. */
void ui_set_theme(touch_gamepad_theme_t theme);

/* Update the status line (active transport and host connection state). */
void ui_set_status(touch_gamepad_transport_t transport, bool connected);

/* Briefly highlight a tapped upper-half zone (0..3) for visual feedback. */
void ui_flash_tap(uint8_t zone, uint8_t finger_count);

/* Show the joystick indicator for a slide gesture. */
void ui_show_slide(uint8_t finger_count, int16_t delta_x, int16_t delta_y);

/* Render the configuration menu overlay for the given menu/config state. Works
 * for both the main screen and the mapping sub-screen (chosen by menu->screen). */
void ui_show_menu(const touch_gamepad_menu_state_t *menu,
                  const touch_gamepad_config_t *config,
                  const touch_gamepad_board_preset_t *preset);

/*
 * Map a raw touch point (screen pixels) to a menu row. Returns true and fills
 * out_row with the logical row index and out_direction with -1 (tap on the left
 * half of the row) or +1 (right half) when the point lands on a visible row.
 */
bool ui_menu_hit_test(int16_t x, int16_t y, uint8_t *out_row, int8_t *out_direction);

/* Scroll the menu list vertically by delta_y pixels (for tall menus). */
void ui_menu_scroll(int16_t delta_y);

/* Hide the configuration menu overlay and return to the gameplay view. */
void ui_hide_menu(void);

#ifdef __cplusplus
}
#endif
