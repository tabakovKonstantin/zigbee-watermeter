#pragma once

#include <stdint.h>

uint8_t battery_percent_from_mv(uint32_t battery_mv);
uint32_t battery_mv_from_gpio_mv(uint32_t gpio_mv, uint32_t top_ohm, uint32_t bottom_ohm);
