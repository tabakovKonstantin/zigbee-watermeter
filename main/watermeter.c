#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_zigbee_cluster.h"
#include "esp_zigbee_core.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "nwk/esp_zigbee_nwk.h"
#include "zcl/esp_zigbee_zcl_basic.h"
#include "zcl/esp_zigbee_zcl_common.h"
#include "zcl/esp_zigbee_zcl_command.h"
#include "zcl/esp_zigbee_zcl_metering.h"
#include "zcl/esp_zigbee_zcl_power_config.h"

#define SENSOR_PIN GPIO_NUM_4
#define BATTERY_ADC_CHANNEL ADC_CHANNEL_5
#define HA_ENDPOINT 1

#define ESP_MANUFACTURER_NAME "ZigbeeHive"
#define ESP_MODEL_IDENTIFIER "WaterMeter"

#define ENABLE_LIGHT_SLEEP 0
#define DEFAULT_MULTIPLIER 10
#define DEFAULT_DIVISOR 1
#define REPORT_INTERVAL_MS (10 * 60 * 1000)
#define DEBOUNCE_US (100 * 1000)
#define SENSOR_QUEUE_LEN 8
#define ESP_ZB_PRIMARY_CHANNEL_MASK ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK

#define NVS_NAMESPACE "watermeter"
#define NVS_KEY_PULSE_COUNT "pulse_count"
#define NVS_KEY_MULTIPLIER "multiplier"
#define NVS_KEY_DIVISOR "divisor"

#define MANUFACTURER_CODE 0x131B
#define ATTR_SCALED_SUMMATION_ID 0xFC00
#define ATTR_SCALE_MULTIPLIER_ID 0xFC01
#define ATTR_SCALE_DIVISOR_ID 0xFC02

static const char *TAG = "ZIGBEE_METER";

typedef struct {
    uint64_t pulse_count;
    uint32_t multiplier;
    uint32_t divisor;
} meter_state_t;

static meter_state_t s_meter = {
    .pulse_count = 0,
    .multiplier = DEFAULT_MULTIPLIER,
    .divisor = DEFAULT_DIVISOR,
};

static portMUX_TYPE s_meter_mux = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t s_sensor_queue;
static nvs_handle_t s_nvs;
static bool s_zigbee_ready;

static adc_oneshot_unit_handle_t s_adc_handle;
static uint8_t s_battery_voltage_zcl;
static uint8_t s_battery_percent_zcl;

static esp_zb_uint48_t s_current_summation_attr;
static esp_zb_uint48_t s_scaled_summation_attr;
static esp_zb_uint24_t s_multiplier_attr;
static esp_zb_uint24_t s_divisor_attr;
static uint32_t s_scale_multiplier_attr;
static uint32_t s_scale_divisor_attr;

static esp_zb_uint24_t uint32_to_zb_u24(uint32_t value)
{
    esp_zb_uint24_t out = {
        .low = (uint16_t)(value & 0xFFFF),
        .high = (uint8_t)((value >> 16) & 0xFF),
    };
    return out;
}

static uint32_t zb_u24_to_uint32(const esp_zb_uint24_t *value)
{
    return ((uint32_t)value->high << 16) | value->low;
}

static esp_zb_uint48_t uint64_to_zb_u48(uint64_t value)
{
    esp_zb_uint48_t out = {
        .low = (uint32_t)(value & 0xFFFFFFFFULL),
        .high = (uint16_t)((value >> 32) & 0xFFFF),
    };
    return out;
}

static uint64_t meter_scaled_summation(uint64_t pulse_count, uint32_t multiplier, uint32_t divisor)
{
    if (divisor == 0) {
        divisor = 1;
    }
    return (pulse_count * multiplier) / divisor;
}

static uint8_t battery_percent_from_mv(uint32_t battery_mv)
{
    if (battery_mv >= 4500) {
        return 100;
    }
    if (battery_mv <= 3300) {
        return 0;
    }
    return (uint8_t)(((battery_mv - 3300) * 100) / (4500 - 3300));
}

static esp_err_t meter_state_save(void)
{
    ESP_RETURN_ON_ERROR(nvs_set_u64(s_nvs, NVS_KEY_PULSE_COUNT, s_meter.pulse_count), TAG, "save pulse_count");
    ESP_RETURN_ON_ERROR(nvs_set_u32(s_nvs, NVS_KEY_MULTIPLIER, s_meter.multiplier), TAG, "save multiplier");
    ESP_RETURN_ON_ERROR(nvs_set_u32(s_nvs, NVS_KEY_DIVISOR, s_meter.divisor), TAG, "save divisor");
    return nvs_commit(s_nvs);
}

