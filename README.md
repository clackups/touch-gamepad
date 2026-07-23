# touch-gamepad

Touch Gamepad is an ESP-IDF 6.x firmware for ESP32-S3 touch displays that turns
touchscreen gestures into gamepad input for a PC or console host. It renders a
gesture surface with LVGL, reads a GT911 capacitive touch panel, and reports the
resulting buttons and joystick axes over BLE HID or (on capable boards) USB HID.

## Supported boards

| Board | Display | Touch | Transports |
| ----- | ------- | ----- | ---------- |
| Guition ESP32-S3-4848S040 | 480x480 ST7701 RGB | GT911 | BLE only |
| Waveshare ESP32-S3-Touch-LCD-4 (untested) | 480x480 ST7701 RGB | GT911 | BLE or USB |

> **Warning: the Waveshare ESP32-S3-Touch-LCD-4 support is untested and
> unreliable.** It has not been verified on real hardware, and even the official
> Waveshare demos do not run correctly on the unit used for development, which
> points to faulty hardware. Treat this board as experimental: the pin map,
> bring-up sequence and transports may not work. The Guition ESP32-S3-4848S040 is
> the supported, working target.

The Guition ESP32-S3-4848S040 does not expose a USB device port, so it acts only
as a BLE gamepad. The Waveshare ESP32-S3-Touch-LCD-4 is intended to act as either
a BLE or a USB gamepad, with the active transport chosen from the configuration
menu and persisted across reboots, but see the warning above about its untested
state.

Reference material for the two boards:

- Guition ESP32-S3-4848S040
  - https://www.guition.com/esp32-display-module/4-inch-esp32s3-display-module
  - https://devices.esphome.io/devices/guition-esp32-s3-4848s040/
  - https://github.com/agillis/esphome-modular-lvgl-buttons
- Waveshare ESP32-S3-Touch-LCD-4
  - https://www.waveshare.com/esp32-s3-touch-lcd-4.htm
  - https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-4

## Gesture model

- Upper half of the screen
  - Split into four tap zones arranged as two rows and two columns.
  - Touching a zone presses the gamepad button bound to that zone and lifting the
    finger releases it, so the button is held for exactly as long as the finger
    rests on the zone. The zone lights up while it is held.
- Lower half of the screen
  - Acts as an analog joystick with a fixed central point. Sliding the finger
    away from the center drives a joystick axis pair whose values are
    proportional to the distance from the center, and the axes re-center when the
    finger lifts.
  - The screen draws the central point and a vector pointing from it to the
    current touch point.
- Configuration menu unlock gesture
  - Tap the corners in the order lower-left, upper-left, upper-right,
    lower-right. Repeating the sequence closes the menu (discarding unsaved
    edits, like Cancel).

## Configuration menu

The menu is a full-screen, scrollable list driven entirely by one-finger taps.
Each row is a large touch target. Tap a row to select it:

- On a multiple-choice row, tap the left half of the row to rotate to the
  previous value or the right half to rotate to the next value. The current
  value is shown between `<` and `>` arrows as a reminder.
- On an action row (Buttons and axes, BLE pairing reset, Save, Cancel), a tap
  runs the action.

If the list is taller than the screen, a one-finger slide up or down scrolls it
so every row (including Save and Cancel at the bottom) can be reached. Edits are
made on a working copy and only written to NVS when you tap **Save**; **Cancel**
(or the unlock sequence) leaves the menu without saving.

Main screen rows:

1. Transport (BLE/USB)
   - Waveshare rotates between BLE and USB; the transport is restarted when the
     edits are saved.
   - Guition stays locked to BLE because the board has no USB device port.
2. Color theme
   - Rotates between blue-on-black and green-on-black. The change is previewed
     live and persisted on Save.
3. Buttons and axes
   - Opens the mapping sub-menu (see below).
4. BLE pairing reset
   - Removes the current BLE bond and restarts advertising so a new host can
     pair. Only meaningful in BLE mode; the action runs immediately.
5. Save / Cancel
   - Save writes every edit to NVS and closes the menu. Cancel discards the
     edits and closes the menu.

