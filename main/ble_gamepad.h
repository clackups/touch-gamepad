/*
 * BLE HID gamepad backend (Bluedroid + esp_hid device role).
 *
 * ASCII only. See AGENTS.md.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "gamepad_hid.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ble_gamepad_start(void);
void ble_gamepad_stop(void);
bool ble_gamepad_connected(void);
void ble_gamepad_send(const gamepad_report_t *report);
esp_err_t ble_gamepad_restart_pairing(void);

#ifdef __cplusplus
}
#endif
