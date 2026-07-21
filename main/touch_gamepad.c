#include "touch_gamepad.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#define TOUCH_GAMEPAD_NVS_NAMESPACE "touchgp"
#define TOUCH_GAMEPAD_NVS_KEY "config"
#define TOUCH_GAMEPAD_CONFIG_VERSION 1U
#define TOUCH_GAMEPAD_TAP_DISTANCE_LIMIT 32
#define TOUCH_GAMEPAD_SLIDE_DISTANCE_MINIMUM 24
#define TOUCH_GAMEPAD_TAP_DISTANCE_LIMIT_SQUARED (TOUCH_GAMEPAD_TAP_DISTANCE_LIMIT * TOUCH_GAMEPAD_TAP_DISTANCE_LIMIT)
#define TOUCH_GAMEPAD_SLIDE_DISTANCE_MINIMUM_SQUARED (TOUCH_GAMEPAD_SLIDE_DISTANCE_MINIMUM * TOUCH_GAMEPAD_SLIDE_DISTANCE_MINIMUM)

static const char *TAG = "touch_gamepad";

enum {
    TOUCH_GAMEPAD_CORNER_LOWER_LEFT = 0,
    TOUCH_GAMEPAD_CORNER_UPPER_LEFT,
    TOUCH_GAMEPAD_CORNER_UPPER_RIGHT,
    TOUCH_GAMEPAD_CORNER_LOWER_RIGHT,
    TOUCH_GAMEPAD_CORNER_COUNT,
    TOUCH_GAMEPAD_CORNER_INVALID = 255,
};

typedef struct {
    uint32_t version;
    touch_gamepad_transport_t transport_mode;
    touch_gamepad_theme_t theme;
    uint8_t tap_buttons[TOUCH_GAMEPAD_TAP_BINDING_COUNT];
    touch_gamepad_axis_binding_t one_finger_slide;
    touch_gamepad_axis_binding_t two_finger_slide;
} touch_gamepad_persisted_config_t;

static const uint8_t s_default_tap_buttons[TOUCH_GAMEPAD_TAP_BINDING_COUNT] = {
    0, 1, 2, 3, 4, 5, 6, 7,
};

static const char *const s_button_labels[] = {
    "A",
    "B",
    "X",
    "Y",
    "L1",
    "R1",
    "L2",
    "R2",
    "START",
    "SELECT",
    "L3",
    "R3",
};

#define TOUCH_GAMEPAD_BUTTON_LABEL_COUNT ((uint8_t)(sizeof(s_button_labels) / sizeof(s_button_labels[0])))

static const touch_gamepad_board_preset_t s_guition_preset = {
    .board_name = "Guition ESP32-S3-4848S040",
    .supports_usb = false,
    .screen_width = CONFIG_TOUCH_GAMEPAD_SCREEN_WIDTH,
    .screen_height = CONFIG_TOUCH_GAMEPAD_SCREEN_HEIGHT,
    .default_transport = TOUCH_GAMEPAD_TRANSPORT_BLE,
};

static const touch_gamepad_board_preset_t s_waveshare_preset = {
    .board_name = "Waveshare ESP32-S3-Touch-LCD-4",
    .supports_usb = true,
    .screen_width = CONFIG_TOUCH_GAMEPAD_SCREEN_WIDTH,
    .screen_height = CONFIG_TOUCH_GAMEPAD_SCREEN_HEIGHT,
    .default_transport = TOUCH_GAMEPAD_TRANSPORT_USB,
};

static uint8_t touch_gamepad_identify_corner(const touch_gamepad_point_t *point, const touch_gamepad_board_preset_t *preset)
{
    const int16_t width = (int16_t)preset->screen_width;
    const int16_t height = (int16_t)preset->screen_height;
    const int16_t margin = CONFIG_TOUCH_GAMEPAD_CORNER_MARGIN;

    if ((point->x <= margin) && (point->y >= (height - margin))) {
        return TOUCH_GAMEPAD_CORNER_LOWER_LEFT;
    }

    if ((point->x <= margin) && (point->y <= margin)) {
        return TOUCH_GAMEPAD_CORNER_UPPER_LEFT;
    }

    if ((point->x >= (width - margin)) && (point->y <= margin)) {
        return TOUCH_GAMEPAD_CORNER_UPPER_RIGHT;
    }

    if ((point->x >= (width - margin)) && (point->y >= (height - margin))) {
        return TOUCH_GAMEPAD_CORNER_LOWER_RIGHT;
    }

    return TOUCH_GAMEPAD_CORNER_INVALID;
}

