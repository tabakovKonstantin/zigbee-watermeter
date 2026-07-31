#pragma once

#include <stdint.h>

#include "esp_zigbee_type.h"

esp_zb_uint24_t uint32_to_zb_u24(uint32_t value);
uint32_t zb_u24_to_uint32(const esp_zb_uint24_t *value);
esp_zb_uint48_t uint64_to_zb_u48(uint64_t value);
uint64_t meter_scaled_summation(uint64_t pulse_count, uint32_t multiplier, uint32_t divisor);
