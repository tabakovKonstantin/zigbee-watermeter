#include "battery_math.h"

#ifndef CONFIG_WATERMETER_BATTERY_EMPTY_MV
#define CONFIG_WATERMETER_BATTERY_EMPTY_MV 3300
#endif

#ifndef CONFIG_WATERMETER_BATTERY_FULL_MV
#define CONFIG_WATERMETER_BATTERY_FULL_MV 4200
#endif

uint8_t battery_percent_from_mv(uint32_t battery_mv)
{
    if (battery_mv >= CONFIG_WATERMETER_BATTERY_FULL_MV) {
        return 100;
    }
    if (battery_mv <= CONFIG_WATERMETER_BATTERY_EMPTY_MV) {
        return 0;
    }
    return (uint8_t)(((battery_mv - CONFIG_WATERMETER_BATTERY_EMPTY_MV) * 100) /
                     (CONFIG_WATERMETER_BATTERY_FULL_MV - CONFIG_WATERMETER_BATTERY_EMPTY_MV));
}

uint32_t battery_mv_from_gpio_mv(uint32_t gpio_mv, uint32_t top_ohm, uint32_t bottom_ohm)
{
    if (bottom_ohm == 0) {
        return 0;
    }
    uint64_t numerator = (uint64_t)gpio_mv * (top_ohm + bottom_ohm) + (bottom_ohm / 2U);
    return (uint32_t)(numerator / bottom_ohm);
}
