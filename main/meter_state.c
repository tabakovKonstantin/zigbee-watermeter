#include "meter_state.h"

#include <inttypes.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"

#define DEFAULT_MULTIPLIER 10
#define DEFAULT_DIVISOR 1

#define NVS_NAMESPACE "watermeter"
#define NVS_KEY_PULSE_COUNT "pulse_count"
#define NVS_KEY_MULTIPLIER "multiplier"
#define NVS_KEY_DIVISOR "divisor"

static const char *TAG = "METER_STATE";

static meter_state_t s_meter = {
    .pulse_count = 0,
    .multiplier = DEFAULT_MULTIPLIER,
    .divisor = DEFAULT_DIVISOR,
};
static portMUX_TYPE s_meter_mux = portMUX_INITIALIZER_UNLOCKED;
static nvs_handle_t s_nvs;

esp_err_t meter_state_save(void)
{
    ESP_RETURN_ON_ERROR(nvs_set_u64(s_nvs, NVS_KEY_PULSE_COUNT, s_meter.pulse_count), TAG, "save pulse_count");
    ESP_RETURN_ON_ERROR(nvs_set_u32(s_nvs, NVS_KEY_MULTIPLIER, s_meter.multiplier), TAG, "save multiplier");
    ESP_RETURN_ON_ERROR(nvs_set_u32(s_nvs, NVS_KEY_DIVISOR, s_meter.divisor), TAG, "save divisor");
    return nvs_commit(s_nvs);
}

esp_err_t meter_state_load(void)
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

void meter_state_snapshot(meter_state_t *out)
{
    portENTER_CRITICAL(&s_meter_mux);
    *out = s_meter;
    portEXIT_CRITICAL(&s_meter_mux);
}

uint64_t meter_state_increment_pulse(void)
{
    portENTER_CRITICAL(&s_meter_mux);
    s_meter.pulse_count++;
    uint64_t pulse_count = s_meter.pulse_count;
    portEXIT_CRITICAL(&s_meter_mux);
    return pulse_count;
}

void meter_state_set_multiplier(uint32_t value)
{
    portENTER_CRITICAL(&s_meter_mux);
    s_meter.multiplier = value;
    portEXIT_CRITICAL(&s_meter_mux);
}

void meter_state_set_divisor(uint32_t value)
{
    portENTER_CRITICAL(&s_meter_mux);
    s_meter.divisor = value;
    portEXIT_CRITICAL(&s_meter_mux);
}
