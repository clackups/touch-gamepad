/*
 * USB HID gamepad backend (TinyUSB).
 *
 * Only compiled on boards that expose a USB device port. See AGENTS.md for the
 * ASCII-only rule.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#include "gamepad_hid.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t usb_gamepad_start(void);
void usb_gamepad_stop(void);
bool usb_gamepad_connected(void);
void usb_gamepad_send(const gamepad_report_t *report);

#ifdef __cplusplus
}
#endif
