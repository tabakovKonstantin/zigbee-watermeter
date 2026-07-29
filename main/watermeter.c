#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_system.h"
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
#include "zcl/esp_zigbee_zcl_ota.h"
#include "zcl/esp_zigbee_zcl_poll_control.h"
#include "zcl/esp_zigbee_zcl_power_config.h"

#include "battery_math.h"
#include "meter_math.h"

#define SENSOR_PIN ((gpio_num_t)CONFIG_WATERMETER_SENSOR_GPIO)
#define BATTERY_ADC_CHANNEL ADC_CHANNEL_0
#define HA_ENDPOINT 1

#define ESP_MANUFACTURER_NAME "ZigbeeHive"
#define ESP_MODEL_IDENTIFIER "WaterMeter"

#define DEFAULT_MULTIPLIER 10
#define DEFAULT_DIVISOR 1
#define REPORT_INTERVAL_MS (10 * 60 * 1000)
#define FIRST_REPORT_DELAY_MS 10000
#define JOIN_RETRY_INTERVAL_MS 5000
#define JOIN_RETRY_TIMEOUT_MS (5 * 60 * 1000)
#define DEBOUNCE_US (100 * 1000)
#define ZIGBEE_KEEP_ALIVE_MS 3000
#define ZIGBEE_SLEEP_THRESHOLD_MS 1000
#define ZIGBEE_AWAKE_AFTER_PULSE_MS 3000
#define ZIGBEE_DEEP_AWAKE_AFTER_PULSE_MS CONFIG_WATERMETER_DEEP_AWAKE_AFTER_PULSE_MS
#define ZIGBEE_WAKE_BEFORE_REPORT_MS 200
#define SENSOR_RELEASE_POLL_MS 20
#define SENSOR_RELEASE_STABLE_MS 50
#define SENSOR_RELEASE_TIMEOUT_MS (30 * 1000)
#define BATTERY_ADC_DISCARD_SAMPLES 1
#define BATTERY_ADC_AVG_SAMPLES 16
#define BATTERY_MAX_REASONABLE_MV 4300U
#define SENSOR_QUEUE_LEN 8
#define ESP_ZB_PRIMARY_CHANNEL_MASK ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK

#define NVS_NAMESPACE "watermeter"
#define NVS_KEY_PULSE_COUNT "pulse_count"
#define NVS_KEY_MULTIPLIER "multiplier"
#define NVS_KEY_DIVISOR "divisor"

#define MANUFACTURER_CODE 0x131B
#define OTA_IMAGE_TYPE 0x0001
#define OTA_HEADER_LENGTH 56
#define OTA_QUERY_INTERVAL_MINUTES 60
#define OTA_MAX_DATA_SIZE 0xff
#ifndef WATERMETER_OTA_FILE_VERSION
#define WATERMETER_OTA_FILE_VERSION 1
#endif
#define ATTR_SCALED_SUMMATION_ID 0xFC00
#define ATTR_SCALE_MULTIPLIER_ID 0xFC01
#define ATTR_SCALE_DIVISOR_ID 0xFC02

#ifndef CONFIG_WATERMETER_DEEP_SLEEP_TIMER_WAKE_MS
#define CONFIG_WATERMETER_DEEP_SLEEP_TIMER_WAKE_MS 600000
#endif
#ifndef CONFIG_WATERMETER_DEEP_AWAKE_AFTER_PULSE_MS
#define CONFIG_WATERMETER_DEEP_AWAKE_AFTER_PULSE_MS 8000
#endif

#if CONFIG_WATERMETER_SLEEP_MODE_DEEP && !GPIO_IS_DEEP_SLEEP_WAKEUP_VALID_GPIO(CONFIG_WATERMETER_SENSOR_GPIO)
#error "WATERMETER_SENSOR_GPIO must be a valid deep sleep wake GPIO in deep sleep mode"
#endif

static const char *TAG = "ZIGBEE_METER";

typedef struct {
    uint64_t pulse_count;
    uint32_t multiplier;
    uint32_t divisor;
} meter_state_t;

typedef struct {
    bool ready;
    bool joined;
    int64_t join_retry_deadline_us;
} zigbee_runtime_t;

typedef struct {
    adc_oneshot_unit_handle_t adc_handle;
    uint8_t voltage_zcl;
    uint8_t percent_zcl;
} battery_runtime_t;

typedef struct {
    esp_zb_uint48_t current_summation;
    esp_zb_uint48_t scaled_summation;
    esp_zb_uint24_t multiplier;
    esp_zb_uint24_t divisor;
    uint32_t scale_multiplier;
    uint32_t scale_divisor;
} meter_zcl_attrs_t;

typedef struct {
    esp_ota_handle_t handle;
    const esp_partition_t *partition;
    uint32_t received;
    uint32_t expected_size;
    uint32_t file_version;
    bool in_progress;
} ota_state_t;

static meter_state_t s_meter = {
    .pulse_count = 0,
    .multiplier = DEFAULT_MULTIPLIER,
    .divisor = DEFAULT_DIVISOR,
};

static portMUX_TYPE s_meter_mux = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t s_sensor_queue;
static nvs_handle_t s_nvs;
static esp_sleep_wakeup_cause_t s_boot_wakeup_cause;
static zigbee_runtime_t s_zigbee;
static battery_runtime_t s_battery;
static meter_zcl_attrs_t s_meter_attrs;
static ota_state_t s_ota;

