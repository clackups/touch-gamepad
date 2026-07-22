#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TOUCH_GAMEPAD_TAP_BINDING_COUNT 8
#define TOUCH_GAMEPAD_BUTTON_NAME_LENGTH 12

typedef enum {
    TOUCH_GAMEPAD_BOARD_GUITION = 0,
    TOUCH_GAMEPAD_BOARD_WAVESHARE,
} touch_gamepad_board_t;

typedef enum {
    TOUCH_GAMEPAD_TRANSPORT_BLE = 0,
    TOUCH_GAMEPAD_TRANSPORT_USB,
} touch_gamepad_transport_t;

typedef enum {
    TOUCH_GAMEPAD_THEME_BLUE_ON_BLACK = 0,
    TOUCH_GAMEPAD_THEME_GREEN_ON_BLACK,
} touch_gamepad_theme_t;

typedef enum {
    TOUCH_GAMEPAD_MENU_ITEM_TRANSPORT = 0,
    TOUCH_GAMEPAD_MENU_ITEM_REPAIR,
    TOUCH_GAMEPAD_MENU_ITEM_MAPPING,
    TOUCH_GAMEPAD_MENU_ITEM_THEME,
    TOUCH_GAMEPAD_MENU_ITEM_COUNT,
} touch_gamepad_menu_item_t;

typedef enum {
    TOUCH_GAMEPAD_GESTURE_NONE = 0,
    TOUCH_GAMEPAD_GESTURE_TAP,
    TOUCH_GAMEPAD_GESTURE_SLIDE,
    TOUCH_GAMEPAD_GESTURE_OPEN_MENU,
} touch_gamepad_gesture_type_t;

typedef struct {
    int16_t x;
    int16_t y;
} touch_gamepad_point_t;

typedef struct {
    uint8_t finger_count;
    touch_gamepad_point_t start[2];
    touch_gamepad_point_t end[2];
} touch_gamepad_touch_frame_t;

typedef struct {
    const char *board_name;
    bool supports_usb;
    uint16_t screen_width;
    uint16_t screen_height;
    touch_gamepad_transport_t default_transport;
} touch_gamepad_board_preset_t;

typedef struct {
    int8_t axis_x;
    int8_t axis_y;
} touch_gamepad_axis_binding_t;

typedef struct {
    touch_gamepad_transport_t transport_mode;
    touch_gamepad_theme_t theme;
    uint8_t tap_buttons[TOUCH_GAMEPAD_TAP_BINDING_COUNT];
    touch_gamepad_axis_binding_t one_finger_slide;
    touch_gamepad_axis_binding_t two_finger_slide;
    bool repair_requested;
} touch_gamepad_config_t;

typedef struct {
    bool open;
    touch_gamepad_menu_item_t current_item;
} touch_gamepad_menu_state_t;

typedef struct {
    uint8_t expected_corner_index;
} touch_gamepad_gesture_state_t;

typedef struct {
    touch_gamepad_gesture_type_t type;
    uint8_t finger_count;
    uint8_t tap_zone;
    int16_t delta_x;
    int16_t delta_y;
} touch_gamepad_gesture_event_t;

typedef struct {
    uint8_t button;
    char label[TOUCH_GAMEPAD_BUTTON_NAME_LENGTH];
} touch_gamepad_tap_binding_t;

typedef struct {
    touch_gamepad_tap_binding_t tap_bindings[TOUCH_GAMEPAD_TAP_BINDING_COUNT];
    touch_gamepad_axis_binding_t one_finger_slide;
    touch_gamepad_axis_binding_t two_finger_slide;
} touch_gamepad_mapping_snapshot_t;

const touch_gamepad_board_preset_t *touch_gamepad_get_board_preset(void);
const char *touch_gamepad_transport_name(touch_gamepad_transport_t transport_mode);
const char *touch_gamepad_theme_name(touch_gamepad_theme_t theme);
const char *touch_gamepad_menu_item_name(touch_gamepad_menu_item_t item);

/* Number of assignable gamepad buttons and the label for a given button. */
uint8_t touch_gamepad_button_count(void);
const char *touch_gamepad_button_label(uint8_t button);

void touch_gamepad_config_set_defaults(touch_gamepad_config_t *config);
esp_err_t touch_gamepad_config_load(touch_gamepad_config_t *config);
esp_err_t touch_gamepad_config_save(const touch_gamepad_config_t *config);

bool touch_gamepad_detect_gesture(touch_gamepad_gesture_state_t *state,
                                  const touch_gamepad_touch_frame_t *frame,
                                  touch_gamepad_gesture_event_t *event);

void touch_gamepad_menu_init(touch_gamepad_menu_state_t *menu_state);
void touch_gamepad_menu_open(touch_gamepad_menu_state_t *menu_state);
void touch_gamepad_menu_next(touch_gamepad_menu_state_t *menu_state);
esp_err_t touch_gamepad_menu_activate(touch_gamepad_config_t *config,
                                      touch_gamepad_menu_state_t *menu_state,
                                      touch_gamepad_mapping_snapshot_t *snapshot);

void touch_gamepad_build_mapping_snapshot(const touch_gamepad_config_t *config,
                                          touch_gamepad_mapping_snapshot_t *snapshot);
esp_err_t touch_gamepad_set_tap_binding(touch_gamepad_config_t *config,
                                        uint8_t binding_index,
                                        uint8_t button);
esp_err_t touch_gamepad_set_slide_binding(touch_gamepad_config_t *config,
                                          uint8_t finger_count,
                                          int8_t axis_x,
                                          int8_t axis_y);
void touch_gamepad_log_configuration(const touch_gamepad_config_t *config);

#ifdef __cplusplus
}
#endif