static bool touch_gamepad_is_tap(const touch_gamepad_touch_frame_t *frame)
{
    uint8_t index;

    for (index = 0; index < frame->finger_count; ++index) {
        const int16_t delta_x = frame->end[index].x - frame->start[index].x;
        const int16_t delta_y = frame->end[index].y - frame->start[index].y;
        const int32_t distance_squared = ((int32_t)delta_x * delta_x) + ((int32_t)delta_y * delta_y);

        if (distance_squared > TOUCH_GAMEPAD_TAP_DISTANCE_LIMIT_SQUARED) {
            return false;
        }
    }

    return true;
}

static uint8_t touch_gamepad_upper_half_zone(const touch_gamepad_point_t *point, const touch_gamepad_board_preset_t *preset)
{
    const int16_t half_height = (int16_t)(preset->screen_height / 2U);
    const int16_t row_height = (int16_t)(half_height / 2U);
    const int16_t column_width = (int16_t)(preset->screen_width / 2U);
    const uint8_t row = (point->y >= row_height) ? 1U : 0U;
    const uint8_t column = (point->x >= column_width) ? 1U : 0U;

    return (uint8_t)((row * 2U) + column);
}

static bool touch_gamepad_all_points_in_upper_half(const touch_gamepad_touch_frame_t *frame,
                                                   const touch_gamepad_board_preset_t *preset)
{
    uint8_t index;

    for (index = 0; index < frame->finger_count; ++index) {
        if (frame->start[index].y >= (int16_t)(preset->screen_height / 2U)) {
            return false;
        }
    }

    return true;
}

static bool touch_gamepad_all_points_in_lower_half(const touch_gamepad_touch_frame_t *frame,
                                                   const touch_gamepad_board_preset_t *preset)
{
    uint8_t index;

    for (index = 0; index < frame->finger_count; ++index) {
        if (frame->start[index].y < (int16_t)(preset->screen_height / 2U)) {
            return false;
        }
    }

    return true;
}

static void touch_gamepad_copy_persisted_to_runtime(const touch_gamepad_persisted_config_t *persisted,
                                                    touch_gamepad_config_t *config)
{
    config->transport_mode = persisted->transport_mode;
    config->theme = persisted->theme;
    memcpy(config->tap_buttons, persisted->tap_buttons, sizeof(config->tap_buttons));
    config->one_finger_slide = persisted->one_finger_slide;
    config->two_finger_slide = persisted->two_finger_slide;
    config->repair_requested = false;
}

static void touch_gamepad_copy_runtime_to_persisted(const touch_gamepad_config_t *config,
                                                    touch_gamepad_persisted_config_t *persisted)
{
    memset(persisted, 0, sizeof(*persisted));
    persisted->version = TOUCH_GAMEPAD_CONFIG_VERSION;
    persisted->transport_mode = config->transport_mode;
    persisted->theme = config->theme;
    memcpy(persisted->tap_buttons, config->tap_buttons, sizeof(config->tap_buttons));
    persisted->one_finger_slide = config->one_finger_slide;
    persisted->two_finger_slide = config->two_finger_slide;
}

static void touch_gamepad_sanitize_config(touch_gamepad_config_t *config)
{
    const touch_gamepad_board_preset_t *preset = touch_gamepad_get_board_preset();
    size_t index;

    if (config->transport_mode > TOUCH_GAMEPAD_TRANSPORT_USB) {
        ESP_LOGW(TAG, "Invalid stored transport mode %d, restoring default", (int)config->transport_mode);
        config->transport_mode = preset->default_transport;
    }

    if ((!preset->supports_usb) && (config->transport_mode == TOUCH_GAMEPAD_TRANSPORT_USB)) {
        ESP_LOGW(TAG, "Stored USB mode is not supported on %s, forcing BLE", preset->board_name);
        config->transport_mode = TOUCH_GAMEPAD_TRANSPORT_BLE;
    }

    if (config->theme > TOUCH_GAMEPAD_THEME_GREEN_ON_BLACK) {
        ESP_LOGW(TAG, "Invalid stored theme %d, restoring default", (int)config->theme);
#if CONFIG_TOUCH_GAMEPAD_DEFAULT_THEME_GREEN
        config->theme = TOUCH_GAMEPAD_THEME_GREEN_ON_BLACK;
#else
        config->theme = TOUCH_GAMEPAD_THEME_BLUE_ON_BLACK;
#endif
    }

    for (index = 0; index < TOUCH_GAMEPAD_TAP_BINDING_COUNT; ++index) {
        if (config->tap_buttons[index] >= TOUCH_GAMEPAD_BUTTON_LABEL_COUNT) {
            ESP_LOGW(TAG,
                     "Invalid stored button index %u for tap binding %u, restoring default",
                     (unsigned)config->tap_buttons[index],
                     (unsigned)index);
            config->tap_buttons[index] = s_default_tap_buttons[index];
        }
    }
}