static void bdb_start_top_level_commissioning_cb(uint8_t mode_mask);
#if CONFIG_WATERMETER_SLEEP_MODE_DEEP
static void deep_sleep_enter_cb(uint8_t arg);
static void schedule_deep_sleep_after_report(uint32_t delay_ms);
#endif

static const char *wakeup_cause_name(esp_sleep_wakeup_cause_t cause)
{
    switch (cause) {
    case ESP_SLEEP_WAKEUP_UNDEFINED:
        return "undefined";
    case ESP_SLEEP_WAKEUP_TIMER:
        return "timer";
    case ESP_SLEEP_WAKEUP_GPIO:
        return "gpio";
    default:
        return "other";
    }
}

static const char *sleep_mode_name(void)
{
#if CONFIG_WATERMETER_SLEEP_MODE_LIGHT
    return "light";
#elif CONFIG_WATERMETER_SLEEP_MODE_DEEP
    return "deep";
#else
    return "off";
#endif
}

static bool sleep_mode_is_light(void)
{
#if CONFIG_WATERMETER_SLEEP_MODE_LIGHT
    return true;
#else
    return false;
#endif
}

static bool sleep_allowed_now(void)
{
    if (!s_zigbee.joined || s_ota.in_progress) {
        return false;
    }
    if (gpio_get_level(SENSOR_PIN) == 0) {
        return false;
    }
    return true;
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

static void ota_reset_state(void)
{
    if (s_ota.in_progress && s_ota.handle) {
        esp_ota_abort(s_ota.handle);
    }
    s_ota.handle = 0;
    s_ota.partition = NULL;
    s_ota.received = 0;
    s_ota.expected_size = 0;
    s_ota.file_version = 0;
    s_ota.in_progress = false;
#if CONFIG_WATERMETER_SLEEP_MODE_LIGHT
    esp_zb_sleep_enable(true);
#endif
}

static esp_err_t ota_mark_running_app_valid(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state = ESP_OTA_IMG_UNDEFINED;
    esp_err_t err = esp_ota_get_state_partition(running, &ota_state);
    if (err == ESP_OK && ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "Marking OTA app valid");
        return esp_ota_mark_app_valid_cancel_rollback();
    }
    return ESP_OK;
}

static uint16_t ota_accept_status(const esp_zb_zcl_ota_upgrade_value_message_t *message)
{
    if (!message || message->info.dst_endpoint != HA_ENDPOINT) {
        return ESP_ZB_ZCL_OTA_UPGRADE_STATUS_ERROR;
    }
    if (message->ota_header.manufacturer_code != MANUFACTURER_CODE ||
        message->ota_header.image_type != OTA_IMAGE_TYPE ||
        message->ota_header.file_version <= WATERMETER_OTA_FILE_VERSION) {
        ESP_LOGW(TAG, "Reject OTA image: manufacturer=0x%04x image_type=0x%04x file_version=%" PRIu32,
                 message->ota_header.manufacturer_code,
                 message->ota_header.image_type,
                 message->ota_header.file_version);
        return ESP_ZB_ZCL_OTA_UPGRADE_STATUS_ERROR;
    }
    return ESP_ZB_ZCL_OTA_UPGRADE_STATUS_OK;
}

