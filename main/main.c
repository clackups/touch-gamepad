/*
 * Touch Gamepad application entry point.
 *
 * Boot sequence:
 *   1. Initialize NVS and load the persisted configuration.
 *   2. Bring up the display, LVGL UI and GT911 touch controller.
 *   3. Start the selected gamepad transport (BLE, or USB on capable boards).
 *   4. Poll touch input, turn completed gestures into gamepad reports, and open
 *      or drive the configuration menu on the unlock sequence.
 *
 * The gesture engine and persistence live in touch_gamepad.c; this file owns
 * the hardware wiring and the runtime event loop.
 *
 * ASCII only. See AGENTS.md.
 */
#include "touch_gamepad.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "display.h"
#include "gamepad_backend.h"
#include "touchpad.h"
#include "ui.h"
#include "waveshare_board.h"

static const char *TAG = "app_main";

#define APP_POLL_INTERVAL_MS 20
#define APP_BUTTON_HOLD_MS 60
#define APP_SLIDE_RECENTER_MS 150

typedef struct {
    bool active;
    uint8_t finger_count;
    touch_gamepad_point_t start[TOUCHPAD_MAX_POINTS];
    touch_gamepad_point_t last[TOUCHPAD_MAX_POINTS];
} app_touch_tracker_t;

static touch_gamepad_config_t s_config;
static touch_gamepad_config_t s_draft;
static touch_gamepad_config_t s_mapping_backup;
static touch_gamepad_menu_state_t s_menu;
static touch_gamepad_gesture_state_t s_gesture_state;
static const touch_gamepad_board_preset_t *s_preset;

static int8_t app_scale_axis(int16_t delta, uint16_t span)
{
    if (span == 0U) {
        return 0;
    }

    int32_t value = ((int32_t)delta * 127) / (int32_t)(span / 2U);
    if (value > 127) {
        value = 127;
    } else if (value < -127) {
        value = -127;
    }
    return (int8_t)value;
}

static void app_pulse_button(uint8_t button_index)
{
    gamepad_backend_set_button(button_index, true);
    gamepad_backend_send();
    vTaskDelay(pdMS_TO_TICKS(APP_BUTTON_HOLD_MS));
    gamepad_backend_set_button(button_index, false);
    gamepad_backend_send();
}

static void app_apply_slide(const touch_gamepad_gesture_event_t *event)
{
    const touch_gamepad_axis_binding_t *binding =
        (event->finger_count >= 2U) ? &s_config.two_finger_slide : &s_config.one_finger_slide;

    const int8_t value_x = app_scale_axis(event->delta_x, s_preset->screen_width);
    const int8_t value_y = app_scale_axis(event->delta_y, s_preset->screen_height);

    if (binding->axis_x >= 0) {
        gamepad_backend_set_axis((uint8_t)binding->axis_x, value_x);
    }
    if (binding->axis_y >= 0) {
        gamepad_backend_set_axis((uint8_t)binding->axis_y, value_y);
    }
    gamepad_backend_send();

    ui_show_slide(event->finger_count, event->delta_x, event->delta_y);

    vTaskDelay(pdMS_TO_TICKS(APP_SLIDE_RECENTER_MS));

    if (binding->axis_x >= 0) {
        gamepad_backend_set_axis((uint8_t)binding->axis_x, 0);
    }
    if (binding->axis_y >= 0) {
        gamepad_backend_set_axis((uint8_t)binding->axis_y, 0);
    }
    gamepad_backend_send();
}

static void app_switch_transport(void)
{
    esp_err_t err = gamepad_backend_start(s_config.transport_mode);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Transport start failed: %s", esp_err_to_name(err));
        return;
    }
    ui_set_status(s_config.transport_mode, gamepad_backend_connected());
}

static void app_close_menu(void)
{
    s_menu.open = false;
    ui_hide_menu();
}

/* Persist the edited draft, apply live side effects and leave the menu. */
static void app_menu_save(void)
{
    const touch_gamepad_transport_t previous_transport = s_config.transport_mode;

    s_config = s_draft;
    esp_err_t err = touch_gamepad_config_save(&s_config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Config save failed: %s", esp_err_to_name(err));
    }

    ui_set_theme(s_config.theme);
    if (s_config.transport_mode != previous_transport) {
        app_switch_transport();
    }
    touch_gamepad_log_configuration(&s_config);
    app_close_menu();
    ESP_LOGI(TAG, "Menu saved");
}

