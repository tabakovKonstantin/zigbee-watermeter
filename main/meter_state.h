#pragma once

#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint64_t pulse_count;
    uint32_t multiplier;
    uint32_t divisor;
} meter_state_t;

esp_err_t meter_state_load(void);
esp_err_t meter_state_save(void);
void meter_state_snapshot(meter_state_t *out);
uint64_t meter_state_increment_pulse(void);
void meter_state_set_multiplier(uint32_t value);
void meter_state_set_divisor(uint32_t value);