static esp_err_t ota_upgrade_handler(esp_zb_zcl_ota_upgrade_value_message_t *message)
{
    if (!message || message->info.status != ESP_ZB_ZCL_STATUS_SUCCESS) {
        return ESP_OK;
    }

    esp_err_t err = ESP_OK;

    switch (message->upgrade_status) {
    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_START:
        if (s_ota.in_progress) {
            message->upgrade_status = ESP_ZB_ZCL_OTA_UPGRADE_STATUS_BUSY;
            return ESP_OK;
        }
        message->upgrade_status = ota_accept_status(message);
        if (message->upgrade_status != ESP_ZB_ZCL_OTA_UPGRADE_STATUS_OK) {
            return ESP_OK;
        }

        s_ota.partition = esp_ota_get_next_update_partition(NULL);
        if (!s_ota.partition) {
            ESP_LOGE(TAG, "No OTA update partition found");
            message->upgrade_status = ESP_ZB_ZCL_OTA_UPGRADE_STATUS_ERROR;
            return ESP_OK;
        }

        s_ota.expected_size = message->ota_header.image_size;
        s_ota.file_version = message->ota_header.file_version;
        err = esp_ota_begin(s_ota.partition, OTA_WITH_SEQUENTIAL_WRITES, &s_ota.handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
            ota_reset_state();
            message->upgrade_status = ESP_ZB_ZCL_OTA_UPGRADE_STATUS_ERROR;
            return ESP_OK;
        }

        s_ota.in_progress = true;
#if CONFIG_WATERMETER_SLEEP_MODE_LIGHT
        esp_zb_sleep_enable(false);
#elif CONFIG_WATERMETER_SLEEP_MODE_DEEP
        esp_zb_scheduler_alarm_cancel((esp_zb_callback_t)deep_sleep_enter_cb, 0);
#endif
        ESP_LOGI(TAG, "OTA started: slot=%s version=%" PRIu32 " size=%" PRIu32,
                 s_ota.partition->label, s_ota.file_version, s_ota.expected_size);
        message->upgrade_status = ESP_ZB_ZCL_OTA_UPGRADE_STATUS_OK;
        break;

    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_RECEIVE:
        if (!s_ota.in_progress || !s_ota.handle || !message->payload || message->payload_size == 0) {
            message->upgrade_status = ESP_ZB_ZCL_OTA_UPGRADE_STATUS_ERROR;
            return ESP_OK;
        }
        err = esp_ota_write(s_ota.handle, message->payload, message->payload_size);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed at %" PRIu32 ": %s", s_ota.received, esp_err_to_name(err));
            ota_reset_state();
            message->upgrade_status = ESP_ZB_ZCL_OTA_UPGRADE_STATUS_ERROR;
            return ESP_OK;
        }
        s_ota.received += message->payload_size;
        message->upgrade_status = ESP_ZB_ZCL_OTA_UPGRADE_STATUS_OK;
        break;

    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_CHECK: {
        uint32_t expected_payload_size = s_ota.expected_size;
        if (expected_payload_size >= OTA_HEADER_LENGTH) {
            expected_payload_size -= OTA_HEADER_LENGTH;
        }
        if (!s_ota.in_progress || (expected_payload_size && s_ota.received != expected_payload_size)) {
            ESP_LOGE(TAG, "OTA size mismatch: received=%" PRIu32 " expected=%" PRIu32,
                     s_ota.received, expected_payload_size);
            ota_reset_state();
            message->upgrade_status = ESP_ZB_ZCL_OTA_UPGRADE_STATUS_ERROR;
            return ESP_OK;
        }
        message->upgrade_status = ESP_ZB_ZCL_OTA_UPGRADE_STATUS_OK;
        break;
    }

    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_APPLY:
    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_FINISH:
        if (!s_ota.in_progress || !s_ota.handle || !s_ota.partition) {
            message->upgrade_status = ESP_ZB_ZCL_OTA_UPGRADE_STATUS_ERROR;
            return ESP_OK;
        }
        err = esp_ota_end(s_ota.handle);
        if (err == ESP_OK) {
            err = esp_ota_set_boot_partition(s_ota.partition);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA finalize failed: %s", esp_err_to_name(err));
            ota_reset_state();
            message->upgrade_status = ESP_ZB_ZCL_OTA_UPGRADE_STATUS_ERROR;
            return ESP_OK;
        }

        ESP_LOGI(TAG, "OTA complete: version=%" PRIu32 " bytes=%" PRIu32 ", rebooting",
                 s_ota.file_version, s_ota.received);
        s_ota.handle = 0;
        message->upgrade_status = ESP_ZB_ZCL_OTA_UPGRADE_STATUS_OK;
        esp_restart();
        break;

    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_ABORT:
        ESP_LOGW(TAG, "OTA aborted");
        ota_reset_state();
        message->upgrade_status = ESP_ZB_ZCL_OTA_UPGRADE_STATUS_OK;
        break;

    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_SERVER_NOT_FOUND:
        ESP_LOGW(TAG, "OTA server not found");
        break;

    default:
        ESP_LOGD(TAG, "OTA callback status=%u", message->upgrade_status);
        break;
    }

    return ESP_OK;
}

static void meter_state_snapshot(meter_state_t *out)
{
    portENTER_CRITICAL(&s_meter_mux);
    *out = s_meter;
    portEXIT_CRITICAL(&s_meter_mux);
}

static uint32_t battery_read_mv(void)
{
    if (!s_battery.adc_handle) {
        ESP_LOGW(TAG, "Battery ADC read failed on A0/D0/GPIO0: ADC is not initialized");
        return 0;
    }

    uint32_t raw_sum = 0;
    int raw = 0;
    for (uint32_t i = 0; i < (BATTERY_ADC_DISCARD_SAMPLES + BATTERY_ADC_AVG_SAMPLES); i++) {
        esp_err_t err = adc_oneshot_read(s_battery.adc_handle, BATTERY_ADC_CHANNEL, &raw);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Battery ADC read failed on A0/D0/GPIO0: %s", esp_err_to_name(err));
            return 0;
        }
        if (i >= BATTERY_ADC_DISCARD_SAMPLES) {
            raw_sum += (uint32_t)raw;
        }
    }

    uint32_t raw_avg = raw_sum / BATTERY_ADC_AVG_SAMPLES;
    uint32_t adc_mv = battery_gpio_mv_from_raw(raw_avg);
    uint32_t battery_mv = battery_mv_from_gpio_mv(adc_mv);
    if (battery_mv > BATTERY_MAX_REASONABLE_MV) {
        ESP_LOGW(TAG,
                 "Battery ADC A0/D0/GPIO0 saturated/clamped: raw_avg=%" PRIu32 " adc=%" PRIu32
                 "mV battery=%" PRIu32 "mV clamped=%umV",
                 raw_avg, adc_mv, battery_mv, BATTERY_MAX_REASONABLE_MV);
        battery_mv = BATTERY_MAX_REASONABLE_MV;
    } else {
        ESP_LOGI(TAG, "Battery ADC A0/D0/GPIO0: raw_avg=%" PRIu32 " adc=%" PRIu32 "mV battery=%" PRIu32 "mV",
                 raw_avg, adc_mv, battery_mv);
    }
    return battery_mv;
}

static void battery_update_attr(void)
{
    uint32_t battery_mv = battery_read_mv();
    if (battery_mv == 0) {
        return;
    }

    uint8_t percent = battery_percent_from_mv(battery_mv);
    s_battery.voltage_zcl = (uint8_t)((battery_mv + 50U) / 100U);
    s_battery.percent_zcl = (uint8_t)(percent * 2U);

    esp_zb_zcl_set_attribute_val(HA_ENDPOINT,
                                 ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_ID,
                                 &s_battery.voltage_zcl,
                                 false);

    esp_zb_zcl_set_attribute_val(HA_ENDPOINT,
                                 ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID,
                                 &s_battery.percent_zcl,
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
    } else {
        ESP_LOGI(TAG, "Report attr 0x%04x/0x%04x queued", cluster_id, attr_id);
    }
}