const touch_gamepad_board_preset_t *touch_gamepad_get_board_preset(void)
{
#if CONFIG_TOUCH_GAMEPAD_BOARD_GUITION
    return &s_guition_preset;
#else
    return &s_waveshare_preset;
#endif
}

const char *touch_gamepad_transport_name(touch_gamepad_transport_t transport_mode)
{
    return (transport_mode == TOUCH_GAMEPAD_TRANSPORT_USB) ? "USB" : "BLE";
}

const char *touch_gamepad_theme_name(touch_gamepad_theme_t theme)
{
    return (theme == TOUCH_GAMEPAD_THEME_GREEN_ON_BLACK) ? "green-on-black" : "blue-on-black";
}

const char *touch_gamepad_menu_item_name(touch_gamepad_menu_item_t item)
{
    switch (item) {
    case TOUCH_GAMEPAD_MENU_ITEM_TRANSPORT:
        return "BLE/USB mode";
    case TOUCH_GAMEPAD_MENU_ITEM_REPAIR:
        return "Start BLE pairing";
    case TOUCH_GAMEPAD_MENU_ITEM_MAPPING:
        return "Buttons and axes mapping";
    case TOUCH_GAMEPAD_MENU_ITEM_THEME:
        return "Color theme";
    default:
        return "Unknown";
    }
}

void touch_gamepad_config_set_defaults(touch_gamepad_config_t *config)
{
    const touch_gamepad_board_preset_t *preset = touch_gamepad_get_board_preset();

    memset(config, 0, sizeof(*config));
    config->transport_mode = preset->default_transport;
#if CONFIG_TOUCH_GAMEPAD_DEFAULT_THEME_GREEN
    config->theme = TOUCH_GAMEPAD_THEME_GREEN_ON_BLACK;
#else
    config->theme = TOUCH_GAMEPAD_THEME_BLUE_ON_BLACK;
#endif
    memcpy(config->tap_buttons, s_default_tap_buttons, sizeof(config->tap_buttons));
    config->one_finger_slide.axis_x = 0;
    config->one_finger_slide.axis_y = 1;
    config->two_finger_slide.axis_x = 2;
    config->two_finger_slide.axis_y = 3;
    config->repair_requested = false;
}

esp_err_t touch_gamepad_config_load(touch_gamepad_config_t *config)
{
    touch_gamepad_persisted_config_t persisted = { 0 };
    nvs_handle_t handle;
    size_t required_size = sizeof(persisted);
    esp_err_t err = nvs_open(TOUCH_GAMEPAD_NVS_NAMESPACE, NVS_READONLY, &handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No stored config found, using defaults");
        touch_gamepad_config_set_defaults(config);
        return ESP_OK;
    }

    if (err != ESP_OK) {
        return err;
    }

    err = nvs_get_blob(handle, TOUCH_GAMEPAD_NVS_KEY, &persisted, &required_size);
    nvs_close(handle);

    if ((err != ESP_OK) || (required_size != sizeof(persisted)) || (persisted.version != TOUCH_GAMEPAD_CONFIG_VERSION)) {
        ESP_LOGW(TAG,
                 "Stored config invalid (err=%s size=%u version=%u), using defaults",
                 esp_err_to_name(err),
                 (unsigned)required_size,
                 (unsigned)persisted.version);
        touch_gamepad_config_set_defaults(config);
        return ESP_OK;
    }

    touch_gamepad_copy_persisted_to_runtime(&persisted, config);
    touch_gamepad_sanitize_config(config);

    return ESP_OK;
}

