#include "zigbee_app.h"

#include <inttypes.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_zigbee_cluster.h"
#include "esp_zigbee_core.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nwk/esp_zigbee_nwk.h"
#include "platform/esp_zigbee_platform.h"
#include "soc/esp32c6/rtc.h"
#include "zcl/esp_zigbee_zcl_common.h"
#include "zcl/esp_zigbee_zcl_command.h"
#include "zcl/esp_zigbee_zcl_metering.h"

#include "meter_math.h"
#include "meter_state.h"
#include "ota.h"
#include "power_schedule.h"
#include "sleep_control.h"
#include "zigbee_clusters.h"

#define SENSOR_PIN ((gpio_num_t)CONFIG_WATERMETER_SENSOR_GPIO)
#define JOIN_RETRY_INTERVAL_MS 5000
#define JOIN_RETRY_TIMEOUT_MS (5 * 60 * 1000)
#define DELIVERY_RETRY_INTERVAL_MS 1000
#define REPORT_SCHEDULE_MAGIC 0x574D5253U
#define ESP_ZB_PRIMARY_CHANNEL_MASK ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK

static const char *TAG = "ZIGBEE_APP";

typedef enum {
    DELIVERY_IDLE,
    DELIVERY_CURRENT_SUMMATION,
    DELIVERY_SCALED_SUMMATION,
} delivery_stage_t;

typedef struct {
    bool ready;
    bool joined;
    bool pulse_activity;
    bool resend_required;
    bool include_battery;
    bool force_battery_report;
    int64_t join_retry_deadline_us;
    uint64_t delivery_deadline_us;
    uint64_t last_pulse_us;
    delivery_stage_t delivery_stage;
} zigbee_runtime_t;

RTC_DATA_ATTR static uint32_t s_report_schedule_magic;
RTC_DATA_ATTR static power_schedule_t s_report_schedule;

static zigbee_runtime_t s_zigbee;

static const uint32_t s_retry_delays_ms[POWER_SCHEDULE_RETRY_LEVELS] = {
    CONFIG_WATERMETER_RETRY_FIRST_MS,
    CONFIG_WATERMETER_RETRY_SECOND_MS,
    CONFIG_WATERMETER_RETRY_THIRD_MS,
    CONFIG_WATERMETER_RETRY_MAX_MS,
};

static void bdb_start_top_level_commissioning_cb(uint8_t mode_mask);
static void delivery_retry_cb(uint8_t arg);
static void delivery_timeout_cb(uint8_t arg);
static void periodic_report_cb(uint8_t arg);
static void pulse_report_cb(uint8_t arg);

static uint64_t rtc_time_us(void)
{
    return esp_rtc_get_time_us();
}

static void init_report_schedule(void)
{
    if (s_report_schedule_magic == REPORT_SCHEDULE_MAGIC) {
        return;
    }

    power_schedule_reset(&s_report_schedule);
    s_report_schedule_magic = REPORT_SCHEDULE_MAGIC;
}

static uint32_t regular_wake_delay_ms(uint64_t now_us)
{
    uint32_t delay_ms = power_schedule_next_wake_ms(&s_report_schedule, now_us);
    return delay_ms == 0 ? 1000 : delay_ms;
}

static void schedule_periodic_report(uint32_t delay_ms)
{
#if CONFIG_WATERMETER_SLEEP_MODE_DEEP
    sleep_control_schedule_deep_sleep(0, delay_ms);
#else
    esp_zb_scheduler_alarm_cancel((esp_zb_callback_t)periodic_report_cb, 0);
    esp_zb_scheduler_alarm((esp_zb_callback_t)periodic_report_cb, 0, delay_ms);
#endif
}

static void cancel_delivery_alarms(void)
{
    esp_zb_scheduler_alarm_cancel((esp_zb_callback_t)delivery_retry_cb, 0);
    esp_zb_scheduler_alarm_cancel((esp_zb_callback_t)delivery_timeout_cb, 0);
}

static void schedule_delivery_timeout(void)
{
    esp_zb_scheduler_alarm_cancel((esp_zb_callback_t)delivery_timeout_cb, 0);
    esp_zb_scheduler_alarm((esp_zb_callback_t)delivery_timeout_cb, 0, CONFIG_WATERMETER_DELIVERY_WINDOW_MS);
}

