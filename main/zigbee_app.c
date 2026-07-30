#include "zigbee_app.h"

#include <inttypes.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_zigbee_cluster.h"
#include "esp_zigbee_core.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nwk/esp_zigbee_nwk.h"
#include "zcl/esp_zigbee_zcl_common.h"
#include "zcl/esp_zigbee_zcl_command.h"
#include "zcl/esp_zigbee_zcl_metering.h"

#include "meter_math.h"
#include "meter_state.h"
#include "ota.h"
#include "sleep_control.h"
#include "zigbee_clusters.h"

#define REPORT_INTERVAL_MS (10 * 60 * 1000)
#define FIRST_REPORT_DELAY_MS 10000
#define JOIN_RETRY_INTERVAL_MS 5000
#define JOIN_RETRY_TIMEOUT_MS (5 * 60 * 1000)
#define ZIGBEE_KEEP_ALIVE_MS 3000
#define ZIGBEE_DEEP_AWAKE_AFTER_PULSE_MS CONFIG_WATERMETER_PULSE_GRACE_MS
#define ESP_ZB_PRIMARY_CHANNEL_MASK ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK

static const char *TAG = "ZIGBEE_APP";

typedef struct {
    bool ready;
    bool joined;
    int64_t join_retry_deadline_us;
} zigbee_runtime_t;

static zigbee_runtime_t s_zigbee;

static void bdb_start_top_level_commissioning_cb(uint8_t mode_mask);

static void update_meter_attrs(bool send_report, bool include_battery)
{
    if (!s_zigbee.ready) {
        return;
    }
    zigbee_clusters_update_meter(send_report, include_battery);
}

static void periodic_report_cb(uint8_t arg)
{
    (void)arg;
    update_meter_attrs(true, true);
#if CONFIG_WATERMETER_SLEEP_MODE_DEEP
    sleep_control_schedule_after_report(ZIGBEE_DEEP_AWAKE_AFTER_PULSE_MS);
#else
    esp_zb_scheduler_alarm((esp_zb_callback_t)periodic_report_cb, 0, REPORT_INTERVAL_MS);
#endif
}

static void schedule_first_report(void)
{
    ESP_LOGI(TAG, "Scheduling first report in %d ms", FIRST_REPORT_DELAY_MS);
    esp_zb_scheduler_alarm((esp_zb_callback_t)periodic_report_cb, 0, FIRST_REPORT_DELAY_MS);
}

static esp_err_t attribute_handler(const esp_zb_zcl_set_attr_value_message_t *message)
{
    if (!message || message->info.status != ESP_ZB_ZCL_STATUS_SUCCESS) {
        return ESP_OK;
    }
    if (message->info.dst_endpoint != WATERMETER_ENDPOINT ||
        message->info.cluster != ESP_ZB_ZCL_CLUSTER_ID_METERING ||
        !message->attribute.data.value) {
        return ESP_OK;
    }

    uint32_t value = 0;
    if (message->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_U24) {
        value = zb_u24_to_uint32((const esp_zb_uint24_t *)message->attribute.data.value);
    } else if (message->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_U32) {
        value = *(const uint32_t *)message->attribute.data.value;
    } else {
        return ESP_OK;
    }

    bool changed = false;
    if ((message->attribute.id == ESP_ZB_ZCL_ATTR_METERING_MULTIPLIER_ID ||
         message->attribute.id == ATTR_SCALE_MULTIPLIER_ID) &&
        value > 0) {
        meter_state_set_multiplier(value);
        changed = true;
    } else if ((message->attribute.id == ESP_ZB_ZCL_ATTR_METERING_DIVISOR_ID ||
                message->attribute.id == ATTR_SCALE_DIVISOR_ID) &&
               value > 0) {
        meter_state_set_divisor(value);
        changed = true;
    }

    if (!changed) {
        ESP_LOGW(TAG, "Ignored metering attr 0x%04x value=%" PRIu32, message->attribute.id, value);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Zigbee config write: attr=0x%04x value=%" PRIu32, message->attribute.id, value);
    esp_err_t err = meter_state_save();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save Zigbee config: %s", esp_err_to_name(err));
    }
    update_meter_attrs(true, true);
    return ESP_OK;
}

static esp_err_t action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message)
{
    switch (callback_id) {
    case ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID:
        return attribute_handler((const esp_zb_zcl_set_attr_value_message_t *)message);
    case ESP_ZB_CORE_OTA_UPGRADE_VALUE_CB_ID:
        return ota_upgrade_handler((esp_zb_zcl_ota_upgrade_value_message_t *)message);
    default:
        ESP_LOGD(TAG, "Zigbee action callback: 0x%x", callback_id);
        return ESP_OK;
    }
}

