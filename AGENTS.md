# AGENTS.md

## Scope

This repository is an ESP-IDF 6.x project for ESP32-S3 touch display boards that
emulate a BLE or USB gamepad.

## Current board presets

- Guition ESP32-S3-4848S040: BLE only
- Waveshare ESP32-S3-Touch-LCD-4: BLE or USB

## Source layout

- `main/main.c`: boot sequence and the touch event loop.
- `main/touch_gamepad.c` / `main/touch_gamepad.h`: board presets, gesture engine,
  menu state machine, and NVS-backed configuration.
- `main/boards.h`: per-board display and touch pin assignments.
- `main/display.c` / `main/display.h`: ST7701 RGB panel and LVGL port bring-up.
- `main/touchpad.c` / `main/touchpad.h`: GT911 touch controller wrapper.
- `main/ui.c` / `main/ui.h`: LVGL tap zones, slide surface, menu and mapping UI.
- `main/gamepad_hid.h`: shared HID report descriptor and report struct.
- `main/gamepad_backend.c` / `main/gamepad_backend.h`: transport dispatcher.
- `main/ble_gamepad.c` / `main/ble_gamepad.h`: BLE HID backend (Bluedroid + esp_hid).
- `main/usb_gamepad.c` / `main/usb_gamepad.h`: USB HID backend (TinyUSB).
- `main/idf_component.yml`: managed component dependencies.

## Implementation notes

- Keep the upper half of the screen mapped to four tap zones.
- Keep the lower half of the screen mapped to one-finger and two-finger slide
  gestures.
- Preserve the menu unlock sequence: lower-left, upper-left, upper-right,
  lower-right.
- Keep the menu items limited to transport mode, BLE pairing reset, mapping, and
  theme selection unless requirements change.
- Store runtime configuration in NVS so gesture mappings and theme changes
  survive a reboot.
- Both boards use a 480x480 ST7701 RGB panel and a GT911 touch controller; keep
  the shared HID report descriptor identical across the BLE and USB transports.
- Only compile the USB backend when the board exposes a USB device port
  (`CONFIG_TOUCH_GAMEPAD_ENABLE_USB`).

## Source file rule

Use ASCII characters only in every source file in this repository.