static void delivery_failed(const char *reason)
{
    if (s_zigbee.delivery_stage == DELIVERY_IDLE) {
        return;
    }

    ESP_LOGW(TAG, "Meter delivery failed: %s", reason);
    cancel_delivery_alarms();
    s_zigbee.delivery_stage = DELIVERY_IDLE;
    s_zigbee.resend_required = false;
    s_zigbee.include_battery = false;
    esp_zb_set_default_long_poll_interval(CONFIG_WATERMETER_ZIGBEE_KEEP_ALIVE_MS);

    uint32_t retry_delay_ms = power_schedule_report_failed(&s_report_schedule, s_retry_delays_ms);
    schedule_periodic_report(retry_delay_ms);
}

static uint32_t pulse_grace_remaining_ms(uint64_t now_us)
{
    if (!s_zigbee.pulse_activity || gpio_get_level(SENSOR_PIN) == 0) {
        return 0;
    }

    uint64_t grace_deadline_us =
        s_zigbee.last_pulse_us + (uint64_t)CONFIG_WATERMETER_PULSE_GRACE_MS * 1000ULL;
    if (grace_deadline_us <= now_us) {
        return 0;
    }
    return (uint32_t)((grace_deadline_us - now_us + 999ULL) / 1000ULL);
}

static void delivery_succeeded(void)
{
    cancel_delivery_alarms();
    s_zigbee.delivery_stage = DELIVERY_IDLE;
    s_zigbee.resend_required = false;

    bool battery_attempted = s_zigbee.include_battery;
    if (battery_attempted) {
        zigbee_clusters_report_battery();
    }
    s_zigbee.include_battery = false;
    s_zigbee.force_battery_report = false;

    uint64_t now_us = rtc_time_us();
    power_schedule_report_succeeded(&s_report_schedule,
                                    now_us,
                                    CONFIG_WATERMETER_REPORT_INTERVAL_MS,
                                    CONFIG_WATERMETER_BATTERY_REPORT_INTERVAL_MS,
                                    battery_attempted);
    esp_zb_set_default_long_poll_interval(CONFIG_WATERMETER_ZIGBEE_KEEP_ALIVE_MS);

    uint32_t sleep_delay_ms = pulse_grace_remaining_ms(now_us);
    if (battery_attempted && sleep_delay_ms < CONFIG_WATERMETER_REPORT_FLUSH_MS) {
        sleep_delay_ms = CONFIG_WATERMETER_REPORT_FLUSH_MS;
    }

#if CONFIG_WATERMETER_SLEEP_MODE_DEEP
    sleep_control_schedule_deep_sleep(sleep_delay_ms, regular_wake_delay_ms(now_us));
#else
    schedule_periodic_report(regular_wake_delay_ms(now_us));
#endif
    ESP_LOGI(TAG, "Meter delivery confirmed; sleep in %" PRIu32 " ms", sleep_delay_ms);
}

static uint16_t current_delivery_attr(void)
{
    return s_zigbee.delivery_stage == DELIVERY_CURRENT_SUMMATION
               ? ESP_ZB_ZCL_ATTR_METERING_CURRENT_SUMMATION_DELIVERED_ID
               : ATTR_SCALED_SUMMATION_ID;
}

static void send_current_delivery_stage(void)
{
    if (s_zigbee.delivery_stage == DELIVERY_IDLE) {
        return;
    }
    if (rtc_time_us() >= s_zigbee.delivery_deadline_us) {
        delivery_failed("delivery window expired");
        return;
    }

    esp_err_t err = zigbee_clusters_report_meter_attribute(current_delivery_attr());
    if (err == ESP_OK) {
        return;
    }

    ESP_LOGW(TAG, "Report queue failed, retrying: %s", esp_err_to_name(err));
    esp_zb_scheduler_alarm_cancel((esp_zb_callback_t)delivery_retry_cb, 0);
    esp_zb_scheduler_alarm((esp_zb_callback_t)delivery_retry_cb, 0, DELIVERY_RETRY_INTERVAL_MS);
}