static uint64_t meter_refresh_attr_mirrors(const meter_state_t *state)
{
    uint64_t scaled = meter_scaled_summation(state->pulse_count, state->multiplier, state->divisor);

    s_meter_attrs.current_summation = uint64_to_zb_u48(state->pulse_count);
    s_meter_attrs.scaled_summation = uint64_to_zb_u48(scaled);
    s_meter_attrs.multiplier = uint32_to_zb_u24(state->multiplier);
    s_meter_attrs.divisor = uint32_to_zb_u24(state->divisor);
    s_meter_attrs.scale_multiplier = state->multiplier;
    s_meter_attrs.scale_divisor = state->divisor;

    return scaled;
}

static void meter_update_zigbee_attrs(bool send_report, bool include_battery)
{
    if (!s_zigbee.ready) {
        return;
    }

    meter_state_t snap;
    meter_state_snapshot(&snap);

    uint64_t scaled = meter_refresh_attr_mirrors(&snap);

    esp_zb_lock_acquire(portMAX_DELAY);

    esp_zb_zcl_set_attribute_val(HA_ENDPOINT,
                                 ESP_ZB_ZCL_CLUSTER_ID_METERING,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 ESP_ZB_ZCL_ATTR_METERING_CURRENT_SUMMATION_DELIVERED_ID,
                                 &s_meter_attrs.current_summation,
                                 false);
    esp_zb_zcl_set_attribute_val(HA_ENDPOINT,
                                 ESP_ZB_ZCL_CLUSTER_ID_METERING,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 ESP_ZB_ZCL_ATTR_METERING_MULTIPLIER_ID,
                                 &s_meter_attrs.multiplier,
                                 false);
    esp_zb_zcl_set_attribute_val(HA_ENDPOINT,
                                 ESP_ZB_ZCL_CLUSTER_ID_METERING,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 ESP_ZB_ZCL_ATTR_METERING_DIVISOR_ID,
                                 &s_meter_attrs.divisor,
                                 false);
    esp_zb_zcl_set_attribute_val(HA_ENDPOINT,
                                 ESP_ZB_ZCL_CLUSTER_ID_METERING,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 ATTR_SCALED_SUMMATION_ID,
                                 &s_meter_attrs.scaled_summation,
                                 false);
    esp_zb_zcl_set_attribute_val(HA_ENDPOINT,
                                 ESP_ZB_ZCL_CLUSTER_ID_METERING,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 ATTR_SCALE_MULTIPLIER_ID,
                                 &s_meter_attrs.scale_multiplier,
                                 false);
    esp_zb_zcl_set_attribute_val(HA_ENDPOINT,
                                 ESP_ZB_ZCL_CLUSTER_ID_METERING,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 ATTR_SCALE_DIVISOR_ID,
                                 &s_meter_attrs.scale_divisor,
                                 false);

    if (include_battery) {
        battery_update_attr();
    }

    if (send_report) {
        report_attr(ESP_ZB_ZCL_CLUSTER_ID_METERING,
                    ESP_ZB_ZCL_ATTR_METERING_CURRENT_SUMMATION_DELIVERED_ID,
                    false);
        report_attr(ESP_ZB_ZCL_CLUSTER_ID_METERING, ATTR_SCALED_SUMMATION_ID, false);
        if (include_battery) {
            report_attr(ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
                        ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_ID,
                        false);
            report_attr(ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
                        ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID,
                        false);
        }
    }

    esp_zb_lock_release();

    ESP_LOGI(TAG, "Meter update: pulses=%" PRIu64 " multiplier=%" PRIu32 " divisor=%" PRIu32 " scaled=%" PRIu64,
             snap.pulse_count, snap.multiplier, snap.divisor, scaled);
}

static void periodic_report_cb(uint8_t arg)
{
    (void)arg;
    meter_update_zigbee_attrs(true, true);
#if CONFIG_WATERMETER_SLEEP_MODE_DEEP
    schedule_deep_sleep_after_report(ZIGBEE_DEEP_AWAKE_AFTER_PULSE_MS);
#else
    esp_zb_scheduler_alarm((esp_zb_callback_t)periodic_report_cb, 0, REPORT_INTERVAL_MS);
#endif
}

static void schedule_first_report(void)
{
    ESP_LOGI(TAG, "Scheduling first report in %d ms", FIRST_REPORT_DELAY_MS);
    esp_zb_scheduler_alarm((esp_zb_callback_t)periodic_report_cb, 0, FIRST_REPORT_DELAY_MS);
}

static void drain_sensor_queue(void)
{
    uint32_t ignored_gpio = 0;
    uint32_t drained = 0;
    while (xQueueReceive(s_sensor_queue, &ignored_gpio, 0) == pdTRUE) {
        drained++;
    }
    if (drained > 0) {
        ESP_LOGI(TAG, "Drained %" PRIu32 " queued duplicate sensor events", drained);
    }
}

#if CONFIG_WATERMETER_SLEEP_MODE_DEEP
static void deep_sleep_enter_cb(uint8_t arg);
#endif

#if CONFIG_WATERMETER_SLEEP_MODE_LIGHT
static void enable_zigbee_sleep_cb(uint8_t arg)
{
    (void)arg;
    if (s_ota.in_progress) {
        ESP_LOGI(TAG, "Keep Zigbee sleep disabled during OTA");
        return;
    }
    ESP_LOGI(TAG, "Re-enable Zigbee sleep after pulse report");
    esp_zb_sleep_enable(true);
}
#endif

