#include "battery.h"

#include <inttypes.h>

#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_zigbee_attribute.h"
#include "zcl/esp_zigbee_zcl_common.h"
#include "zcl/esp_zigbee_zcl_power_config.h"

#include "battery_math.h"

#define BATTERY_ADC_CHANNEL ADC_CHANNEL_0
#define BATTERY_ADC_DISCARD_SAMPLES 1
#define BATTERY_ADC_AVG_SAMPLES 16
#define BATTERY_MAX_REASONABLE_MV 4300U

static const char *TAG = "BATTERY";

typedef struct {
    adc_oneshot_unit_handle_t adc_handle;
    uint8_t voltage_zcl;
    uint8_t percent_zcl;
} battery_state_t;

static battery_state_t s_battery;

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

esp_err_t battery_init(void)
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

void battery_update_zigbee_attrs(uint8_t endpoint)
{
    uint32_t battery_mv = battery_read_mv();
    if (battery_mv == 0) {
        return;
    }

    uint8_t percent = battery_percent_from_mv(battery_mv);
    s_battery.voltage_zcl = (uint8_t)((battery_mv + 50U) / 100U);
    s_battery.percent_zcl = (uint8_t)(percent * 2U);

    esp_zb_zcl_set_attribute_val(endpoint,
                                 ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_ID,
                                 &s_battery.voltage_zcl,
                                 false);

    esp_zb_zcl_set_attribute_val(endpoint,
                                 ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID,
                                 &s_battery.percent_zcl,
                                 false);

    ESP_LOGI(TAG, "Battery: %" PRIu32 "mV %u%%", battery_mv, percent);
}

uint8_t *battery_voltage_zcl_attr(void)
{
    return &s_battery.voltage_zcl;
}

uint8_t *battery_percent_zcl_attr(void)
{
    return &s_battery.percent_zcl;
}
