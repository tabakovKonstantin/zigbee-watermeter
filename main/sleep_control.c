#include "sleep_control.h"

#include <inttypes.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "esp_zigbee_cluster.h"
#include "esp_zigbee_core.h"
#include "freertos/task.h"

#include "ota.h"
#include "xiao_board.h"

#define SENSOR_PIN ((gpio_num_t)CONFIG_WATERMETER_SENSOR_GPIO)
#define ZIGBEE_SLEEP_THRESHOLD_MS 1000
#define ZIGBEE_AWAKE_AFTER_PULSE_MS 3000
#define ZIGBEE_DEEP_AWAKE_AFTER_PULSE_MS CONFIG_WATERMETER_DEEP_AWAKE_AFTER_PULSE_MS
#define SENSOR_RELEASE_POLL_MS 20
#define SENSOR_RELEASE_STABLE_MS 50
#define SENSOR_RELEASE_TIMEOUT_MS (30 * 1000)

#ifndef CONFIG_WATERMETER_DEEP_SLEEP_TIMER_WAKE_MS
#define CONFIG_WATERMETER_DEEP_SLEEP_TIMER_WAKE_MS 600000
#endif
#ifndef CONFIG_WATERMETER_DEEP_AWAKE_AFTER_PULSE_MS
#define CONFIG_WATERMETER_DEEP_AWAKE_AFTER_PULSE_MS 8000
#endif

#if CONFIG_WATERMETER_SLEEP_MODE_DEEP && !GPIO_IS_DEEP_SLEEP_WAKEUP_VALID_GPIO(CONFIG_WATERMETER_SENSOR_GPIO)
#error "WATERMETER_SENSOR_GPIO must be a valid deep sleep wake GPIO in deep sleep mode"
#endif

static const char *TAG = "SLEEP_CONTROL";

static QueueHandle_t s_sensor_queue;
static bool s_joined;

#if CONFIG_WATERMETER_SLEEP_MODE_DEEP
static void deep_sleep_enter_cb(uint8_t arg);
#endif

const char *sleep_control_wakeup_cause_name(esp_sleep_wakeup_cause_t cause)
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

const char *sleep_control_mode_name(void)
{
#if CONFIG_WATERMETER_SLEEP_MODE_LIGHT
    return "light";
#elif CONFIG_WATERMETER_SLEEP_MODE_DEEP
    return "deep";
#else
    return "off";
#endif
}

static bool sleep_allowed_now(void)
{
    if (!s_joined || ota_is_in_progress()) {
        return false;
    }
    return gpio_get_level(SENSOR_PIN) != 0;
}

static void drain_sensor_queue(void)
{
    uint32_t ignored_gpio = 0;
    uint32_t drained = 0;

    while (s_sensor_queue && xQueueReceive(s_sensor_queue, &ignored_gpio, 0) == pdTRUE) {
        drained++;
    }
    if (drained > 0) {
        ESP_LOGI(TAG, "Drained %" PRIu32 " queued duplicate sensor events", drained);
    }
}

#if CONFIG_WATERMETER_SLEEP_MODE_LIGHT
static void enable_zigbee_sleep_cb(uint8_t arg)
{
    (void)arg;
    if (ota_is_in_progress()) {
        ESP_LOGI(TAG, "Keep Zigbee sleep disabled during OTA");
        return;
    }
    ESP_LOGI(TAG, "Re-enable Zigbee sleep after pulse report");
    esp_zb_sleep_enable(true);
}
#endif

void sleep_control_set_sensor_queue(QueueHandle_t queue)
{
    s_sensor_queue = queue;
}

void sleep_control_set_joined(bool joined)
{
    s_joined = joined;
}

void sleep_control_set_ota_active(bool active)
{
#if CONFIG_WATERMETER_SLEEP_MODE_LIGHT
    esp_zb_sleep_enable(!active);
#elif CONFIG_WATERMETER_SLEEP_MODE_DEEP
    if (active) {
        esp_zb_scheduler_alarm_cancel((esp_zb_callback_t)deep_sleep_enter_cb, 0);
    }
#else
    (void)active;
#endif
}

void sleep_control_configure_zigbee(void)
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
             sleep_control_mode_name(), esp_zb_get_rx_on_when_idle() ? "true" : "false");
