/*
 * Shared HID gamepad report definition used by both the BLE and USB backends.
 *
 * The report exposes 16 digital buttons and four signed 8-bit analog axes
 * (X, Y, Z, Rz). Tap bindings drive the buttons; slide bindings drive pairs of
 * axes. Keeping a single descriptor guarantees the host sees an identical
 * device regardless of the active transport.
 *
 * ASCII only. See AGENTS.md.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GAMEPAD_AXIS_COUNT 4
#define GAMEPAD_BUTTON_COUNT 16

/* Packed 5-byte input report: 16 buttons + 4 axes. */
typedef struct __attribute__((packed)) {
    uint16_t buttons;
    int8_t axes[GAMEPAD_AXIS_COUNT];
} gamepad_report_t;

/*
 * Standard USB HID gamepad report descriptor. It is transport agnostic and is
 * reused verbatim by the BLE HID service and the USB HID interface.
 */
#define GAMEPAD_HID_REPORT_DESCRIPTOR                                        \
    0x05, 0x01,       /* Usage Page (Generic Desktop) */                    \
    0x09, 0x05,       /* Usage (Game Pad) */                                \
    0xA1, 0x01,       /* Collection (Application) */                        \
    0xA1, 0x00,       /*   Collection (Physical) */                         \
    0x05, 0x09,       /*     Usage Page (Button) */                         \
    0x19, 0x01,       /*     Usage Minimum (Button 1) */                    \
    0x29, 0x10,       /*     Usage Maximum (Button 16) */                   \
    0x15, 0x00,       /*     Logical Minimum (0) */                         \
    0x25, 0x01,       /*     Logical Maximum (1) */                         \
    0x75, 0x01,       /*     Report Size (1) */                             \
    0x95, 0x10,       /*     Report Count (16) */                           \
    0x81, 0x02,       /*     Input (Data,Var,Abs) */                        \
    0x05, 0x01,       /*     Usage Page (Generic Desktop) */                \
    0x09, 0x30,       /*     Usage (X) */                                   \
    0x09, 0x31,       /*     Usage (Y) */                                   \
    0x09, 0x32,       /*     Usage (Z) */                                   \
    0x09, 0x35,       /*     Usage (Rz) */                                  \
    0x15, 0x81,       /*     Logical Minimum (-127) */                      \
    0x25, 0x7F,       /*     Logical Maximum (127) */                       \
    0x75, 0x08,       /*     Report Size (8) */                             \
    0x95, 0x04,       /*     Report Count (4) */                            \
    0x81, 0x02,       /*     Input (Data,Var,Abs) */                        \
    0xC0,             /*   End Collection */                                \
    0xC0              /* End Collection */

#ifdef __cplusplus
}
#endif