static void keep_awake_for_pulse_report(void)
{
#if CONFIG_WATERMETER_SLEEP_MODE_LIGHT
    ESP_LOGI(TAG, "Keep Zigbee awake for %d ms after pulse report", ZIGBEE_AWAKE_AFTER_PULSE_MS);
    esp_zb_sleep_enable(false);
    esp_zb_scheduler_alarm_cancel((esp_zb_callback_t)enable_zigbee_sleep_cb, 0);
    esp_zb_scheduler_alarm((esp_zb_callback_t)enable_zigbee_sleep_cb, 0, ZIGBEE_AWAKE_AFTER_PULSE_MS);
#elif CONFIG_WATERMETER_SLEEP_MODE_DEEP
    ESP_LOGI(TAG, "Keep Zigbee awake for %d ms after pulse report", ZIGBEE_DEEP_AWAKE_AFTER_PULSE_MS);
    esp_zb_scheduler_alarm_cancel((esp_zb_callback_t)deep_sleep_enter_cb, 0);
    esp_zb_scheduler_alarm((esp_zb_callback_t)deep_sleep_enter_cb, 0, ZIGBEE_DEEP_AWAKE_AFTER_PULSE_MS);
#endif
}

#if CONFIG_WATERMETER_SLEEP_MODE_LIGHT || CONFIG_WATERMETER_SLEEP_MODE_DEEP
static void enqueue_sensor_event_from_sleep_wakeup(void)
{
    if (!s_sensor_queue) {
        return;
    }

    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    int level = gpio_get_level(SENSOR_PIN);
    ESP_LOGI(TAG, "Wakeup check: cause=%s (%d) GPIO%d level=%d",
             wakeup_cause_name(cause), cause, SENSOR_PIN, level);

    if (cause == ESP_SLEEP_WAKEUP_GPIO && level == 0) {
        uint32_t gpio_num = SENSOR_PIN;
        if (xQueueSend(s_sensor_queue, &gpio_num, 0) != pdTRUE) {
            ESP_LOGW(TAG, "Sensor queue full after GPIO wakeup");
        }
    }
}
#endif

static void finish_sensor_pulse_before_sleep(void)
{
    if (gpio_get_level(SENSOR_PIN) != 0) {
        drain_sensor_queue();
        return;
    }

    ESP_LOGI(TAG, "GPIO%d still low after pulse, keep Zigbee awake until release", SENSOR_PIN);
#if CONFIG_WATERMETER_SLEEP_MODE_LIGHT
    esp_zb_sleep_enable(false);
    esp_zb_scheduler_alarm_cancel((esp_zb_callback_t)enable_zigbee_sleep_cb, 0);
#elif CONFIG_WATERMETER_SLEEP_MODE_DEEP
    esp_zb_scheduler_alarm_cancel((esp_zb_callback_t)deep_sleep_enter_cb, 0);
#endif

    bool released = false;
    const int64_t timeout_us = (int64_t)SENSOR_RELEASE_TIMEOUT_MS * 1000;
    const int64_t deadline_us = esp_timer_get_time() + timeout_us;

    while (esp_timer_get_time() < deadline_us) {
        if (gpio_get_level(SENSOR_PIN) == 0) {
            vTaskDelay(pdMS_TO_TICKS(SENSOR_RELEASE_POLL_MS));
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(SENSOR_RELEASE_STABLE_MS));
        if (gpio_get_level(SENSOR_PIN) != 0) {
            released = true;
            break;
        }

        ESP_LOGI(TAG, "GPIO%d went low again during release debounce", SENSOR_PIN);
        vTaskDelay(pdMS_TO_TICKS(SENSOR_RELEASE_POLL_MS));
    }

    drain_sensor_queue();
    if (!released) {
        ESP_LOGW(TAG, "GPIO%d release wait timed out after %d ms", SENSOR_PIN, SENSOR_RELEASE_TIMEOUT_MS);
#if CONFIG_WATERMETER_SLEEP_MODE_LIGHT
        esp_zb_sleep_enable(true);
#elif CONFIG_WATERMETER_SLEEP_MODE_DEEP
        schedule_deep_sleep_after_report(ZIGBEE_DEEP_AWAKE_AFTER_PULSE_MS);
#endif
        return;
    }

    ESP_LOGI(TAG, "GPIO%d released high, Zigbee sleep can resume", SENSOR_PIN);
    if (sleep_allowed_now() && sleep_mode_is_light()) {
        esp_zb_sleep_enable(true);
    }
}

#if CONFIG_WATERMETER_SLEEP_MODE_DEEP
static void deep_sleep_enter_cb(uint8_t arg)
{
    (void)arg;

    if (!sleep_allowed_now()) {
        ESP_LOGI(TAG, "Deep sleep postponed: joined=%s ota=%s GPIO%d=%d",
                 s_zigbee.joined ? "true" : "false",
                 s_ota.in_progress ? "true" : "false",
                 SENSOR_PIN,
                 gpio_get_level(SENSOR_PIN));
        esp_zb_scheduler_alarm_cancel((esp_zb_callback_t)deep_sleep_enter_cb, 0);
        esp_zb_scheduler_alarm((esp_zb_callback_t)deep_sleep_enter_cb, 0, 1000);
        return;
    }

    drain_sensor_queue();
    ESP_ERROR_CHECK(esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL));
    ESP_ERROR_CHECK(esp_deep_sleep_enable_gpio_wakeup(1ULL << SENSOR_PIN, ESP_GPIO_WAKEUP_GPIO_LOW));
    ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup((uint64_t)CONFIG_WATERMETER_DEEP_SLEEP_TIMER_WAKE_MS * 1000ULL));
    ESP_LOGI(TAG, "Entering deep sleep: GPIO%d low wake, timer=%d ms",
             SENSOR_PIN, CONFIG_WATERMETER_DEEP_SLEEP_TIMER_WAKE_MS);
    esp_deep_sleep_start();
}

