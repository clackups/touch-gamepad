/*
 * USB HID gamepad backend built on the esp_tinyusb managed component. It
 * registers a single gamepad HID interface using the shared report descriptor
 * and streams the cached report to the host.
 *
 * This backend is only compiled when the selected board exposes a USB device
 * port (see CONFIG_TOUCH_GAMEPAD_ENABLE_USB).
 *
 * ASCII only. See AGENTS.md.
 */
#include "usb_gamepad.h"

#if CONFIG_TOUCH_GAMEPAD_ENABLE_USB

#include <string.h>

#include "class/hid/hid_device.h"
#include "esp_log.h"
#include "tinyusb.h"
#include "tusb.h"

static const char *TAG = "usb_gamepad";

static const uint8_t s_hid_report_descriptor[] = { GAMEPAD_HID_REPORT_DESCRIPTOR };

static const char *s_string_descriptor[5] = {
    (char[]){ 0x09, 0x04 },  /* 0: language id (English) */
    "touch-gamepad",         /* 1: manufacturer */
    "Touch Gamepad",         /* 2: product */
    "0001",                  /* 3: serial */
    "Touch Gamepad HID",     /* 4: HID interface */
};

#define USB_GAMEPAD_ITF_COUNT 1
#define USB_GAMEPAD_EP_IN 0x81

static const uint8_t s_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, USB_GAMEPAD_ITF_COUNT, 0,
                          (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN), 0, 100),
    TUD_HID_DESCRIPTOR(0, 4, false, sizeof(s_hid_report_descriptor),
                       USB_GAMEPAD_EP_IN, 16, 10),
};

/* TinyUSB HID callbacks. */
const uint8_t *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return s_hid_report_descriptor;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)bufsize;
}

esp_err_t usb_gamepad_start(void)
{
    const tinyusb_config_t tusb_cfg = {
        .device_descriptor = NULL,
        .string_descriptor = s_string_descriptor,
        .string_descriptor_count = sizeof(s_string_descriptor) / sizeof(s_string_descriptor[0]),
        .external_phy = false,
        .configuration_descriptor = s_configuration_descriptor,
    };

    esp_err_t err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "USB HID gamepad started");
    return ESP_OK;
}

void usb_gamepad_stop(void)
{
    tinyusb_driver_uninstall();
    ESP_LOGI(TAG, "USB HID gamepad stopped");
}

bool usb_gamepad_connected(void)
{
    return tud_mounted() && tud_hid_ready();
}

void usb_gamepad_send(const gamepad_report_t *report)
{
    if (!tud_mounted() || !tud_hid_ready()) {
        return;
    }

    tud_hid_report(0, report, sizeof(*report));
}

#else /* !CONFIG_TOUCH_GAMEPAD_ENABLE_USB */

esp_err_t usb_gamepad_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void usb_gamepad_stop(void)
{
}

bool usb_gamepad_connected(void)
{
    return false;
}

void usb_gamepad_send(const gamepad_report_t *report)
{
    (void)report;
}

#endif /* CONFIG_TOUCH_GAMEPAD_ENABLE_USB */
