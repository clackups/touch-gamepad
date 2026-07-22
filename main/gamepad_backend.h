/*
 * Transport-agnostic gamepad backend interface.
 *
 * The application maintains a single logical gamepad state (buttons and axes)
 * and pushes it through whichever transport the user selected. On boards that
 * do not expose USB only the BLE backend is compiled in.
 *
 * ASCII only. See AGENTS.md.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "gamepad_hid.h"
#include "touch_gamepad.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Bring up the gamepad using the requested transport. Only one transport is
 * active at a time. Returns ESP_ERR_NOT_SUPPORTED when USB is requested on a
 * board that does not expose a USB device port.
 */
esp_err_t gamepad_backend_start(touch_gamepad_transport_t transport);

/* Tear down the active backend so a different transport can be started. */
void gamepad_backend_stop(void);

/* True once the host has connected (BLE link established or USB mounted). */
bool gamepad_backend_connected(void);

/* Update the cached button state. Call gamepad_backend_send() to transmit. */
void gamepad_backend_set_button(uint8_t button_index, bool pressed);

/* Update a cached axis value in the range [-127, 127]. */
void gamepad_backend_set_axis(uint8_t axis_index, int8_t value);

/* Clear every button and re-center every axis. */
void gamepad_backend_reset_state(void);

/* Transmit the cached report to the host if a backend is connected. */
void gamepad_backend_send(void);

/*
 * Forget the current BLE bond and restart advertising so a new host can pair.
 * Only meaningful for the BLE transport; a no-op that returns ESP_OK otherwise.
 */
esp_err_t gamepad_backend_restart_pairing(void);

#ifdef __cplusplus
}
#endif
