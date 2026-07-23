/*
 * BLE HID gamepad backend built on the Bluedroid stack and the esp_hid device
 * role. It advertises a single gamepad report map, accepts a bonded host, and
 * can drop the current bond to allow re-pairing from the configuration menu.
 *
 * The GAP/advertising handling mirrors the official esp_hid_device example but
 * is trimmed to the single gamepad use case handled here.
 *
 * ASCII only. See AGENTS.md.
 */
#include "ble_gamepad.h"

#include <stdlib.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_bt_defs.h"
#include "esp_bt_main.h"
#include "esp_event.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_hidd.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "ble_gamepad";

static const uint8_t s_report_map[] = { GAMEPAD_HID_REPORT_DESCRIPTOR };

static esp_hidd_dev_t *s_hid_dev = NULL;
static volatile bool s_connected = false;
static volatile bool s_started = false;

/* Single extended advertising set used for the gamepad. The ESP32-S3 controller
 * only builds the BLE 5.0 feature set, so advertising uses the extended
 * advertising API. Legacy advertising PDUs are still emitted (LEGACY_IND) so
 * that hosts scanning with a legacy scanner can discover the gamepad. */
#define BLE_GAMEPAD_ADV_INSTANCE 0
#define BLE_GAMEPAD_ADV_SET_COUNT 1

/* Raw advertising payload: flags, gamepad appearance (0x03C4), the HID service
 * UUID (0x1812), and the complete local name. */
static uint8_t s_ext_adv_raw_data[] = {
    0x02, ESP_BLE_AD_TYPE_FLAG, 0x06,
    0x03, ESP_BLE_AD_TYPE_APPEARANCE, 0xC4, 0x03,
    0x03, ESP_BLE_AD_TYPE_16SRV_CMPL, 0x12, 0x18,
    0x0E, ESP_BLE_AD_TYPE_NAME_CMPL, 'T', 'o', 'u', 'c', 'h', ' ', 'G', 'a', 'm', 'e', 'p', 'a', 'd',
};

static esp_ble_gap_ext_adv_params_t s_ext_adv_params = {
    .type = ESP_BLE_GAP_SET_EXT_ADV_PROP_LEGACY_IND,
    .interval_min = 0x20,
    .interval_max = 0x30,
    .channel_map = ADV_CHNL_ALL,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
    .primary_phy = ESP_BLE_GAP_PHY_1M,
    .max_skip = 0,
    .secondary_phy = ESP_BLE_GAP_PHY_1M,
    .sid = 0,
    .scan_req_notif = false,
    .tx_power = EXT_ADV_TX_PWR_NO_PREFERENCE,
};

static esp_ble_gap_ext_adv_t s_ext_adv[BLE_GAMEPAD_ADV_SET_COUNT] = {
    { .instance = BLE_GAMEPAD_ADV_INSTANCE, .duration = 0, .max_events = 0 },
};

/*
 * Kick off (or restart) advertising by re-running the full extended
 * advertising configuration chain:
 *   set_params -> ESP_GAP_BLE_EXT_ADV_SET_PARAMS_COMPLETE_EVT
 *             -> config_ext_adv_data_raw -> ESP_GAP_BLE_EXT_ADV_DATA_SET_COMPLETE_EVT
 *             -> ext_adv_start
 * Always starting from set_params guarantees the controller has an advertising
 * set registered for the instance before ext_adv_start is issued. Calling
 * ext_adv_start (or ext_adv_stop) on an instance that was never registered
 * makes the controller reject the HCI LE Set Extended Advertising Enable
 * command with error 0x42 ("Unknown Advertising Identifier"), which is exactly
 * what happened when re-pairing issued a bare stop/start pair.
 */
static esp_err_t ble_gamepad_start_advertising(void)
{
    esp_err_t err = esp_ble_gap_ext_adv_set_params(BLE_GAMEPAD_ADV_INSTANCE, &s_ext_adv_params);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ext_adv_set_params failed: %s", esp_err_to_name(err));
    }
    return err;
}

static void ble_gamepad_gap_event(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_EXT_ADV_SET_PARAMS_COMPLETE_EVT:
        if (param->ext_adv_set_params.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "Ext adv params set failed, status 0x%x",
                     param->ext_adv_set_params.status);
            break;
        }
        esp_ble_gap_config_ext_adv_data_raw(BLE_GAMEPAD_ADV_INSTANCE, sizeof(s_ext_adv_raw_data),
                                            s_ext_adv_raw_data);
        break;
    case ESP_GAP_BLE_EXT_ADV_DATA_SET_COMPLETE_EVT:
        if (param->ext_adv_data_set.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "Ext adv data set failed, status 0x%x",
                     param->ext_adv_data_set.status);
            break;
        }
        esp_ble_gap_ext_adv_start(BLE_GAMEPAD_ADV_SET_COUNT, s_ext_adv);
        break;
    case ESP_GAP_BLE_EXT_ADV_START_COMPLETE_EVT:
        if (param->ext_adv_start.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "Ext adv start failed, status 0x%x", param->ext_adv_start.status);
        } else {
            ESP_LOGI(TAG, "Advertising as \"Touch Gamepad\"");
        }
        break;
    case ESP_GAP_BLE_SEC_REQ_EVT:
        esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
        break;
    case ESP_GAP_BLE_AUTH_CMPL_EVT:
        if (param->ble_security.auth_cmpl.success) {
            ESP_LOGI(TAG, "Bonding complete");
        } else {
            ESP_LOGW(TAG, "Bonding failed, reason 0x%x", param->ble_security.auth_cmpl.fail_reason);
        }
        break;
    default:
        break;
    }
}