/* Drop the draft edits and leave the menu, restoring the previewed theme. */
static void app_menu_cancel(void)
{
    ui_set_theme(s_config.theme);
    app_close_menu();
    ESP_LOGI(TAG, "Menu cancelled");
}

static void app_handle_menu_result(touch_gamepad_menu_result_t result)
{
    switch (result) {
    case TOUCH_GAMEPAD_MENU_RESULT_THEME_CHANGED:
        ui_set_theme(s_draft.theme);
        ui_show_menu(&s_menu, &s_draft, s_preset);
        break;
    case TOUCH_GAMEPAD_MENU_RESULT_CHANGED:
        ui_show_menu(&s_menu, &s_draft, s_preset);
        break;
    case TOUCH_GAMEPAD_MENU_RESULT_OPEN_MAPPING:
        s_mapping_backup = s_draft;
        s_menu.screen = TOUCH_GAMEPAD_MENU_SCREEN_MAPPING;
        s_menu.selected = 0U;
        ui_show_menu(&s_menu, &s_draft, s_preset);
        break;
    case TOUCH_GAMEPAD_MENU_RESULT_REPAIR:
        if (s_draft.transport_mode == TOUCH_GAMEPAD_TRANSPORT_BLE) {
            gamepad_backend_restart_pairing();
        }
        ui_show_menu(&s_menu, &s_draft, s_preset);
        break;
    case TOUCH_GAMEPAD_MENU_RESULT_SAVE:
        if (s_menu.screen == TOUCH_GAMEPAD_MENU_SCREEN_MAPPING) {
            /* Keep the mapping edits in the draft and return to the main menu. */
            s_menu.screen = TOUCH_GAMEPAD_MENU_SCREEN_MAIN;
            s_menu.selected = 0U;
            ui_show_menu(&s_menu, &s_draft, s_preset);
        } else {
            app_menu_save();
        }
        break;
    case TOUCH_GAMEPAD_MENU_RESULT_CANCEL:
        if (s_menu.screen == TOUCH_GAMEPAD_MENU_SCREEN_MAPPING) {
            /* Discard the mapping edits made since entering the sub-menu. */
            s_draft = s_mapping_backup;
            s_menu.screen = TOUCH_GAMEPAD_MENU_SCREEN_MAIN;
            s_menu.selected = 0U;
            ui_show_menu(&s_menu, &s_draft, s_preset);
        } else {
            app_menu_cancel();
        }
        break;
    case TOUCH_GAMEPAD_MENU_RESULT_NONE:
    default:
        break;
    }
}

static void app_handle_gesture(const touch_gamepad_gesture_event_t *event)
{
    if (event->type == TOUCH_GAMEPAD_GESTURE_OPEN_MENU) {
        if (s_menu.open) {
            /* The unlock sequence closes the menu, discarding any draft edits. */
            app_menu_cancel();
        } else {
            s_draft = s_config;
            touch_gamepad_menu_open(&s_menu);
            ui_show_menu(&s_menu, &s_draft, s_preset);
            ESP_LOGI(TAG, "Menu opened");
        }
        return;
    }

    if (s_menu.open) {
        /*
         * The configuration menu is driven by one-finger taps only. A tap on a
         * row selects it: choice rows rotate their value depending on whether
         * the left or right half of the row was tapped, action rows run their
         * action. A one-finger slide scrolls tall menus.
         */
        if ((event->type == TOUCH_GAMEPAD_GESTURE_TAP) && (event->finger_count == 1U)) {
            uint8_t row = 0;
            int8_t direction = 0;
            if (ui_menu_hit_test(event->tap_x, event->tap_y, &row, &direction)) {
                const touch_gamepad_menu_result_t result =
                    touch_gamepad_menu_tap_row(&s_menu, &s_draft, s_preset, row, direction);
                app_handle_menu_result(result);
            }
        } else if (event->type == TOUCH_GAMEPAD_GESTURE_SLIDE) {
            ui_menu_scroll(event->delta_y);
        }
        return;
    }

    if (event->type == TOUCH_GAMEPAD_GESTURE_TAP) {
        if (!event->tap_upper_half) {
            return;
        }
        const uint8_t finger_slot = (event->finger_count >= 2U) ? 1U : 0U;
        const uint8_t binding_index = (uint8_t)(event->tap_zone * 2U + finger_slot);
        if (binding_index < TOUCH_GAMEPAD_TAP_BINDING_COUNT) {
            const uint8_t button = s_config.tap_buttons[binding_index];
            ui_flash_tap(event->tap_zone, event->finger_count);
            app_pulse_button(button);
        }
        return;
    }

    if (event->type == TOUCH_GAMEPAD_GESTURE_SLIDE) {
        app_apply_slide(event);
    }
}