static esp_err_t meter_state_load(void)
{
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s_nvs);
    ESP_RETURN_ON_ERROR(err, TAG, "open meter nvs");

    uint64_t pulse_count = 0;
    uint32_t multiplier = DEFAULT_MULTIPLIER;
    uint32_t divisor = DEFAULT_DIVISOR;

    err = nvs_get_u64(s_nvs, NVS_KEY_PULSE_COUNT, &pulse_count);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        return err;
    }

    err = nvs_get_u32(s_nvs, NVS_KEY_MULTIPLIER, &multiplier);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        return err;
    }

    err = nvs_get_u32(s_nvs, NVS_KEY_DIVISOR, &divisor);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        return err;
    }

    if (multiplier == 0) {
        multiplier = DEFAULT_MULTIPLIER;
    }
    if (divisor == 0) {
        divisor = DEFAULT_DIVISOR;
    }

    portENTER_CRITICAL(&s_meter_mux);
    s_meter.pulse_count = pulse_count;
    s_meter.multiplier = multiplier;
    s_meter.divisor = divisor;
    portEXIT_CRITICAL(&s_meter_mux);

    ESP_LOGI(TAG, "Loaded state: pulses=%" PRIu64 " multiplier=%" PRIu32 " divisor=%" PRIu32,
             pulse_count, multiplier, divisor);

    return meter_state_save();
}

static void meter_state_snapshot(meter_state_t *out)
{
    portENTER_CRITICAL(&s_meter_mux);
    *out = s_meter;
    portEXIT_CRITICAL(&s_meter_mux);
}

static uint32_t battery_read_mv(void)
{
    int raw = 0;
    if (!s_adc_handle || adc_oneshot_read(s_adc_handle, BATTERY_ADC_CHANNEL, &raw) != ESP_OK) {
        return 0;
    }

    uint32_t adc_mv = ((uint32_t)raw * 3300U) / 4095U;
    return adc_mv * 2U;
}

static void battery_update_attr(void)
{
    uint32_t battery_mv = battery_read_mv();
    if (battery_mv == 0) {
        return;
    }

    uint8_t percent = battery_percent_from_mv(battery_mv);
    s_battery_voltage_zcl = (uint8_t)((battery_mv + 50U) / 100U);
    s_battery_percent_zcl = (uint8_t)(percent * 2U);

    esp_zb_zcl_set_attribute_val(HA_ENDPOINT,
                                 ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_ID,
                                 &s_battery_voltage_zcl,
                                 false);

    esp_zb_zcl_set_attribute_val(HA_ENDPOINT,
                                 ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID,
                                 &s_battery_percent_zcl,
                                 false);

    ESP_LOGI(TAG, "Battery: %" PRIu32 "mV %u%%", battery_mv, percent);
}

static void report_attr(uint16_t cluster_id, uint16_t attr_id, bool manufacturer_specific)
{
    esp_zb_zcl_report_attr_cmd_t cmd = {
        .zcl_basic_cmd = {
            .src_endpoint = HA_ENDPOINT,
            .dst_endpoint = HA_ENDPOINT,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_DST_ADDR_ENDP_NOT_PRESENT,
        .clusterID = cluster_id,
        .manuf_specific = manufacturer_specific ? 1 : 0,
        .direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI,
        .dis_default_resp = 1,
        .manuf_code = manufacturer_specific ? MANUFACTURER_CODE : 0,
        .attributeID = attr_id,
    };
    esp_err_t err = esp_zb_zcl_report_attr_cmd_req(&cmd);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Report attr 0x%04x/0x%04x failed: %s", cluster_id, attr_id, esp_err_to_name(err));
    }
}