static void ble_gamepad_hidd_event(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    esp_hidd_event_t event = (esp_hidd_event_t)id;

    switch (event) {
    case ESP_HIDD_START_EVENT:
        esp_ble_gap_set_device_name("Touch Gamepad");
        ble_gamepad_start_advertising();
        break;
    case ESP_HIDD_CONNECT_EVENT:
        s_connected = true;
        ESP_LOGI(TAG, "Host connected");
        break;
    case ESP_HIDD_DISCONNECT_EVENT:
        s_connected = false;
        ESP_LOGI(TAG, "Host disconnected, advertising again");
        ble_gamepad_start_advertising();
        break;
    default:
        break;
    }
}

static void ble_gamepad_configure_security(void)
{
    esp_ble_auth_req_t auth_req = ESP_LE_AUTH_REQ_SC_BOND;
    esp_ble_io_cap_t io_cap = ESP_IO_CAP_NONE;
    uint8_t key_size = 16;
    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;

    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(auth_req));
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &io_cap, sizeof(io_cap));
    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(key_size));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(init_key));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(rsp_key));
}

esp_err_t ble_gamepad_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    esp_err_t err = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    err = esp_bt_controller_init(&bt_cfg);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_bluedroid_init();
    if (err != ESP_OK) {
        return err;
    }

    err = esp_bluedroid_enable();
    if (err != ESP_OK) {
        return err;
    }

    err = esp_ble_gap_register_callback(ble_gamepad_gap_event);
    if (err != ESP_OK) {
        return err;
    }

    ble_gamepad_configure_security();

    /*
     * Route Bluedroid GATTS events to the esp_hid device layer. Without this
     * callback the HID GATT service (report map, HID info, input reports) is
     * never created, so a host can bond over GAP but never discovers a HID
     * device: it "pairs but does nothing". This must be registered before
     * esp_hidd_dev_init, which registers the GATTS applications whose
     * ESP_GATTS_REG_EVT drives the attribute-table creation.
     */
    err = esp_ble_gatts_register_callback(esp_hidd_gatts_event_handler);
    if (err != ESP_OK) {
        return err;
    }

    static esp_hid_raw_report_map_t report_maps[1];
    report_maps[0].data = s_report_map;
    report_maps[0].len = sizeof(s_report_map);

    static esp_hid_device_config_t hid_config;
    memset(&hid_config, 0, sizeof(hid_config));
    hid_config.vendor_id = 0x303A;  /* Espressif */
    hid_config.product_id = 0x4001;
    hid_config.version = 0x0100;
    hid_config.device_name = "Touch Gamepad";
    hid_config.manufacturer_name = "touch-gamepad";
    hid_config.serial_number = "0001";
    hid_config.report_maps = report_maps;
    hid_config.report_maps_len = 1;

    err = esp_hidd_dev_init(&hid_config, ESP_HID_TRANSPORT_BLE, ble_gamepad_hidd_event, &s_hid_dev);
    if (err != ESP_OK) {
        return err;
    }

    s_started = true;
    ESP_LOGI(TAG, "BLE HID gamepad started");
    return ESP_OK;
}

void ble_gamepad_stop(void)
{
    if (!s_started) {
        return;
    }

    if (s_hid_dev != NULL) {
        esp_hidd_dev_deinit(s_hid_dev);
        s_hid_dev = NULL;
    }

    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();

    s_connected = false;
    s_started = false;
    ESP_LOGI(TAG, "BLE HID gamepad stopped");
}

bool ble_gamepad_connected(void)
{
    return s_connected;
}

void ble_gamepad_send(const gamepad_report_t *report)
{
    if (!s_connected || s_hid_dev == NULL) {
        return;
    }

    esp_hidd_dev_input_set(s_hid_dev, 0, 0, (uint8_t *)report, sizeof(*report));
}

static void ble_gamepad_remove_all_bonds(void)
{
    int count = esp_ble_get_bond_device_num();
    if (count <= 0) {
        return;
    }

    esp_ble_bond_dev_t *devices = calloc((size_t)count, sizeof(esp_ble_bond_dev_t));
    if (devices == NULL) {
        return;
    }

    if (esp_ble_get_bond_device_list(&count, devices) == ESP_OK) {
        for (int i = 0; i < count; ++i) {
            esp_ble_remove_bond_device(devices[i].bd_addr);
        }
    }

    free(devices);
}

esp_err_t ble_gamepad_restart_pairing(void)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Clearing bonds and restarting advertising for re-pairing");
    ble_gamepad_remove_all_bonds();

    /*
     * Re-run the whole advertising configuration chain instead of a bare
     * stop/start on the advertising handle. After a bond is dropped the
     * controller may no longer hold the advertising set, and issuing the HCI
     * enable command against an unknown handle fails with error 0x42
     * ("Unknown Advertising Identifier"). Re-registering the set with
     * esp_ble_gap_ext_adv_set_params first (which implicitly disables any
     * ongoing advertising for the instance) always leaves a valid set for the
     * subsequent ext_adv_start.
     */
    return ble_gamepad_start_advertising();
}
