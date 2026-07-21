# touch-gamepad

Touch Gamepad is an ESP-IDF 6.x project for ESP32-S3 touch displays that turns touchscreen gestures into gamepad input for a PC host.

## Supported presets

The repository now contains presets for these 480x480 boards:

- Guition ESP32-S3-4848S040
  - BLE gamepad only
  - No exposed USB port for HID mode
- Waveshare ESP32-S3-Touch-LCD-4
  - BLE gamepad mode
  - USB gamepad mode

## Gesture model

The app models the touchscreen input exactly as described in the issue:

- Upper half of the screen
  - Split into four tap areas arranged as two rows and two columns
  - Each area accepts one-finger taps and two-finger taps
  - The default button map uses eight bindings, one per area and finger count
- Lower half of the screen
  - One-finger slides map to one joystick axis pair
  - Two-finger slides map to a second joystick axis pair
- Configuration menu unlock gesture
  - Tap lower-left corner
  - Tap upper-left corner
  - Tap upper-right corner
  - Tap lower-right corner

## Configuration menu

The menu state machine exposes four items:

1. BLE/USB mode
   - Waveshare can toggle between BLE and USB.
   - Guition stays locked to BLE because the board does not expose USB for HID use.
2. Start BLE pairing
   - Marks the current bond for reset and requests a new pairing cycle.
3. Buttons and axes mapping
   - Persists the full tap and slide mapping set in NVS.
   - The current code exposes setter APIs for all eight tap bindings and for both slide axis pairs so a display UI can edit and persist them.
4. Color theme
   - Blue on black
   - Green on black

## Build presets

Use ESP-IDF 6.x and select the ESP32-S3 target before building.

### Guition preset

```sh
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.guition" set-target esp32s3 build
```

### Waveshare preset

```sh
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.waveshare" set-target esp32s3 build
```

## Project layout

- `/Kconfig.projbuild` defines the board, transport, theme, and screen-size presets.
- `/sdkconfig.defaults.guition` and `/sdkconfig.defaults.waveshare` select the board defaults.
- `/main/touch_gamepad.c` contains the preset table, touch gesture detection, menu logic, and persistent mapping configuration.
- `/main/main.c` initializes NVS, logs the selected preset, and exposes the gesture and menu contract to future board-specific HID and touchscreen drivers.

## Persistence

The runtime configuration is stored in NVS under the `touchgp` namespace. That persisted state includes:

- selected transport mode
- selected color theme
- eight tap-to-button bindings
- one-finger slide axis pair
- two-finger slide axis pair

## ASCII-only rule

Use ASCII characters only in source files. This applies to C, header, CMake, Kconfig, and documentation files that describe or configure the build.

## Validation status

This repository originally contained no source tree, build files, or tests. The current change adds the minimal ESP-IDF project skeleton and documents the exact preset and gesture behavior. If ESP-IDF tooling is available locally, the build commands above are the expected validation path.
