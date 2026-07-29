#pragma once

#include <stdint.h>

uint8_t battery_percent_from_mv(uint32_t battery_mv);
uint32_t battery_gpio_mv_from_raw(uint32_t raw);
uint32_t battery_mv_from_gpio_mv(uint32_t gpio_mv);
