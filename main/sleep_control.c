#include "sleep_control.h"

#include <inttypes.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_zigbee_cluster.h"
#include "esp_zigbee_core.h"
#include "soc/esp32c6/rtc.h"

#include "ota.h"
#include "xiao_board.h"

#define SENSOR_PIN ((gpio_num_t)CONFIG_WATERMETER_SENSOR_GPIO)
#define ZIGBEE_SLEEP_THRESHOLD_MS 1000
#define ZIGBEE_AWAKE_AFTER_PULSE_MS 3000
#define ZIGBEE_DEEP_AWAKE_AFTER_PULSE_MS CONFIG_WATERMETER_PULSE_GRACE_MS
#define SENSOR_WAKE_STATE_MAGIC 0x574D5357U
#define MIN_DEEP_SLEEP_MS 100

#if CONFIG_WATERMETER_SLEEP_MODE_DEEP && !GPIO_IS_DEEP_SLEEP_WAKEUP_VALID_GPIO(CONFIG_WATERMETER_SENSOR_GPIO)
#error "WATERMETER_SENSOR_GPIO must be a valid deep sleep wake GPIO in deep sleep mode"
#endif

static const char *TAG = "SLEEP_CONTROL";

static QueueHandle_t s_sensor_queue;
static bool s_joined;

#if CONFIG_WATERMETER_SLEEP_MODE_DEEP
RTC_DATA_ATTR static uint32_t s_sensor_wake_state_magic;
RTC_DATA_ATTR static uint8_t s_sensor_wake_level;
RTC_DATA_ATTR static uint64_t s_timer_deadline_us;

static uint32_t s_requested_wake_after_ms = CONFIG_WATERMETER_REPORT_INTERVAL_MS;

static void deep_sleep_enter_cb(uint8_t arg);
static void enter_deep_sleep(bool preserve_timer_deadline);
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

bool sleep_control_handle_early_wakeup(void)
{
#if CONFIG_WATERMETER_SLEEP_MODE_DEEP
    if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_GPIO ||
        s_sensor_wake_state_magic != SENSOR_WAKE_STATE_MAGIC ||
        s_sensor_wake_level == 0) {
        return false;
    }

    s_sensor_wake_state_magic = 0;
    gpio_config_t sensor_config = {
        .pin_bit_mask = 1ULL << SENSOR_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&sensor_config));
    ESP_LOGI(TAG, "Hall release wake; rearming without starting the application");
    enter_deep_sleep(true);
    return true;
#else
    return false;
#endif
}

static bool sleep_allowed_now(void)
{
    return s_joined && !ota_is_in_progress();
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
#if CONFIG_WATERMETER_SLEEP_MODE_DEEP
    if (!s_sensor_queue) {
        return;
    }

    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    int level = gpio_get_level(SENSOR_PIN);
    ESP_LOGI(TAG, "Wakeup check: cause=%s (%d) GPIO%d level=%d armed_level=%d",
             sleep_control_wakeup_cause_name(cause), cause, SENSOR_PIN, level, s_sensor_wake_level);

    if (cause != ESP_SLEEP_WAKEUP_GPIO || s_sensor_wake_state_magic != SENSOR_WAKE_STATE_MAGIC) {
        return;
    }

    uint8_t armed_level = s_sensor_wake_level;
    s_sensor_wake_state_magic = 0;
    if (armed_level != 0) {
        ESP_LOGI(TAG, "Hall sensor released; rearm without counting a pulse");
        return;
    }

    uint32_t gpio_num = SENSOR_PIN;
    if (xQueueSend(s_sensor_queue, &gpio_num, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Sensor queue full after GPIO wakeup");
    }
#endif
}

void sleep_control_finish_sensor_pulse(void)
{
    drain_sensor_queue();
    if (gpio_get_level(SENSOR_PIN) == 0) {
        ESP_LOGI(TAG, "GPIO%d remains low; next deep sleep wake will wait for release", SENSOR_PIN);
    }
}

#if CONFIG_WATERMETER_SLEEP_MODE_DEEP
static void enter_deep_sleep(bool preserve_timer_deadline)
{
    uint64_t now_us = esp_rtc_get_time_us();
    if (!preserve_timer_deadline || s_timer_deadline_us <= now_us) {
        s_timer_deadline_us = now_us + (uint64_t)s_requested_wake_after_ms * 1000ULL;
    }
    uint64_t timer_delay_us = s_timer_deadline_us - now_us;
    if (timer_delay_us < (uint64_t)MIN_DEEP_SLEEP_MS * 1000ULL) {
        timer_delay_us = (uint64_t)MIN_DEEP_SLEEP_MS * 1000ULL;
        s_timer_deadline_us = now_us + timer_delay_us;
    }

    int sensor_level = gpio_get_level(SENSOR_PIN);
    esp_deepsleep_gpio_wake_up_mode_t wake_mode =
        sensor_level == 0 ? ESP_GPIO_WAKEUP_GPIO_HIGH : ESP_GPIO_WAKEUP_GPIO_LOW;

    ESP_ERROR_CHECK(esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL));
    ESP_ERROR_CHECK(esp_deep_sleep_enable_gpio_wakeup(1ULL << SENSOR_PIN, wake_mode));
    ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(timer_delay_us));
    s_sensor_wake_level = sensor_level == 0 ? 1 : 0;
    s_sensor_wake_state_magic = SENSOR_WAKE_STATE_MAGIC;
    ESP_LOGI(TAG, "Entering deep sleep: GPIO%d %s wake, timer=%" PRIu64 " ms",
             SENSOR_PIN, s_sensor_wake_level == 0 ? "low" : "high", timer_delay_us / 1000ULL);
    xiao_board_prepare_deep_sleep();
    esp_deep_sleep_start();
}

static void deep_sleep_enter_cb(uint8_t arg)
{
    (void)arg;

    if (!sleep_allowed_now()) {
        ESP_LOGI(TAG, "Deep sleep postponed: joined=%s ota=%s",
                 s_joined ? "true" : "false",
                 ota_is_in_progress() ? "true" : "false");
        esp_zb_scheduler_alarm_cancel((esp_zb_callback_t)deep_sleep_enter_cb, 0);
        esp_zb_scheduler_alarm((esp_zb_callback_t)deep_sleep_enter_cb, 0, 1000);
        return;
    }

    drain_sensor_queue();
    enter_deep_sleep(false);
}
#endif

void sleep_control_cancel_pending_sleep(void)
{
#if CONFIG_WATERMETER_SLEEP_MODE_DEEP
    esp_zb_scheduler_alarm_cancel((esp_zb_callback_t)deep_sleep_enter_cb, 0);
#endif
}

void sleep_control_schedule_deep_sleep(uint32_t delay_ms, uint32_t wake_after_ms)
{
#if CONFIG_WATERMETER_SLEEP_MODE_DEEP
    s_requested_wake_after_ms = wake_after_ms;
    sleep_control_cancel_pending_sleep();
    esp_zb_scheduler_alarm((esp_zb_callback_t)deep_sleep_enter_cb, 0, delay_ms);
#else
    (void)delay_ms;
    (void)wake_after_ms;
#endif
}

void sleep_control_schedule_after_report(uint32_t delay_ms)
{
    sleep_control_schedule_deep_sleep(delay_ms, CONFIG_WATERMETER_REPORT_INTERVAL_MS);
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
