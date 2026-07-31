#pragma once

#include <stdint.h>

typedef struct __attribute__((packed)) {
    uint16_t low;
    uint8_t high;
} esp_zb_uint24_t;

typedef struct __attribute__((packed)) {
    uint32_t low;
    uint16_t high;
} esp_zb_uint48_t;

_Static_assert(sizeof(esp_zb_uint24_t) == 3, "esp_zb_uint24_t host stub must remain packed");
_Static_assert(sizeof(esp_zb_uint48_t) == 6, "esp_zb_uint48_t host stub must remain packed");