static void meter_update_zigbee_attrs(bool send_report)
{
    if (!s_zigbee_ready) {
        return;
    }

    meter_state_t snap;
    meter_state_snapshot(&snap);

    uint64_t scaled = meter_scaled_summation(snap.pulse_count, snap.multiplier, snap.divisor);
    s_current_summation_attr = uint64_to_zb_u48(snap.pulse_count);
    s_scaled_summation_attr = uint64_to_zb_u48(scaled);
    s_multiplier_attr = uint32_to_zb_u24(snap.multiplier);
    s_divisor_attr = uint32_to_zb_u24(snap.divisor);
    s_scale_multiplier_attr = snap.multiplier;
    s_scale_divisor_attr = snap.divisor;

    esp_zb_lock_acquire(portMAX_DELAY);

    esp_zb_zcl_set_attribute_val(HA_ENDPOINT,
                                 ESP_ZB_ZCL_CLUSTER_ID_METERING,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 ESP_ZB_ZCL_ATTR_METERING_CURRENT_SUMMATION_DELIVERED_ID,
                                 &s_current_summation_attr,
                                 false);
    esp_zb_zcl_set_attribute_val(HA_ENDPOINT,
                                 ESP_ZB_ZCL_CLUSTER_ID_METERING,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 ESP_ZB_ZCL_ATTR_METERING_MULTIPLIER_ID,
                                 &s_multiplier_attr,
                                 false);
    esp_zb_zcl_set_attribute_val(HA_ENDPOINT,
                                 ESP_ZB_ZCL_CLUSTER_ID_METERING,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 ESP_ZB_ZCL_ATTR_METERING_DIVISOR_ID,
                                 &s_divisor_attr,
                                 false);
    esp_zb_zcl_set_attribute_val(HA_ENDPOINT,
                                 ESP_ZB_ZCL_CLUSTER_ID_METERING,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 ATTR_SCALED_SUMMATION_ID,
                                 &s_scaled_summation_attr,
                                 false);
    esp_zb_zcl_set_attribute_val(HA_ENDPOINT,
                                 ESP_ZB_ZCL_CLUSTER_ID_METERING,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 ATTR_SCALE_MULTIPLIER_ID,
                                 &s_scale_multiplier_attr,
                                 false);
    esp_zb_zcl_set_attribute_val(HA_ENDPOINT,
                                 ESP_ZB_ZCL_CLUSTER_ID_METERING,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 ATTR_SCALE_DIVISOR_ID,
                                 &s_scale_divisor_attr,
                                 false);

    battery_update_attr();

    if (send_report) {
        report_attr(ESP_ZB_ZCL_CLUSTER_ID_METERING,
                    ESP_ZB_ZCL_ATTR_METERING_CURRENT_SUMMATION_DELIVERED_ID,
                    false);
        report_attr(ESP_ZB_ZCL_CLUSTER_ID_METERING, ATTR_SCALED_SUMMATION_ID, false);
        report_attr(ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
                    ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_ID,
                    false);
        report_attr(ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
                    ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID,
                    false);
    }

    esp_zb_lock_release();

    ESP_LOGI(TAG, "Meter update: pulses=%" PRIu64 " multiplier=%" PRIu32 " divisor=%" PRIu32 " scaled=%" PRIu64,
             snap.pulse_count, snap.multiplier, snap.divisor, scaled);
}

static void periodic_report_cb(uint8_t arg)
{
    meter_update_zigbee_attrs(true);
    esp_zb_scheduler_alarm((esp_zb_callback_t)periodic_report_cb, 0, REPORT_INTERVAL_MS);
}

static void IRAM_ATTR sensor_isr_handler(void *arg)
{
    uint32_t gpio_num = (uint32_t)arg;
    BaseType_t higher_priority_task_woken = pdFALSE;
    xQueueSendFromISR(s_sensor_queue, &gpio_num, &higher_priority_task_woken);
    if (higher_priority_task_woken) {
        portYIELD_FROM_ISR();
    }
}

static void sensor_task(void *pvParameters)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << SENSOR_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(SENSOR_PIN, sensor_isr_handler, (void *)SENSOR_PIN));

    ESP_LOGI(TAG, "Sensor interrupt started on GPIO%d", SENSOR_PIN);

    uint32_t gpio_num = 0;
    int64_t last_pulse_us = 0;
    while (1) {
        if (xQueueReceive(s_sensor_queue, &gpio_num, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        int64_t now_us = esp_timer_get_time();
        if ((now_us - last_pulse_us) < DEBOUNCE_US) {
            continue;
        }
        last_pulse_us = now_us;

        portENTER_CRITICAL(&s_meter_mux);
        s_meter.pulse_count++;
        uint64_t pulses = s_meter.pulse_count;
        portEXIT_CRITICAL(&s_meter_mux);

        esp_err_t err = meter_state_save();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save pulse count: %s", esp_err_to_name(err));
        }

        ESP_LOGI(TAG, "Pulse counted on GPIO%" PRIu32 ", total=%" PRIu64, gpio_num, pulses);
        meter_update_zigbee_attrs(true);

#if ENABLE_LIGHT_SLEEP
        ESP_LOGD(TAG, "Light sleep placeholder is enabled but not active in this debug build");
#endif
    }
}

