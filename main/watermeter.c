#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_zigbee_core.h"
#include "nvs_flash.h"

#include "battery.h"
#include "meter_state.h"
#include "ota.h"
#include "sensor.h"
#include "sleep_control.h"
#include "xiao_board.h"
#include "zigbee_app.h"

#define SENSOR_PIN CONFIG_WATERMETER_SENSOR_GPIO

static const char *TAG = "WATERMETER";

void app_main(void)
{
    esp_zb_platform_config_t config = {
        .radio_config = { .radio_mode = ZB_RADIO_MODE_NATIVE },
        .host_config = { .host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE },
    };

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_LOGI(TAG, "app_main: nvs ok");
    esp_sleep_wakeup_cause_t boot_wakeup_cause = esp_sleep_get_wakeup_cause();
    ESP_LOGI(TAG, "app_main: boot wake cause=%s (%d)",
             sleep_control_wakeup_cause_name(boot_wakeup_cause), boot_wakeup_cause);

    ota_set_activity_callback(sleep_control_set_ota_active);
    ESP_ERROR_CHECK(ota_mark_running_app_valid());
    ESP_LOGI(TAG, "app_main: firmware=%s ota_file_version=%d", esp_app_get_description()->version,
             WATERMETER_OTA_FILE_VERSION);

    ESP_ERROR_CHECK(meter_state_load());
    ESP_LOGI(TAG, "app_main: meter state ok");

    ESP_ERROR_CHECK(battery_init());
    ESP_LOGI(TAG, "app_main: battery adc ok on ADC1 channel 0");

    ESP_ERROR_CHECK(sleep_control_init_power_management());
    ESP_LOGI(TAG, "app_main: power management %d-%d MHz", CONFIG_WATERMETER_PM_MIN_FREQ_MHZ,
             CONFIG_WATERMETER_PM_MAX_FREQ_MHZ);

    ESP_ERROR_CHECK(xiao_board_enable_external_antenna());
    ESP_ERROR_CHECK(esp_zb_platform_config(&config));
    ESP_LOGI(TAG, "app_main: zigbee platform config ok");

    const sensor_callbacks_t sensor_callbacks = {
        .is_zigbee_joined = zigbee_app_is_joined,
        .report_pulse = zigbee_app_report_sensor_pulse,
    };
    ESP_ERROR_CHECK(sensor_start(&sensor_callbacks));
    ESP_LOGI(TAG, "app_main: sensor task created");

    ESP_ERROR_CHECK(zigbee_app_start());
    ESP_LOGI(TAG, "app_main: zigbee task created");

    ESP_LOGI(TAG, "Sleep mode=%s sensor GPIO=%d", sleep_control_mode_name(), SENSOR_PIN);
}
