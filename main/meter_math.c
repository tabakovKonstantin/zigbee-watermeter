#include "meter_math.h"

esp_zb_uint24_t uint32_to_zb_u24(uint32_t value)
{
    esp_zb_uint24_t out = {
        .low = (uint16_t)(value & 0xFFFF),
        .high = (uint8_t)((value >> 16) & 0xFF),
    };
    return out;
}

uint32_t zb_u24_to_uint32(const esp_zb_uint24_t *value)
{
    return ((uint32_t)value->high << 16) | value->low;
}

esp_zb_uint48_t uint64_to_zb_u48(uint64_t value)
{
    esp_zb_uint48_t out = {
        .low = (uint32_t)(value & 0xFFFFFFFFULL),
        .high = (uint16_t)((value >> 32) & 0xFFFF),
    };
    return out;
}

uint64_t meter_scaled_summation(uint64_t pulse_count, uint32_t multiplier, uint32_t divisor)
{
    if (divisor == 0) {
        divisor = 1;
    }
    return (pulse_count * multiplier) / divisor;
}