static void schedule_deep_sleep_after_report(uint32_t delay_ms)
{
    esp_zb_scheduler_alarm_cancel((esp_zb_callback_t)deep_sleep_enter_cb, 0);
    esp_zb_scheduler_alarm((esp_zb_callback_t)deep_sleep_enter_cb, 0, delay_ms);
}
#endif

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
#if CONFIG_WATERMETER_SLEEP_MODE_LIGHT
    ESP_ERROR_CHECK(gpio_wakeup_enable(SENSOR_PIN, GPIO_INTR_LOW_LEVEL));
    ESP_ERROR_CHECK(esp_sleep_enable_gpio_wakeup());
#endif
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(SENSOR_PIN, sensor_isr_handler, (void *)SENSOR_PIN));
#if CONFIG_WATERMETER_SLEEP_MODE_DEEP
    enqueue_sensor_event_from_sleep_wakeup();
#endif

    ESP_LOGI(TAG, "Sensor interrupt started on GPIO%d", SENSOR_PIN);

    uint32_t gpio_num = 0;
    int64_t last_pulse_us = 0;
    while (1) {
        if (xQueueReceive(s_sensor_queue, &gpio_num, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        int64_t now_us = esp_timer_get_time();
        if ((now_us - last_pulse_us) < DEBOUNCE_US) {
            ESP_LOGD(TAG, "Ignore GPIO%" PRIu32 " pulse inside debounce window", gpio_num);
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
        if (s_zigbee.joined) {
            keep_awake_for_pulse_report();
            vTaskDelay(pdMS_TO_TICKS(ZIGBEE_WAKE_BEFORE_REPORT_MS));
        }
        meter_update_zigbee_attrs(true, false);
        finish_sensor_pulse_before_sleep();
        last_pulse_us = esp_timer_get_time();
    }
}

static esp_err_t adc_init(void)
{
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT_1,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&init_config, &s_battery.adc_handle), TAG, "adc unit");

    adc_oneshot_chan_cfg_t chan_config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_12,
    };
    return adc_oneshot_config_channel(s_battery.adc_handle, BATTERY_ADC_CHANNEL, &chan_config);
}

#if CONFIG_WATERMETER_SLEEP_MODE_LIGHT
static esp_err_t power_management_init(void)
{
    int cpu_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    const esp_pm_config_t pm_config = {
        .max_freq_mhz = cpu_freq_mhz,
        .min_freq_mhz = cpu_freq_mhz,
        .light_sleep_enable = true,
    };
    return esp_pm_configure(&pm_config);
}
#endif

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
    meter_update_zigbee_attrs(true, true);
    return ESP_OK;
}

static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message)
{
    switch (callback_id) {
    case ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID:
        return zb_attribute_handler((const esp_zb_zcl_set_attr_value_message_t *)message);
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
            ESP_LOGI(TAG, "Device rebooted");
            schedule_first_report();
        }
        break;
    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (err_status == ESP_OK) {
            s_zigbee.joined = true;
            s_zigbee.join_retry_deadline_us = 0;
            ESP_LOGI(TAG, "Joined network successfully");
            schedule_first_report();
        } else {
            ESP_LOGW(TAG, "Network steering failed, retrying in %d ms", JOIN_RETRY_INTERVAL_MS);
            schedule_join_retry();
        }
        break;
    case ESP_ZB_COMMON_SIGNAL_CAN_SLEEP: {
#if CONFIG_WATERMETER_SLEEP_MODE_LIGHT
        esp_zb_zdo_signal_can_sleep_params_t *sleep_params =
            (esp_zb_zdo_signal_can_sleep_params_t *)esp_zb_app_signal_get_params(p_sg_p);
        if (s_zigbee.joined && err_status == ESP_OK) {
            if (gpio_get_level(SENSOR_PIN) == 0) {
                ESP_LOGI(TAG, "Skip Zigbee sleep while GPIO%d is low", SENSOR_PIN);
                break;
            }
            ESP_LOGI(TAG, "Zigbee stack can sleep for %" PRIu32 " ms", sleep_params->sleep_duration);
            esp_zb_sleep_now();
            esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
            ESP_LOGI(TAG, "Returned from Zigbee sleep, wake cause=%s (%d)",
                     wakeup_cause_name(cause), cause);
            enqueue_sensor_event_from_sleep_wakeup();
        }
#endif
        break;
    }
    default:
        ESP_LOGI(TAG, "ZDO signal: %s (0x%x)", esp_zb_zdo_signal_to_string(sig_type), sig_type);
        break;
    }
}