static void app_process_release(app_touch_tracker_t *tracker)
{
    touch_gamepad_touch_frame_t frame;
    touch_gamepad_gesture_event_t event;

    memset(&frame, 0, sizeof(frame));
    frame.finger_count = tracker->finger_count;
    for (uint8_t i = 0; i < tracker->finger_count && i < 2U; ++i) {
        frame.start[i] = tracker->start[i];
        frame.end[i] = tracker->last[i];
    }

    if (touch_gamepad_detect_gesture(&s_gesture_state, &frame, &event)) {
        app_handle_gesture(&event);
    }
}

static void app_event_loop(void)
{
    app_touch_tracker_t tracker;
    memset(&tracker, 0, sizeof(tracker));

    for (;;) {
        touch_gamepad_point_t points[TOUCHPAD_MAX_POINTS];
        const uint8_t count = touchpad_read(points);

        if (count > 0U) {
            if (!tracker.active) {
                tracker.active = true;
                tracker.finger_count = count;
                for (uint8_t i = 0; i < count; ++i) {
                    tracker.start[i] = points[i];
                    tracker.last[i] = points[i];
                }
            } else {
                for (uint8_t i = 0; i < count && i < TOUCHPAD_MAX_POINTS; ++i) {
                    if (i >= tracker.finger_count) {
                        /*
                         * A finger that lands after the initial touch-down.
                         * Record its starting position so multi-finger taps are
                         * measured from where each finger actually touched down;
                         * otherwise its start stays at the origin and the tap is
                         * misread as a large slide, which breaks two-finger tap
                         * gestures such as menu activation.
                         */
                        tracker.start[i] = points[i];
                    }
                    tracker.last[i] = points[i];
                }
                if (count > tracker.finger_count) {
                    tracker.finger_count = count;
                }
            }
        } else if (tracker.active) {
            app_process_release(&tracker);
            memset(&tracker, 0, sizeof(tracker));
            ui_set_status(s_config.transport_mode, gamepad_backend_connected());
        }

        vTaskDelay(pdMS_TO_TICKS(APP_POLL_INTERVAL_MS));
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    s_preset = touch_gamepad_get_board_preset();
    ESP_ERROR_CHECK(touch_gamepad_config_load(&s_config));
    touch_gamepad_menu_init(&s_menu);
    memset(&s_gesture_state, 0, sizeof(s_gesture_state));

    if (!s_preset->supports_usb && s_config.transport_mode == TOUCH_GAMEPAD_TRANSPORT_USB) {
        s_config.transport_mode = TOUCH_GAMEPAD_TRANSPORT_BLE;
        ESP_ERROR_CHECK(touch_gamepad_config_save(&s_config));
    }

    ESP_LOGI(TAG, "Board preset: %s (%ux%u, USB %s)",
             s_preset->board_name, s_preset->screen_width, s_preset->screen_height,
             s_preset->supports_usb ? "yes" : "no");
    touch_gamepad_log_configuration(&s_config);

    ESP_ERROR_CHECK(waveshare_board_bringup());
    ESP_ERROR_CHECK(display_init(NULL));
    ESP_ERROR_CHECK(ui_init(&s_config, s_preset));
    err = touchpad_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Touch init failed (%s); continuing without touch input",
                 esp_err_to_name(err));
    }

    err = gamepad_backend_start(s_config.transport_mode);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falling back to BLE transport");
        s_config.transport_mode = TOUCH_GAMEPAD_TRANSPORT_BLE;
        ESP_ERROR_CHECK(gamepad_backend_start(s_config.transport_mode));
    }
    ui_set_status(s_config.transport_mode, gamepad_backend_connected());

    ESP_LOGI(TAG, "Entering touch event loop");
    app_event_loop();
}