static void start_delivery_sequence(void)
{
    zigbee_clusters_refresh_meter(s_zigbee.include_battery);
    s_zigbee.delivery_stage = DELIVERY_CURRENT_SUMMATION;
    send_current_delivery_stage();
}

static void request_delivery(bool include_battery)
{
    if (!s_zigbee.ready || !s_zigbee.joined) {
        return;
    }

    sleep_control_cancel_pending_sleep();
    s_zigbee.include_battery |= include_battery;
    s_zigbee.delivery_deadline_us =
        rtc_time_us() + (uint64_t)CONFIG_WATERMETER_DELIVERY_WINDOW_MS * 1000ULL;
    schedule_delivery_timeout();
    esp_zb_set_default_long_poll_interval(CONFIG_WATERMETER_PARENT_POLL_INTERVAL_MS);

    if (s_zigbee.delivery_stage != DELIVERY_IDLE) {
        s_zigbee.resend_required = true;
        return;
    }

    start_delivery_sequence();
}

static void delivery_retry_cb(uint8_t arg)
{
    (void)arg;
    send_current_delivery_stage();
}

static void delivery_timeout_cb(uint8_t arg)
{
    (void)arg;
    delivery_failed("delivery timeout");
}

static void report_send_status_cb(esp_zb_zcl_command_send_status_message_t message)
{
    if (s_zigbee.delivery_stage == DELIVERY_IDLE || message.src_endpoint != WATERMETER_ENDPOINT) {
        return;
    }

    if (s_zigbee.resend_required) {
        s_zigbee.resend_required = false;
        start_delivery_sequence();
        return;
    }

    if (message.status != ESP_OK) {
        ESP_LOGW(TAG, "Report send status failed: %s", esp_err_to_name(message.status));
        esp_zb_scheduler_alarm_cancel((esp_zb_callback_t)delivery_retry_cb, 0);
        esp_zb_scheduler_alarm((esp_zb_callback_t)delivery_retry_cb, 0, DELIVERY_RETRY_INTERVAL_MS);
        return;
    }

    if (s_zigbee.delivery_stage == DELIVERY_CURRENT_SUMMATION) {
        s_zigbee.delivery_stage = DELIVERY_SCALED_SUMMATION;
        send_current_delivery_stage();
        return;
    }

    delivery_succeeded();
}

static void periodic_report_cb(uint8_t arg)
{
    (void)arg;
    bool include_battery =
        s_zigbee.force_battery_report || power_schedule_battery_due(&s_report_schedule, rtc_time_us());
    request_delivery(include_battery);
}

static void pulse_report_cb(uint8_t arg)
{
    (void)arg;
    s_zigbee.pulse_activity = true;
    s_zigbee.last_pulse_us = rtc_time_us();
    request_delivery(s_zigbee.force_battery_report ||
                     power_schedule_battery_due(&s_report_schedule, s_zigbee.last_pulse_us));
}

static void schedule_first_report(void)
{
    esp_zb_scheduler_alarm_cancel((esp_zb_callback_t)periodic_report_cb, 0);
    esp_zb_scheduler_alarm((esp_zb_callback_t)periodic_report_cb, 0, 0);
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
    request_delivery(true);
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
            .keep_alive = CONFIG_WATERMETER_ZIGBEE_KEEP_ALIVE_MS,
        },
    };
    esp_zb_init(&network_config);
    sleep_control_configure_zigbee();

    esp_zb_ep_list_t *endpoint_list = zigbee_clusters_create_endpoint();
    esp_zb_device_register(endpoint_list);
    esp_zb_core_action_handler_register(action_handler);
    esp_zb_zcl_command_send_status_handler_register(report_send_status_cb);
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
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_scheduler_alarm_cancel((esp_zb_callback_t)pulse_report_cb, 0);
    esp_zb_scheduler_alarm((esp_zb_callback_t)pulse_report_cb, 0, 0);
    esp_zb_lock_release();
}

esp_err_t zigbee_app_start(void)
{
    init_report_schedule();
    s_zigbee.force_battery_report = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_UNDEFINED;
    return xTaskCreate(zigbee_task, "Zigbee_main", 4096, NULL, 5, NULL) == pdPASS ? ESP_OK : ESP_FAIL;
}