Mapping sub-menu rows:

- Eight `Zone N M-finger` choice rows, each rotating through the gamepad button
  bound to that tap (one- and two-finger taps edit separate bindings).
- `1-finger slide` and `2-finger slide` choice rows, each rotating that slide's
  joystick axis pair.
- Save returns to the main menu keeping the mapping edits; Cancel returns to the
  main menu discarding the mapping edits made in the sub-menu.

## Firmware architecture

- `main/main.c` runs the boot sequence and the touch event loop: it polls the
  touch controller, drives the buttons and the joystick from the live touch
  state every poll (press, hold and release in real time), and still tracks
  corner taps to detect the menu unlock sequence and to drive the menu.
- `main/touch_gamepad.c` / `main/touch_gamepad.h` hold the board preset table,
  the gesture-detection state machine, the menu state machine, and the NVS-backed
  configuration.
- `main/boards.h` centralizes the per-board display and touch pin assignments.
- `main/display.c` brings up the ST7701 RGB panel and the LVGL port.
- `main/touchpad.c` wraps the GT911 controller and returns up to two touch points.
- `main/ui.c` builds the LVGL screen: the tap zones that light up while pressed,
  the lower-half joystick surface with a central point and a live vector to the
  touch point, a status line, and the menu / mapping overlays.
- `main/gamepad_hid.h` defines the shared HID report descriptor and report
  struct used by both transports.
- `main/gamepad_backend.c` caches the logical gamepad state and dispatches it to
  the active transport.
- `main/ble_gamepad.c` implements the BLE HID gamepad (Bluedroid + esp_hid).
- `main/usb_gamepad.c` implements the USB HID gamepad (TinyUSB); it compiles to a
  stub on boards without USB.

## Building

Use ESP-IDF 6.x. The managed component dependencies (LVGL, esp_lvgl_port, the
ST7701 and GT911 drivers, and esp_tinyusb) are listed in
`main/idf_component.yml` and fetched automatically.

### Guition preset (BLE only)

```sh
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.guition" set-target esp32s3 build
```

### Waveshare preset (BLE or USB)

```sh
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.waveshare" set-target esp32s3 build
```

CMake presets are also provided (`guition` and `waveshare`) that layer the shared
`sdkconfig.defaults` with the matching board overlay.

## Hardware pin mapping

All display and touch pins live in `main/boards.h`. The Guition values come from
the community and ESPHome reference designs listed above. The Waveshare ESP32-S3-
Touch-LCD-4 pins, timing and ST7701 init sequence come from the official
Waveshare `esp32_s3_touch_lcd_4` BSP. That board routes the backlight and the
LCD / touch reset lines through an on-board I/O expander on the GT911 I2C bus;
`waveshare_board_bringup()` releases those reset lines and enables the backlight
before the display and touch controllers are initialized. Board revisions ship
with either the Waveshare CH32V003 (I2C address 0x24, default) or a TCA9554 (I2C
address 0x20); select the populated part under
`Touch Gamepad -> Waveshare I/O expander` in menuconfig.

## Persistence

Runtime configuration is stored in NVS under the `touchgp` namespace and survives
a reboot:

- selected transport mode
- selected color theme
- eight tap-to-button bindings
- one-finger slide axis pair
- two-finger slide axis pair

## ASCII-only rule

Every source and configuration file in this repository uses ASCII characters
only. This applies to C, header, CMake, Kconfig, YAML, JSON, and Markdown files.

## Validation status

This firmware targets physical ESP32-S3 hardware and the ESP-IDF 6.x build
system with the managed components above; it is intended to be built and flashed
with the commands in the Building section. Because the panel and touch pin maps
(especially for the Waveshare board) come from vendor references, confirm them on
the target hardware before relying on a build.

The Guition ESP32-S3-4848S040 preset is the validated target. The Waveshare
ESP32-S3-Touch-LCD-4 preset is **untested and unreliable**: it has not been
confirmed on hardware, and the reference unit fails to run even the official
Waveshare demos (a likely hardware fault), so its bring-up and transports should
be considered experimental.