static esp_err_t adc_init(void)
{
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT_1,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&init_config, &s_adc_handle), TAG, "adc unit");

    adc_oneshot_chan_cfg_t chan_config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_12,
    };
    return adc_oneshot_config_channel(s_adc_handle, BATTERY_ADC_CHANNEL, &chan_config);
}

static esp_err_t zb_attribute_handler(const esp_zb_zcl_set_attr_value_message_t *message)
{
    if (!message || message->info.status != ESP_ZB_ZCL_STATUS_SUCCESS) {
        return ESP_OK;
    }

    if (message->info.dst_endpoint != HA_ENDPOINT ||
        message->info.cluster != ESP_ZB_ZCL_CLUSTER_ID_METERING) {
        return ESP_OK;
    }

    if (!message->attribute.data.value) {
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

    portENTER_CRITICAL(&s_meter_mux);
    if ((message->attribute.id == ESP_ZB_ZCL_ATTR_METERING_MULTIPLIER_ID ||
         message->attribute.id == ATTR_SCALE_MULTIPLIER_ID) &&
        value > 0) {
        s_meter.multiplier = value;
        changed = true;
    } else if ((message->attribute.id == ESP_ZB_ZCL_ATTR_METERING_DIVISOR_ID ||
                message->attribute.id == ATTR_SCALE_DIVISOR_ID) &&
               value > 0) {
        s_meter.divisor = value;
        changed = true;
    }
    portEXIT_CRITICAL(&s_meter_mux);

    if (!changed) {
        ESP_LOGW(TAG, "Ignored metering attr 0x%04x value=%" PRIu32, message->attribute.id, value);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Zigbee config write: attr=0x%04x value=%" PRIu32, message->attribute.id, value);
    esp_err_t err = meter_state_save();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save Zigbee config: %s", esp_err_to_name(err));
    }
    meter_update_zigbee_attrs(true);
    return ESP_OK;
}

static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message)
{
    switch (callback_id) {
    case ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID:
        return zb_attribute_handler((const esp_zb_zcl_set_attr_value_message_t *)message);
    default:
        ESP_LOGD(TAG, "Zigbee action callback: 0x%x", callback_id);
        return ESP_OK;
    }
}

static void bdb_start_top_level_commissioning_cb(uint8_t mode_mask)
{
    ESP_ERROR_CHECK(esp_zb_bdb_start_top_level_commissioning(mode_mask));
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    uint32_t *p_sg_p = signal_struct->p_app_signal;
    esp_err_t err_status = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t sig_type = *p_sg_p;

    switch (sig_type) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Zigbee Stack Initialized. Starting Commissioning...");
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
        break;
    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (err_status != ESP_OK) {
            ESP_LOGW(TAG, "Failed to initialize Zigbee stack (status: %s)", esp_err_to_name(err_status));
            return;
        }

        s_zigbee_ready = true;
        ESP_LOGI(TAG, "Device started up in %s factory-reset mode", esp_zb_bdb_is_factory_new() ? "" : "non");
        meter_update_zigbee_attrs(false);
        esp_zb_scheduler_alarm((esp_zb_callback_t)periodic_report_cb, 0, REPORT_INTERVAL_MS);

        if (esp_zb_bdb_is_factory_new()) {
            ESP_LOGI(TAG, "Start network steering");
            esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
        } else {
            ESP_LOGI(TAG, "Device rebooted");
        }
        break;
    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (err_status == ESP_OK) {
            ESP_LOGI(TAG, "Joined network successfully");
            meter_update_zigbee_attrs(true);
        } else {
            ESP_LOGW(TAG, "Network steering failed, retrying in 1s");
            esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_top_level_commissioning_cb,
                                   ESP_ZB_BDB_MODE_NETWORK_STEERING,
                                   1000);
        }
        break;
    default:
        ESP_LOGI(TAG, "ZDO signal: %s (0x%x)", esp_zb_zdo_signal_to_string(sig_type), sig_type);
        break;
    }
}

