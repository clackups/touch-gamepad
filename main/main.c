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
#define APP_TAP_ZONE_COUNT 4

typedef struct {
    bool active;
    uint8_t finger_count;
    touch_gamepad_point_t start[TOUCHPAD_MAX_POINTS];
    touch_gamepad_point_t last[TOUCHPAD_MAX_POINTS];
} app_touch_tracker_t;

/*
 * Live gameplay input state. Buttons and the joystick are driven in real time:
 * a zone is held for as long as a finger rests on it and the joystick tracks the
 * finger continuously, so the HID report mirrors the actual touch state instead
 * of firing only when the finger lifts.
 */
typedef struct {
    bool zone_active[APP_TAP_ZONE_COUNT];
    bool joystick_active;
    int8_t axis_x;
    int8_t axis_y;
} app_live_state_t;

static touch_gamepad_config_t s_config;
static touch_gamepad_config_t s_draft;
static touch_gamepad_config_t s_mapping_backup;
static touch_gamepad_menu_state_t s_menu;
static touch_gamepad_gesture_state_t s_gesture_state;
static app_live_state_t s_live;
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

/* Re-center the joystick axes and hide its indicator. */
static void app_joystick_center(void)
{
    const touch_gamepad_axis_binding_t *binding = &s_config.one_finger_slide;

    if (binding->axis_x >= 0) {
        gamepad_backend_set_axis((uint8_t)binding->axis_x, 0);
    }
    if (binding->axis_y >= 0) {
        gamepad_backend_set_axis((uint8_t)binding->axis_y, 0);
    }
    s_live.axis_x = 0;
    s_live.axis_y = 0;
    s_live.joystick_active = false;
    ui_hide_joystick();
}

/* Release every held button and the joystick, transmitting once if needed. */
static void app_gameplay_release_all(void)
{
    bool changed = false;

    for (uint8_t zone = 0; zone < APP_TAP_ZONE_COUNT; ++zone) {
        if (s_live.zone_active[zone]) {
            s_live.zone_active[zone] = false;
            gamepad_backend_set_button(s_config.tap_buttons[zone * 2U], false);
            ui_set_zone_active(zone, false);
            changed = true;
        }
    }

    if (s_live.joystick_active) {
        app_joystick_center();
        changed = true;
    }

    if (changed) {
        gamepad_backend_send();
    }
}

/*
 * Translate the current set of touch points into live button and joystick state
 * (menu closed only). Upper-half fingers press their zone's button; the first
 * lower-half finger drives the joystick with axes proportional to its distance
 * from the fixed central point.
 */
static void app_gameplay_update(const touch_gamepad_point_t *points, uint8_t count)
{
    const int16_t width = (int16_t)s_preset->screen_width;
    const int16_t height = (int16_t)s_preset->screen_height;
    const int16_t half_height = (int16_t)(height / 2);

    bool zone_now[APP_TAP_ZONE_COUNT] = { false, false, false, false };
    bool have_joystick = false;
    touch_gamepad_point_t joystick = { 0, 0 };

    for (uint8_t i = 0; i < count; ++i) {
        if (points[i].y < half_height) {
            const uint8_t row = (points[i].y >= (int16_t)(height / 4)) ? 1U : 0U;
            const uint8_t col = (points[i].x >= (int16_t)(width / 2)) ? 1U : 0U;
            zone_now[(row * 2U) + col] = true;
        } else if (!have_joystick) {
            have_joystick = true;
            joystick = points[i];
        }
    }

    bool changed = false;

    for (uint8_t zone = 0; zone < APP_TAP_ZONE_COUNT; ++zone) {
        if (zone_now[zone] != s_live.zone_active[zone]) {
            s_live.zone_active[zone] = zone_now[zone];
            gamepad_backend_set_button(s_config.tap_buttons[zone * 2U], zone_now[zone]);
            ui_set_zone_active(zone, zone_now[zone]);
            changed = true;
        }
    }

    if (have_joystick) {
        const int16_t center_x = (int16_t)(width / 2);
        const int16_t center_y = (int16_t)(half_height + (half_height / 2));
        const int16_t delta_x = (int16_t)(joystick.x - center_x);
        const int16_t delta_y = (int16_t)(joystick.y - center_y);
        const int8_t value_x = app_scale_axis(delta_x, s_preset->screen_width);
        const int8_t value_y = app_scale_axis(delta_y, (uint16_t)half_height);
        const touch_gamepad_axis_binding_t *binding = &s_config.one_finger_slide;

        if (!s_live.joystick_active || (value_x != s_live.axis_x) || (value_y != s_live.axis_y)) {
            if (binding->axis_x >= 0) {
                gamepad_backend_set_axis((uint8_t)binding->axis_x, value_x);
            }
            if (binding->axis_y >= 0) {
                gamepad_backend_set_axis((uint8_t)binding->axis_y, value_y);
            }
            s_live.axis_x = value_x;
            s_live.axis_y = value_y;
            changed = true;
        }
        s_live.joystick_active = true;
        ui_show_joystick(delta_x, delta_y);
    } else if (s_live.joystick_active) {
        app_joystick_center();
        changed = true;
    }

    if (changed) {
        gamepad_backend_send();
    }
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
            /* Drop any held button/joystick input before entering the menu. */
            app_gameplay_release_all();
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

    /*
     * During gameplay, buttons and the joystick are driven in real time by
     * app_gameplay_update(); completed-gesture events other than the menu
     * unlock sequence handled above are intentionally ignored here.
     */
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

        /*
         * Drive buttons and the joystick from the live touch state every poll so
         * the HID report tracks press, hold and release in real time. When the
         * menu is open, gameplay input is suspended (and already released on
         * open).
         */
        if (!s_menu.open) {
            app_gameplay_update(points, count);
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
    memset(&s_live, 0, sizeof(s_live));

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
