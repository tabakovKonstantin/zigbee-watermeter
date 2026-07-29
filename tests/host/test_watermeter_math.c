#include <assert.h>
#include <stdint.h>

#include "battery_math.h"
#include "meter_math.h"

static void test_u24_conversion(void)
{
    const uint32_t values[] = { 0, 1, 0xFFFF, 0x123456, 0xFFFFFF };

    for (unsigned int i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        esp_zb_uint24_t encoded = uint32_to_zb_u24(values[i]);
        assert(zb_u24_to_uint32(&encoded) == values[i]);
    }

    esp_zb_uint24_t truncated = uint32_to_zb_u24(0xA5123456);
    assert(zb_u24_to_uint32(&truncated) == 0x123456);
}

static void test_u48_conversion(void)
{
    esp_zb_uint48_t encoded = uint64_to_zb_u48(UINT64_C(0xABCD12345678));
    assert(encoded.low == UINT32_C(0x12345678));
    assert(encoded.high == UINT16_C(0xABCD));

    encoded = uint64_to_zb_u48(UINT64_C(0xFFFFABCD12345678));
    assert(encoded.low == UINT32_C(0x12345678));
    assert(encoded.high == UINT16_C(0xABCD));
}

static void test_scaled_summation(void)
{
    assert(meter_scaled_summation(0, 10, 1) == 0);
    assert(meter_scaled_summation(123, 10, 1) == 1230);
    assert(meter_scaled_summation(123, 10, 4) == 307);
    assert(meter_scaled_summation(123, 10, 0) == 1230);
}

static void test_battery_percent(void)
{
    assert(battery_percent_from_mv(3200) == 0);
    assert(battery_percent_from_mv(3300) == 0);
    assert(battery_percent_from_mv(3750) == 50);
    assert(battery_percent_from_mv(4199) == 99);
    assert(battery_percent_from_mv(4200) == 100);
    assert(battery_percent_from_mv(4300) == 100);
}

static void test_battery_calibration(void)
{
    assert(battery_gpio_mv_from_raw(0) == 0);
    assert(battery_gpio_mv_from_raw(2515) == 2370);
    assert(battery_mv_from_gpio_mv(0) == 0);
    assert(battery_mv_from_gpio_mv(2270) == 4830);
}

int main(void)
{
    test_u24_conversion();
    test_u48_conversion();
    test_scaled_summation();
    test_battery_percent();
    test_battery_calibration();
    return 0;
}