static void zigbee_configure_sleep(void)
{
#if CONFIG_WATERMETER_SLEEP_MODE_LIGHT
    esp_zb_set_rx_on_when_idle(false);
    ESP_ERROR_CHECK(esp_zb_sleep_set_threshold(ZIGBEE_SLEEP_THRESHOLD_MS));
    esp_zb_sleep_enable(true);
    ESP_LOGI(TAG, "Enable Zigbee-managed light sleep, rx_on_when_idle=%s",
             esp_zb_get_rx_on_when_idle() ? "true" : "false");
#elif CONFIG_WATERMETER_SLEEP_MODE_DEEP
    esp_zb_set_rx_on_when_idle(false);
    ESP_ERROR_CHECK(esp_zb_sleep_set_threshold(ZIGBEE_SLEEP_THRESHOLD_MS));
    esp_zb_sleep_enable(true);
    ESP_LOGI(TAG, "Enable Zigbee sleepy end-device mode for deep sleep, rx_on_when_idle=%s",
             esp_zb_get_rx_on_when_idle() ? "true" : "false");
#else
    esp_zb_set_rx_on_when_idle(true);
    esp_zb_sleep_enable(false);
    ESP_LOGI(TAG, "Zigbee stack sleep disabled for sleep mode=%s, rx_on_when_idle=%s",
             sleep_mode_name(), esp_zb_get_rx_on_when_idle() ? "true" : "false");
#endif
}

static esp_zb_attribute_list_t *zigbee_create_metering_cluster(void)
{
    esp_zb_attribute_list_t *metering_attr_list = esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_METERING);
    static uint8_t metering_device_type = ESP_ZB_ZCL_METERING_WATER_METERING;
    static uint8_t unit_of_measure = ESP_ZB_ZCL_METERING_UNIT_L_LH_BINARY;
    static esp_zb_int24_t instantaneous_demand = { .low = 0, .high = 0 };

    meter_state_t snap;
    meter_state_snapshot(&snap);
    meter_refresh_attr_mirrors(&snap);

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
                                            &s_meter_attrs.multiplier));
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(metering_attr_list,
                                            ESP_ZB_ZCL_CLUSTER_ID_METERING,
                                            ESP_ZB_ZCL_ATTR_METERING_DIVISOR_ID,
                                            ESP_ZB_ZCL_ATTR_TYPE_U24,
                                            ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                            &s_meter_attrs.divisor));
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
                                            &s_meter_attrs.current_summation));
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(metering_attr_list,
                                            ESP_ZB_ZCL_CLUSTER_ID_METERING,
                                            ATTR_SCALED_SUMMATION_ID,
                                            ESP_ZB_ZCL_ATTR_TYPE_U48,
                                            ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
                                            &s_meter_attrs.scaled_summation));
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(metering_attr_list,
                                            ESP_ZB_ZCL_CLUSTER_ID_METERING,
                                            ATTR_SCALE_MULTIPLIER_ID,
                                            ESP_ZB_ZCL_ATTR_TYPE_U32,
                                            ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE,
                                            &s_meter_attrs.scale_multiplier));
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(metering_attr_list,
                                            ESP_ZB_ZCL_CLUSTER_ID_METERING,
                                            ATTR_SCALE_DIVISOR_ID,
                                            ESP_ZB_ZCL_ATTR_TYPE_U32,
                                            ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE,
                                            &s_meter_attrs.scale_divisor));

    return metering_attr_list;
}

static void zigbee_add_basic_cluster(esp_zb_cluster_list_t *cluster_list)
{
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
}

static void zigbee_add_power_config_cluster(esp_zb_cluster_list_t *cluster_list)
{
    esp_zb_attribute_list_t *power_attr_list = esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG);
    s_battery.voltage_zcl = 0;
    s_battery.percent_zcl = 0;
    static uint8_t battery_size = ESP_ZB_ZCL_POWER_CONFIG_BATTERY_SIZE_BUILT_IN;
    static uint8_t battery_quantity = 1;
    static uint8_t battery_rated_voltage = 37;
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(power_attr_list,
                                            ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
                                            ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_ID,
                                            ESP_ZB_ZCL_ATTR_TYPE_U8,
                                            ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
                                            &s_battery.voltage_zcl));
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(power_attr_list,
                                            ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
                                            ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID,
                                            ESP_ZB_ZCL_ATTR_TYPE_U8,
                                            ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
                                            &s_battery.percent_zcl));
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
}

