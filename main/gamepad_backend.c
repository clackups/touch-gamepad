/*
 * Transport-agnostic gamepad backend dispatcher.
 *
 * Holds the cached gamepad report and forwards it to whichever transport is
 * currently active. USB support is only linked in when the board exposes a USB
 * device port.
 *
 * ASCII only. See AGENTS.md.
 */
#include "gamepad_backend.h"

#include <string.h>

#include "esp_log.h"

#include "ble_gamepad.h"
#include "usb_gamepad.h"

static const char *TAG = "gamepad_backend";

static gamepad_report_t s_report;
static touch_gamepad_transport_t s_transport = TOUCH_GAMEPAD_TRANSPORT_BLE;
static bool s_active = false;

esp_err_t gamepad_backend_start(touch_gamepad_transport_t transport)
{
    esp_err_t err;

    if (s_active) {
        gamepad_backend_stop();
    }

    gamepad_backend_reset_state();

    if (transport == TOUCH_GAMEPAD_TRANSPORT_USB) {
#if CONFIG_TOUCH_GAMEPAD_ENABLE_USB
        err = usb_gamepad_start();
#else
        ESP_LOGE(TAG, "USB transport requested but not supported on this board");
        return ESP_ERR_NOT_SUPPORTED;
#endif
    } else {
        err = ble_gamepad_start();
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start %s transport: %s",
                 touch_gamepad_transport_name(transport), esp_err_to_name(err));
        return err;
    }

    s_transport = transport;
    s_active = true;
    return ESP_OK;
}

void gamepad_backend_stop(void)
{
    if (!s_active) {
        return;
    }

    if (s_transport == TOUCH_GAMEPAD_TRANSPORT_USB) {
        usb_gamepad_stop();
    } else {
        ble_gamepad_stop();
    }

    s_active = false;
}

bool gamepad_backend_connected(void)
{
    if (!s_active) {
        return false;
    }

    return (s_transport == TOUCH_GAMEPAD_TRANSPORT_USB) ? usb_gamepad_connected()
                                                        : ble_gamepad_connected();
}

void gamepad_backend_set_button(uint8_t button_index, bool pressed)
{
    if (button_index >= GAMEPAD_BUTTON_COUNT) {
        return;
    }

    const uint16_t mask = (uint16_t)(1U << button_index);
    if (pressed) {
        s_report.buttons |= mask;
    } else {
        s_report.buttons &= (uint16_t)~mask;
    }
}

void gamepad_backend_set_axis(uint8_t axis_index, int8_t value)
{
    if (axis_index >= GAMEPAD_AXIS_COUNT) {
        return;
    }

    s_report.axes[axis_index] = value;
}

void gamepad_backend_reset_state(void)
{
    memset(&s_report, 0, sizeof(s_report));
}

void gamepad_backend_send(void)
{
    if (!s_active) {
        return;
    }

    if (s_transport == TOUCH_GAMEPAD_TRANSPORT_USB) {
        usb_gamepad_send(&s_report);
    } else {
        ble_gamepad_send(&s_report);
    }
}

esp_err_t gamepad_backend_restart_pairing(void)
{
    if (s_active && s_transport == TOUCH_GAMEPAD_TRANSPORT_BLE) {
        return ble_gamepad_restart_pairing();
    }

    return ESP_OK;
}
