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

/* Highlight (active=true) or clear (active=false) an upper-half tap zone (0..3)
 * so the button reflects the current press state of the finger on it. */
void ui_set_zone_active(uint8_t zone, bool active);

/*
 * Show the lower-half joystick: draw the vector from the fixed central point to
 * the current touch point. delta_x/delta_y are the touch offset from the center
 * in screen pixels. Positive x is right, positive y is down.
 */
void ui_show_joystick(int16_t delta_x, int16_t delta_y);

/* Return the joystick indicator to the central point (no active touch). */
void ui_hide_joystick(void);

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
