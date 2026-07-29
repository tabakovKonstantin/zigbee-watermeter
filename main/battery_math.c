#include "battery_math.h"

#define BATTERY_ADC_CAL_RAW 2515U
#define BATTERY_ADC_CAL_GPIO_MV 2370U
#define BATTERY_DIVIDER_GPIO_MV 2270U
#define BATTERY_DIVIDER_BATTERY_MV 4830U
#define BATTERY_EMPTY_MV 3300U
#define BATTERY_FULL_MV 4200U

uint8_t battery_percent_from_mv(uint32_t battery_mv)
{
    if (battery_mv >= BATTERY_FULL_MV) {
        return 100;
    }
    if (battery_mv <= BATTERY_EMPTY_MV) {
        return 0;
    }
    return (uint8_t)(((battery_mv - BATTERY_EMPTY_MV) * 100) / (BATTERY_FULL_MV - BATTERY_EMPTY_MV));
}

uint32_t battery_gpio_mv_from_raw(uint32_t raw)
{
    return (raw * BATTERY_ADC_CAL_GPIO_MV + (BATTERY_ADC_CAL_RAW / 2U)) / BATTERY_ADC_CAL_RAW;
}

uint32_t battery_mv_from_gpio_mv(uint32_t gpio_mv)
{
    return (gpio_mv * BATTERY_DIVIDER_BATTERY_MV + (BATTERY_DIVIDER_GPIO_MV / 2U)) / BATTERY_DIVIDER_GPIO_MV;
}
