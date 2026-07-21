# AGENTS.md

## Scope

This repository is an ESP-IDF 6.x project for ESP32-S3 touch display boards that emulate a BLE or USB gamepad.

## Current board presets

- Guition ESP32-S3-4848S040: BLE only
- Waveshare ESP32-S3-Touch-LCD-4: BLE or USB

## Implementation notes

- Keep the upper half of the screen mapped to four tap zones.
- Keep the lower half of the screen mapped to one-finger and two-finger slide gestures.
- Preserve the menu unlock sequence: lower-left, upper-left, upper-right, lower-right.
- Keep the menu items limited to transport mode, BLE pairing reset, mapping, and theme selection unless requirements change.
- Store runtime configuration in NVS so gesture mappings and theme changes survive a reboot.

## Source file rule

Use ASCII characters only in every source file in this repository.