#endif
}

esp_err_t sleep_control_init_power_management(void)
{
#if CONFIG_WATERMETER_SLEEP_MODE_LIGHT || CONFIG_WATERMETER_SLEEP_MODE_DEEP
    const esp_pm_config_t pm_config = {
        .max_freq_mhz = CONFIG_WATERMETER_PM_MAX_FREQ_MHZ,
        .min_freq_mhz = CONFIG_WATERMETER_PM_MIN_FREQ_MHZ,
        .light_sleep_enable = true,
    };
    return esp_pm_configure(&pm_config);
#else
    return ESP_OK;
#endif
}

void sleep_control_keep_awake_for_pulse_report(void)
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

void sleep_control_enqueue_sensor_wakeup(void)
{
#if CONFIG_WATERMETER_SLEEP_MODE_LIGHT || CONFIG_WATERMETER_SLEEP_MODE_DEEP
    if (!s_sensor_queue) {
        return;
    }

    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    int level = gpio_get_level(SENSOR_PIN);
    ESP_LOGI(TAG, "Wakeup check: cause=%s (%d) GPIO%d level=%d",
             sleep_control_wakeup_cause_name(cause), cause, SENSOR_PIN, level);

    if (cause == ESP_SLEEP_WAKEUP_GPIO && level == 0) {
        uint32_t gpio_num = SENSOR_PIN;
        if (xQueueSend(s_sensor_queue, &gpio_num, 0) != pdTRUE) {
            ESP_LOGW(TAG, "Sensor queue full after GPIO wakeup");
        }
    }
#endif
}

void sleep_control_finish_sensor_pulse(void)
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
        sleep_control_schedule_after_report(ZIGBEE_DEEP_AWAKE_AFTER_PULSE_MS);
#endif
        return;
    }

    ESP_LOGI(TAG, "GPIO%d released high, Zigbee sleep can resume", SENSOR_PIN);
#if CONFIG_WATERMETER_SLEEP_MODE_LIGHT
    if (sleep_allowed_now()) {
        esp_zb_sleep_enable(true);
    }
#endif
}

#if CONFIG_WATERMETER_SLEEP_MODE_DEEP
static void deep_sleep_enter_cb(uint8_t arg)
{
    (void)arg;

    if (!sleep_allowed_now()) {
        ESP_LOGI(TAG, "Deep sleep postponed: joined=%s ota=%s GPIO%d=%d",
                 s_joined ? "true" : "false",
                 ota_is_in_progress() ? "true" : "false",
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
    xiao_board_prepare_deep_sleep();
    esp_deep_sleep_start();
}
#endif

void sleep_control_schedule_after_report(uint32_t delay_ms)
{
#if CONFIG_WATERMETER_SLEEP_MODE_DEEP
    esp_zb_scheduler_alarm_cancel((esp_zb_callback_t)deep_sleep_enter_cb, 0);
    esp_zb_scheduler_alarm((esp_zb_callback_t)deep_sleep_enter_cb, 0, delay_ms);
#else
    (void)delay_ms;
#endif
}

void sleep_control_handle_can_sleep(uint32_t *signal, esp_err_t status)
{
#if CONFIG_WATERMETER_SLEEP_MODE_LIGHT
    esp_zb_zdo_signal_can_sleep_params_t *sleep_params =
        (esp_zb_zdo_signal_can_sleep_params_t *)esp_zb_app_signal_get_params(signal);
    if (s_joined && status == ESP_OK) {
        if (gpio_get_level(SENSOR_PIN) == 0) {
            ESP_LOGI(TAG, "Skip Zigbee sleep while GPIO%d is low", SENSOR_PIN);
            return;
        }
        ESP_LOGI(TAG, "Zigbee stack can sleep for %" PRIu32 " ms", sleep_params->sleep_duration);
        esp_zb_sleep_now();
        esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
        ESP_LOGI(TAG, "Returned from Zigbee sleep, wake cause=%s (%d)",
                 sleep_control_wakeup_cause_name(cause), cause);
        sleep_control_enqueue_sensor_wakeup();
    }
#else
    (void)signal;
    (void)status;
#endif
}