static void schedule_join_retry(void)
{
    if (s_zigbee.joined) {
        return;
    }

    int64_t now_us = esp_timer_get_time();
    if (s_zigbee.join_retry_deadline_us == 0) {
        s_zigbee.join_retry_deadline_us = now_us + ((int64_t)JOIN_RETRY_TIMEOUT_MS * 1000);
    }
    if (now_us >= s_zigbee.join_retry_deadline_us) {
        ESP_LOGW(TAG, "Zigbee join retry timeout reached, stop retrying");
        s_zigbee.join_retry_deadline_us = 0;
        return;
    }

    esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_top_level_commissioning_cb,
                           ESP_ZB_BDB_MODE_NETWORK_STEERING,
                           JOIN_RETRY_INTERVAL_MS);
}

static void bdb_start_top_level_commissioning_cb(uint8_t mode_mask)
{
    if (s_zigbee.joined) {
        return;
    }

    esp_err_t err = esp_zb_bdb_start_top_level_commissioning(mode_mask);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to start commissioning mode 0x%x: %s", mode_mask, esp_err_to_name(err));
        schedule_join_retry();
    }
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    uint32_t *signal = signal_struct->p_app_signal;
    esp_err_t status = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t signal_type = *signal;

    switch (signal_type) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Zigbee Stack Initialized. Starting Commissioning...");
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
        break;
    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (status != ESP_OK) {
            ESP_LOGW(TAG, "Failed to initialize Zigbee stack (status: %s)", esp_err_to_name(status));
            ESP_LOGW(TAG, "Scheduling network steering retries for %d seconds", JOIN_RETRY_TIMEOUT_MS / 1000);
            schedule_join_retry();
            return;
        }

        s_zigbee.ready = true;
        ESP_LOGI(TAG, "Device started up in %s factory-reset mode", esp_zb_bdb_is_factory_new() ? "" : "non");
        if (esp_zb_bdb_is_factory_new()) {
            ESP_LOGI(TAG, "Start network steering");
            bdb_start_top_level_commissioning_cb(ESP_ZB_BDB_MODE_NETWORK_STEERING);
        } else {
            s_zigbee.joined = true;
            sleep_control_set_joined(true);
            ESP_LOGI(TAG, "Device rebooted");
            schedule_first_report();
        }
        break;
    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (status == ESP_OK) {
            s_zigbee.joined = true;
            sleep_control_set_joined(true);
            s_zigbee.join_retry_deadline_us = 0;
            ESP_LOGI(TAG, "Joined network successfully");
            schedule_first_report();
        } else {
            ESP_LOGW(TAG, "Network steering failed, retrying in %d ms", JOIN_RETRY_INTERVAL_MS);
            schedule_join_retry();
        }
        break;
    case ESP_ZB_COMMON_SIGNAL_CAN_SLEEP:
        sleep_control_handle_can_sleep(signal, status);
        break;
    default:
        ESP_LOGI(TAG, "ZDO signal: %s (0x%x)", esp_zb_zdo_signal_to_string(signal_type), signal_type);
        break;
    }
}

static void zigbee_task(void *pvParameters)
{
    (void)pvParameters;

    esp_zb_cfg_t network_config = {
        .esp_zb_role = ESP_ZB_DEVICE_TYPE_ED,
        .install_code_policy = false,
        .nwk_cfg.zed_cfg = {
            .ed_timeout = ESP_ZB_ED_AGING_TIMEOUT_64MIN,
            .keep_alive = ZIGBEE_KEEP_ALIVE_MS,
        },
    };
    esp_zb_init(&network_config);
    sleep_control_configure_zigbee();

    esp_zb_ep_list_t *endpoint_list = zigbee_clusters_create_endpoint();
    esp_zb_device_register(endpoint_list);
    esp_zb_core_action_handler_register(action_handler);
    esp_zb_set_primary_network_channel_set(ESP_ZB_PRIMARY_CHANNEL_MASK);
    ESP_ERROR_CHECK(esp_zb_start(false));
    esp_zb_set_node_descriptor_power_source(false);
    esp_zb_stack_main_loop();
}

bool zigbee_app_is_joined(void)
{
    return s_zigbee.joined;
}

void zigbee_app_report_sensor_pulse(void)
{
    update_meter_attrs(true, false);
}

esp_err_t zigbee_app_start(void)
{
    return xTaskCreate(zigbee_task, "Zigbee_main", 4096, NULL, 5, NULL) == pdPASS ? ESP_OK : ESP_FAIL;
}