static void esp_zb_task(void *pvParameters)
{
    esp_zb_cfg_t zb_nwk_cfg = {
        .esp_zb_role = ESP_ZB_DEVICE_TYPE_ED,
        .install_code_policy = false,
        .nwk_cfg.zed_cfg = {
            .ed_timeout = ESP_ZB_ED_AGING_TIMEOUT_64MIN,
            .keep_alive = 3000,
        },
    };
    esp_zb_init(&zb_nwk_cfg);

    esp_zb_attribute_list_t *metering_attr_list = esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_METERING);

    static uint8_t metering_device_type = ESP_ZB_ZCL_METERING_WATER_METERING;
    static uint8_t unit_of_measure = ESP_ZB_ZCL_METERING_UNIT_L_LH_BINARY;
    static esp_zb_int24_t instantaneous_demand = { .low = 0, .high = 0 };

    meter_state_t snap;
    meter_state_snapshot(&snap);
    s_current_summation_attr = uint64_to_zb_u48(snap.pulse_count);
    s_scaled_summation_attr = uint64_to_zb_u48(meter_scaled_summation(snap.pulse_count, snap.multiplier, snap.divisor));
    s_multiplier_attr = uint32_to_zb_u24(snap.multiplier);
    s_divisor_attr = uint32_to_zb_u24(snap.divisor);
    s_scale_multiplier_attr = snap.multiplier;
    s_scale_divisor_attr = snap.divisor;

    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(metering_attr_list,
                                            ESP_ZB_ZCL_CLUSTER_ID_METERING,
                                            ESP_ZB_ZCL_ATTR_METERING_METERING_DEVICE_TYPE_ID,
                                            ESP_ZB_ZCL_ATTR_TYPE_8BIT_ENUM,
                                            ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                            &metering_device_type));
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(metering_attr_list,
                                            ESP_ZB_ZCL_CLUSTER_ID_METERING,
                                            ESP_ZB_ZCL_ATTR_METERING_UNIT_OF_MEASURE_ID,
                                            ESP_ZB_ZCL_ATTR_TYPE_8BIT_ENUM,
                                            ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                            &unit_of_measure));
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(metering_attr_list,
                                            ESP_ZB_ZCL_CLUSTER_ID_METERING,
                                            ESP_ZB_ZCL_ATTR_METERING_MULTIPLIER_ID,
                                            ESP_ZB_ZCL_ATTR_TYPE_U24,
                                            ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                            &s_multiplier_attr));
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(metering_attr_list,
                                            ESP_ZB_ZCL_CLUSTER_ID_METERING,
                                            ESP_ZB_ZCL_ATTR_METERING_DIVISOR_ID,
                                            ESP_ZB_ZCL_ATTR_TYPE_U24,
                                            ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                            &s_divisor_attr));
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(metering_attr_list,
                                            ESP_ZB_ZCL_CLUSTER_ID_METERING,
                                            ESP_ZB_ZCL_ATTR_METERING_INSTANTANEOUS_DEMAND_ID,
                                            ESP_ZB_ZCL_ATTR_TYPE_S24,
                                            ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
                                            &instantaneous_demand));
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(metering_attr_list,
                                            ESP_ZB_ZCL_CLUSTER_ID_METERING,
                                            ESP_ZB_ZCL_ATTR_METERING_CURRENT_SUMMATION_DELIVERED_ID,
                                            ESP_ZB_ZCL_ATTR_TYPE_U48,
                                            ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
                                            &s_current_summation_attr));
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(metering_attr_list,
                                            ESP_ZB_ZCL_CLUSTER_ID_METERING,
                                            ATTR_SCALED_SUMMATION_ID,
                                            ESP_ZB_ZCL_ATTR_TYPE_U48,
                                            ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
                                            &s_scaled_summation_attr));
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(metering_attr_list,
                                            ESP_ZB_ZCL_CLUSTER_ID_METERING,
                                            ATTR_SCALE_MULTIPLIER_ID,
                                            ESP_ZB_ZCL_ATTR_TYPE_U32,
                                            ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE,
                                            &s_scale_multiplier_attr));
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(metering_attr_list,
                                            ESP_ZB_ZCL_CLUSTER_ID_METERING,
                                            ATTR_SCALE_DIVISOR_ID,
                                            ESP_ZB_ZCL_ATTR_TYPE_U32,
                                            ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE,
                                            &s_scale_divisor_attr));

    esp_zb_cluster_list_t *cluster_list = esp_zb_zcl_cluster_list_create();
    esp_zb_cluster_list_add_metering_cluster(cluster_list, metering_attr_list, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_basic_cluster_cfg_t basic_cfg = {
        .zcl_version = 0x02,
        .power_source = ESP_ZB_ZCL_BASIC_POWER_SOURCE_BATTERY,
    };
    esp_zb_attribute_list_t *basic_attr_list = esp_zb_basic_cluster_create(&basic_cfg);
    esp_zb_basic_cluster_add_attr(basic_attr_list,
                                  ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID,
                                  (void *)"\x0A" ESP_MANUFACTURER_NAME);
    esp_zb_basic_cluster_add_attr(basic_attr_list,
                                  ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID,
                                  (void *)"\x0A" ESP_MODEL_IDENTIFIER);
    esp_zb_cluster_list_add_basic_cluster(cluster_list, basic_attr_list, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_attribute_list_t *power_attr_list = esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG);
    s_battery_voltage_zcl = 0;
    s_battery_percent_zcl = 0;
    static uint8_t battery_size = ESP_ZB_ZCL_POWER_CONFIG_BATTERY_SIZE_AAA;
    static uint8_t battery_quantity = 3;
    static uint8_t battery_rated_voltage = 15;
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(power_attr_list,
                                            ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
                                            ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_ID,
                                            ESP_ZB_ZCL_ATTR_TYPE_U8,
                                            ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
                                            &s_battery_voltage_zcl));
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(power_attr_list,
                                            ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
                                            ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID,
                                            ESP_ZB_ZCL_ATTR_TYPE_U8,
                                            ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
                                            &s_battery_percent_zcl));
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(power_attr_list,
                                            ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
                                            ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_SIZE_ID,
                                            ESP_ZB_ZCL_ATTR_TYPE_8BIT_ENUM,
                                            ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                            &battery_size));
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(power_attr_list,
                                            ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
                                            ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_QUANTITY_ID,
                                            ESP_ZB_ZCL_ATTR_TYPE_U8,
                                            ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                            &battery_quantity));
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(power_attr_list,
                                            ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
                                            ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_RATED_VOLTAGE_ID,
                                            ESP_ZB_ZCL_ATTR_TYPE_U8,
                                            ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                            &battery_rated_voltage));
    esp_zb_cluster_list_add_power_config_cluster(cluster_list, power_attr_list, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_identify_cluster_cfg_t identify_cfg = { .identify_time = 0 };
    esp_zb_attribute_list_t *identify_attr_list = esp_zb_identify_cluster_create(&identify_cfg);
    esp_zb_cluster_list_add_identify_cluster(cluster_list, identify_attr_list, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();
    esp_zb_endpoint_config_t endpoint_config = {
        .endpoint = HA_ENDPOINT,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_METER_INTERFACE_DEVICE_ID,
        .app_device_version = 0,
    };
    esp_zb_ep_list_add_ep(ep_list, cluster_list, endpoint_config);

    esp_zb_device_register(ep_list);
    esp_zb_core_action_handler_register(zb_action_handler);
    esp_zb_set_primary_network_channel_set(ESP_ZB_PRIMARY_CHANNEL_MASK);
    ESP_ERROR_CHECK(esp_zb_start(false));

    esp_zb_stack_main_loop();
}

void app_main(void)
{
    esp_zb_platform_config_t config = {
        .radio_config = { .radio_mode = ZB_RADIO_MODE_NATIVE },
        .host_config = { .host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE },
    };

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_LOGI(TAG, "app_main: nvs ok");

    ESP_ERROR_CHECK(meter_state_load());
    ESP_LOGI(TAG, "app_main: meter state ok");

    ESP_ERROR_CHECK(adc_init());
    ESP_LOGI(TAG, "app_main: battery adc ok on ADC1 channel %d", BATTERY_ADC_CHANNEL);

    ESP_ERROR_CHECK(esp_zb_platform_config(&config));
    ESP_LOGI(TAG, "app_main: zigbee platform config ok");

    s_sensor_queue = xQueueCreate(SENSOR_QUEUE_LEN, sizeof(uint32_t));
    ESP_ERROR_CHECK(s_sensor_queue ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(xTaskCreate(sensor_task, "sensor", 4096, NULL, 6, NULL) == pdPASS ? ESP_OK : ESP_FAIL);
    ESP_LOGI(TAG, "app_main: sensor task created");

    ESP_ERROR_CHECK(xTaskCreate(esp_zb_task, "Zigbee_main", 4096, NULL, 5, NULL) == pdPASS ? ESP_OK : ESP_FAIL);
    ESP_LOGI(TAG, "app_main: zigbee task created");

    ESP_LOGI(TAG, "Light sleep support compiled as %s", ENABLE_LIGHT_SLEEP ? "enabled" : "disabled");
}