static void zigbee_add_identify_cluster(esp_zb_cluster_list_t *cluster_list)
{
    esp_zb_identify_cluster_cfg_t identify_cfg = { .identify_time = 0 };
    esp_zb_attribute_list_t *identify_attr_list = esp_zb_identify_cluster_create(&identify_cfg);
    esp_zb_cluster_list_add_identify_cluster(cluster_list, identify_attr_list, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
}

static void zigbee_add_poll_control_cluster(esp_zb_cluster_list_t *cluster_list)
{
    esp_zb_poll_control_cluster_cfg_t poll_control_cfg = {
        .check_in_interval = 2400,      /* 10 minutes in quarter-seconds */
        .long_poll_interval = 20,       /* 5 seconds in quarter-seconds */
        .short_poll_interval = 2,       /* 0.5 seconds in quarter-seconds */
        .fast_poll_timeout = 40,        /* 10 seconds in quarter-seconds */
        .check_in_interval_min = 240,   /* 1 minute in quarter-seconds */
        .long_poll_interval_min = 4,    /* 1 second in quarter-seconds */
        .fast_poll_timeout_max = 240,   /* 1 minute in quarter-seconds */
    };
    esp_zb_attribute_list_t *poll_control_attr_list = esp_zb_poll_control_cluster_create(&poll_control_cfg);
    esp_zb_cluster_list_add_poll_control_cluster(cluster_list,
                                                 poll_control_attr_list,
                                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
}

static void zigbee_add_ota_cluster(esp_zb_cluster_list_t *cluster_list)
{
    esp_zb_ota_cluster_cfg_t ota_cfg = {
        .ota_upgrade_file_version = WATERMETER_OTA_FILE_VERSION,
        .ota_upgrade_manufacturer = MANUFACTURER_CODE,
        .ota_upgrade_image_type = OTA_IMAGE_TYPE,
        .ota_min_block_reque = ESP_ZB_OTA_UPGRADE_MIN_BLOCK_PERIOD_DEF_VALUE,
        .ota_upgrade_file_offset = ESP_ZB_ZCL_OTA_UPGRADE_FILE_OFFSET_DEF_VALUE,
        .ota_upgrade_downloaded_file_ver = ESP_ZB_ZCL_OTA_UPGRADE_DOWNLOADED_FILE_VERSION_DEF_VALUE,
        .ota_upgrade_server_id = ESP_ZB_ZCL_OTA_UPGRADE_SERVER_DEF_VALUE,
        .ota_image_upgrade_status = ESP_ZB_ZCL_OTA_UPGRADE_IMAGE_STATUS_DEF_VALUE,
    };
    esp_zb_attribute_list_t *ota_attr_list = esp_zb_ota_cluster_create(&ota_cfg);
    static esp_zb_zcl_ota_upgrade_client_variable_t ota_client_variable = {
        .timer_query = OTA_QUERY_INTERVAL_MINUTES,
        .hw_version = 1,
        .max_data_size = OTA_MAX_DATA_SIZE,
    };
    ESP_ERROR_CHECK(esp_zb_ota_cluster_add_attr(ota_attr_list,
                                                ESP_ZB_ZCL_ATTR_OTA_UPGRADE_CLIENT_DATA_ID,
                                                &ota_client_variable));
    esp_zb_cluster_list_add_ota_cluster(cluster_list, ota_attr_list, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
}

static esp_zb_ep_list_t *zigbee_create_endpoint(void)
{
    esp_zb_cluster_list_t *cluster_list = esp_zb_zcl_cluster_list_create();
    esp_zb_attribute_list_t *metering_attr_list = zigbee_create_metering_cluster();
    esp_zb_cluster_list_add_metering_cluster(cluster_list, metering_attr_list, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    zigbee_add_basic_cluster(cluster_list);
    zigbee_add_power_config_cluster(cluster_list);
    zigbee_add_identify_cluster(cluster_list);
    zigbee_add_poll_control_cluster(cluster_list);
    zigbee_add_ota_cluster(cluster_list);

    esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();
    esp_zb_endpoint_config_t endpoint_config = {
        .endpoint = HA_ENDPOINT,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_METER_INTERFACE_DEVICE_ID,
        .app_device_version = 0,
    };
    esp_zb_ep_list_add_ep(ep_list, cluster_list, endpoint_config);
    return ep_list;
}

static void esp_zb_task(void *pvParameters)
{
    (void)pvParameters;

    esp_zb_cfg_t zb_nwk_cfg = {
        .esp_zb_role = ESP_ZB_DEVICE_TYPE_ED,
        .install_code_policy = false,
        .nwk_cfg.zed_cfg = {
            .ed_timeout = ESP_ZB_ED_AGING_TIMEOUT_64MIN,
            .keep_alive = ZIGBEE_KEEP_ALIVE_MS,
        },
    };
    esp_zb_init(&zb_nwk_cfg);
    zigbee_configure_sleep();

    esp_zb_ep_list_t *ep_list = zigbee_create_endpoint();
    esp_zb_device_register(ep_list);
    esp_zb_core_action_handler_register(zb_action_handler);
    esp_zb_set_primary_network_channel_set(ESP_ZB_PRIMARY_CHANNEL_MASK);
    ESP_ERROR_CHECK(esp_zb_start(false));
    esp_zb_set_node_descriptor_power_source(false);

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
    s_boot_wakeup_cause = esp_sleep_get_wakeup_cause();
    ESP_LOGI(TAG, "app_main: boot wake cause=%s (%d)",
             wakeup_cause_name(s_boot_wakeup_cause), s_boot_wakeup_cause);

    ESP_ERROR_CHECK(ota_mark_running_app_valid());
    ESP_LOGI(TAG, "app_main: firmware=%s ota_file_version=%d", esp_app_get_description()->version,
             WATERMETER_OTA_FILE_VERSION);

    ESP_ERROR_CHECK(meter_state_load());
    ESP_LOGI(TAG, "app_main: meter state ok");

    ESP_ERROR_CHECK(adc_init());
    ESP_LOGI(TAG, "app_main: battery adc ok on ADC1 channel %d", BATTERY_ADC_CHANNEL);

#if CONFIG_WATERMETER_SLEEP_MODE_LIGHT
    ESP_ERROR_CHECK(power_management_init());
    ESP_LOGI(TAG, "app_main: power management ok");
#endif

    ESP_ERROR_CHECK(esp_zb_platform_config(&config));
    ESP_LOGI(TAG, "app_main: zigbee platform config ok");

    s_sensor_queue = xQueueCreate(SENSOR_QUEUE_LEN, sizeof(uint32_t));
    ESP_ERROR_CHECK(s_sensor_queue ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(xTaskCreate(sensor_task, "sensor", 4096, NULL, 6, NULL) == pdPASS ? ESP_OK : ESP_FAIL);
    ESP_LOGI(TAG, "app_main: sensor task created");

    ESP_ERROR_CHECK(xTaskCreate(esp_zb_task, "Zigbee_main", 4096, NULL, 5, NULL) == pdPASS ? ESP_OK : ESP_FAIL);
    ESP_LOGI(TAG, "app_main: zigbee task created");

    ESP_LOGI(TAG, "Sleep mode=%s sensor GPIO=%d", sleep_mode_name(), SENSOR_PIN);
}