esp_err_t touch_gamepad_config_save(const touch_gamepad_config_t *config)
{
    touch_gamepad_persisted_config_t persisted;
    nvs_handle_t handle;

    touch_gamepad_copy_runtime_to_persisted(config, &persisted);
    esp_err_t err = nvs_open(TOUCH_GAMEPAD_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_blob(handle, TOUCH_GAMEPAD_NVS_KEY, &persisted, sizeof(persisted));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    return err;
}

bool touch_gamepad_detect_gesture(touch_gamepad_gesture_state_t *state,
                                  const touch_gamepad_touch_frame_t *frame,
                                  touch_gamepad_gesture_event_t *event)
{
    const touch_gamepad_board_preset_t *preset = touch_gamepad_get_board_preset();
    const bool is_tap = touch_gamepad_is_tap(frame);

    memset(event, 0, sizeof(*event));
    event->type = TOUCH_GAMEPAD_GESTURE_NONE;

    if ((frame->finger_count == 0U) || (frame->finger_count > 2U)) {
        state->expected_corner_index = 0U;
        return false;
    }

    if (is_tap) {
        const uint8_t corner = touch_gamepad_identify_corner(&frame->start[0], preset);
        static const uint8_t expected_corners[TOUCH_GAMEPAD_CORNER_COUNT] = {
            TOUCH_GAMEPAD_CORNER_LOWER_LEFT,
            TOUCH_GAMEPAD_CORNER_UPPER_LEFT,
            TOUCH_GAMEPAD_CORNER_UPPER_RIGHT,
            TOUCH_GAMEPAD_CORNER_LOWER_RIGHT,
        };

        if (corner == expected_corners[state->expected_corner_index]) {
            state->expected_corner_index += 1U;
            if (state->expected_corner_index == TOUCH_GAMEPAD_CORNER_COUNT) {
                state->expected_corner_index = 0U;
                event->type = TOUCH_GAMEPAD_GESTURE_OPEN_MENU;
                return true;
            }
        } else {
            state->expected_corner_index = (corner == expected_corners[0]) ? 1U : 0U;
        }

        if (touch_gamepad_all_points_in_upper_half(frame, preset)) {
            event->type = TOUCH_GAMEPAD_GESTURE_TAP;
            event->finger_count = frame->finger_count;
            event->tap_zone = touch_gamepad_upper_half_zone(&frame->start[0], preset);
            return true;
        }

        return false;
    }

    if (touch_gamepad_all_points_in_lower_half(frame, preset)) {
        const int16_t delta_x = frame->end[0].x - frame->start[0].x;
        const int16_t delta_y = frame->end[0].y - frame->start[0].y;
        const int32_t distance_squared = ((int32_t)delta_x * delta_x) + ((int32_t)delta_y * delta_y);

        state->expected_corner_index = 0U;
        if (distance_squared <= TOUCH_GAMEPAD_SLIDE_DISTANCE_MINIMUM_SQUARED) {
            return false;
        }

        event->type = TOUCH_GAMEPAD_GESTURE_SLIDE;
        event->finger_count = frame->finger_count;
        event->delta_x = delta_x;
        event->delta_y = delta_y;
        return true;
    }

    state->expected_corner_index = 0U;
    return false;
}

void touch_gamepad_menu_init(touch_gamepad_menu_state_t *menu_state)
{
    memset(menu_state, 0, sizeof(*menu_state));
    menu_state->current_item = TOUCH_GAMEPAD_MENU_ITEM_TRANSPORT;
}

void touch_gamepad_menu_open(touch_gamepad_menu_state_t *menu_state)
{
    menu_state->open = true;
    menu_state->current_item = TOUCH_GAMEPAD_MENU_ITEM_TRANSPORT;
}

void touch_gamepad_menu_next(touch_gamepad_menu_state_t *menu_state)
{
    if (!menu_state->open) {
        return;
    }

    menu_state->current_item = (touch_gamepad_menu_item_t)((menu_state->current_item + 1U) % TOUCH_GAMEPAD_MENU_ITEM_COUNT);
}

void touch_gamepad_build_mapping_snapshot(const touch_gamepad_config_t *config,
                                          touch_gamepad_mapping_snapshot_t *snapshot)
{
    size_t index;

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->one_finger_slide = config->one_finger_slide;
    snapshot->two_finger_slide = config->two_finger_slide;

    for (index = 0; index < TOUCH_GAMEPAD_TAP_BINDING_COUNT; ++index) {
        uint8_t button = config->tap_buttons[index];

        if (button >= TOUCH_GAMEPAD_BUTTON_LABEL_COUNT) {
            button = s_default_tap_buttons[index];
        }

        snapshot->tap_bindings[index].button = button;
        snprintf(snapshot->tap_bindings[index].label,
                 sizeof(snapshot->tap_bindings[index].label),
                 "%s",
                 s_button_labels[button]);
    }
}

esp_err_t touch_gamepad_set_tap_binding(touch_gamepad_config_t *config, uint8_t binding_index, uint8_t button)
{
    if ((binding_index >= TOUCH_GAMEPAD_TAP_BINDING_COUNT) || (button >= TOUCH_GAMEPAD_BUTTON_LABEL_COUNT)) {
        return ESP_ERR_INVALID_ARG;
    }

    config->tap_buttons[binding_index] = button;
    return touch_gamepad_config_save(config);
}

esp_err_t touch_gamepad_set_slide_binding(touch_gamepad_config_t *config,
                                          uint8_t finger_count,
                                          int8_t axis_x,
                                          int8_t axis_y)
{
    if (finger_count == 1U) {
        config->one_finger_slide.axis_x = axis_x;
        config->one_finger_slide.axis_y = axis_y;
        return touch_gamepad_config_save(config);
    }

    if (finger_count == 2U) {
        config->two_finger_slide.axis_x = axis_x;
        config->two_finger_slide.axis_y = axis_y;
        return touch_gamepad_config_save(config);
    }

    return ESP_ERR_INVALID_ARG;
}

esp_err_t touch_gamepad_menu_activate(touch_gamepad_config_t *config,
                                      touch_gamepad_menu_state_t *menu_state,
                                      touch_gamepad_mapping_snapshot_t *snapshot)
{
    const touch_gamepad_board_preset_t *preset = touch_gamepad_get_board_preset();

    if (!menu_state->open) {
        return ESP_ERR_INVALID_STATE;
    }

    switch (menu_state->current_item) {
    case TOUCH_GAMEPAD_MENU_ITEM_TRANSPORT:
        if (preset->supports_usb) {
            config->transport_mode = (config->transport_mode == TOUCH_GAMEPAD_TRANSPORT_BLE)
                                         ? TOUCH_GAMEPAD_TRANSPORT_USB
                                         : TOUCH_GAMEPAD_TRANSPORT_BLE;
        } else {
            config->transport_mode = TOUCH_GAMEPAD_TRANSPORT_BLE;
        }
        break;
    case TOUCH_GAMEPAD_MENU_ITEM_REPAIR:
        if (config->transport_mode == TOUCH_GAMEPAD_TRANSPORT_BLE) {
            config->repair_requested = true;
        }
        break;
    case TOUCH_GAMEPAD_MENU_ITEM_MAPPING:
        if (snapshot == NULL) {
            return ESP_ERR_INVALID_ARG;
        }
        touch_gamepad_build_mapping_snapshot(config, snapshot);
        return ESP_OK;
    case TOUCH_GAMEPAD_MENU_ITEM_THEME:
        config->theme = (config->theme == TOUCH_GAMEPAD_THEME_BLUE_ON_BLACK)
                            ? TOUCH_GAMEPAD_THEME_GREEN_ON_BLACK
                            : TOUCH_GAMEPAD_THEME_BLUE_ON_BLACK;
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }

    return touch_gamepad_config_save(config);
}

void touch_gamepad_log_configuration(const touch_gamepad_config_t *config)
{
    touch_gamepad_mapping_snapshot_t snapshot;
    size_t index;

    touch_gamepad_build_mapping_snapshot(config, &snapshot);
    ESP_LOGI(TAG, "Transport: %s", touch_gamepad_transport_name(config->transport_mode));
    ESP_LOGI(TAG, "Theme: %s", touch_gamepad_theme_name(config->theme));
    ESP_LOGI(TAG, "One-finger slide axes: %d,%d", snapshot.one_finger_slide.axis_x, snapshot.one_finger_slide.axis_y);
    ESP_LOGI(TAG, "Two-finger slide axes: %d,%d", snapshot.two_finger_slide.axis_x, snapshot.two_finger_slide.axis_y);

    for (index = 0; index < TOUCH_GAMEPAD_TAP_BINDING_COUNT; ++index) {
        ESP_LOGI(TAG,
                 "Tap binding %u -> %s (button %u)",
                 (unsigned)index,
                 snapshot.tap_bindings[index].label,
                 (unsigned)snapshot.tap_bindings[index].button);
    }

    if (config->repair_requested) {
        ESP_LOGI(TAG, "BLE repair was requested and should clear the previous bond before advertising again.");
    }
}
