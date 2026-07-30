#include "sensor.h"

#include <inttypes.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "meter_state.h"
#include "sleep_control.h"

#define SENSOR_PIN ((gpio_num_t)CONFIG_WATERMETER_SENSOR_GPIO)
#define SENSOR_QUEUE_LEN 8
#define DEBOUNCE_US (100 * 1000)
#define ZIGBEE_WAKE_BEFORE_REPORT_MS 200

static const char *TAG = "SENSOR";

static QueueHandle_t s_sensor_queue;
static sensor_callbacks_t s_callbacks;

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
    (void)pvParameters;

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << SENSOR_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
#if CONFIG_WATERMETER_SLEEP_MODE_LIGHT || CONFIG_WATERMETER_SLEEP_MODE_DEEP
    ESP_ERROR_CHECK(gpio_wakeup_enable(SENSOR_PIN, GPIO_INTR_LOW_LEVEL));
    ESP_ERROR_CHECK(esp_sleep_enable_gpio_wakeup());
#endif
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(SENSOR_PIN, sensor_isr_handler, (void *)SENSOR_PIN));
#if CONFIG_WATERMETER_SLEEP_MODE_DEEP
    sleep_control_enqueue_sensor_wakeup();
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

        uint64_t pulses = meter_state_increment_pulse();
        esp_err_t err = meter_state_save();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save pulse count: %s", esp_err_to_name(err));
        }

        ESP_LOGI(TAG, "Pulse counted on GPIO%" PRIu32 ", total=%" PRIu64, gpio_num, pulses);
        if (s_callbacks.is_zigbee_joined()) {
            sleep_control_keep_awake_for_pulse_report();
            vTaskDelay(pdMS_TO_TICKS(ZIGBEE_WAKE_BEFORE_REPORT_MS));
        }
        s_callbacks.report_pulse();
        sleep_control_finish_sensor_pulse();
        last_pulse_us = esp_timer_get_time();
    }
}

esp_err_t sensor_start(const sensor_callbacks_t *callbacks)
{
    if (!callbacks || !callbacks->is_zigbee_joined || !callbacks->report_pulse) {
        return ESP_ERR_INVALID_ARG;
    }
    s_callbacks = *callbacks;

    s_sensor_queue = xQueueCreate(SENSOR_QUEUE_LEN, sizeof(uint32_t));
    if (!s_sensor_queue) {
        return ESP_ERR_NO_MEM;
    }
    sleep_control_set_sensor_queue(s_sensor_queue);

    return xTaskCreate(sensor_task, "sensor", 4096, NULL, 6, NULL) == pdPASS ? ESP_OK : ESP_FAIL;
}
