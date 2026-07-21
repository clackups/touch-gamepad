#include "touch_gamepad.h"

#include <string.h>

#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "app_main";

static void log_menu_capabilities(const touch_gamepad_board_preset_t *preset)
{
    ESP_LOGI(TAG, "Menu sequence: lower-left, upper-left, upper-right, lower-right");
    ESP_LOGI(TAG, "Menu item 1: %s%s",
             touch_gamepad_menu_item_name(TOUCH_GAMEPAD_MENU_ITEM_TRANSPORT),
             preset->supports_usb ? "" : " (fixed to BLE for this board)");
    ESP_LOGI(TAG, "Menu item 2: %s", touch_gamepad_menu_item_name(TOUCH_GAMEPAD_MENU_ITEM_REPAIR));
    ESP_LOGI(TAG, "Menu item 3: %s", touch_gamepad_menu_item_name(TOUCH_GAMEPAD_MENU_ITEM_MAPPING));
    ESP_LOGI(TAG, "Menu item 4: %s", touch_gamepad_menu_item_name(TOUCH_GAMEPAD_MENU_ITEM_THEME));
}

void app_main(void)
{
    touch_gamepad_config_t config;
    touch_gamepad_menu_state_t menu_state;
    const touch_gamepad_board_preset_t *preset = touch_gamepad_get_board_preset();
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(touch_gamepad_config_load(&config));
    touch_gamepad_menu_init(&menu_state);

    ESP_LOGI(TAG, "Board preset: %s", preset->board_name);
    ESP_LOGI(TAG, "Display: %ux%u", preset->screen_width, preset->screen_height);
    ESP_LOGI(TAG, "USB capable: %s", preset->supports_usb ? "yes" : "no");
    ESP_LOGI(TAG, "Upper half: four tap zones with one-finger and two-finger bindings");
    ESP_LOGI(TAG, "Lower half: one-finger and two-finger slides for joystick axes");
    log_menu_capabilities(preset);
    touch_gamepad_log_configuration(&config);

    if (!preset->supports_usb && config.transport_mode == TOUCH_GAMEPAD_TRANSPORT_USB) {
        config.transport_mode = TOUCH_GAMEPAD_TRANSPORT_BLE;
        ESP_ERROR_CHECK(touch_gamepad_config_save(&config));
    }

    ESP_LOGI(TAG,
             "Hardware-specific display, touch, BLE HID, and USB HID drivers can now consume this preset and gesture model.");
}
