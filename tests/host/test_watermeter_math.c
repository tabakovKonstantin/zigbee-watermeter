#include <assert.h>
#include <stdint.h>

#include "battery_math.h"
#include "meter_math.h"
#include "power_schedule.h"

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
    assert(battery_mv_from_gpio_mv(0, 200000, 200000) == 0);
    assert(battery_mv_from_gpio_mv(2100, 200000, 200000) == 4200);
    assert(battery_mv_from_gpio_mv(1650, 200000, 200000) == 3300);
    assert(battery_mv_from_gpio_mv(2100, 100000, 200000) == 3150);
    assert(battery_mv_from_gpio_mv(2100, 200000, 0) == 0);
}

static void test_power_schedule(void)
{
    power_schedule_t schedule;
    power_schedule_reset(&schedule);
    assert(power_schedule_battery_due(&schedule, 1000));

    power_schedule_report_succeeded(&schedule, 1000, 3300000, 21600000, true);
    assert(!power_schedule_battery_due(&schedule, 1000));
    assert(power_schedule_next_wake_ms(&schedule, 1000) == 3300000);

    power_schedule_report_succeeded(&schedule, 2000, 3300000, 21600000, false);
    assert(schedule.battery_due_us == 21600001000ULL);
    assert(power_schedule_next_wake_ms(&schedule, 2000) == 3300000);
}

static void test_power_schedule_retry(void)
{
    const uint32_t retry_delays_ms[POWER_SCHEDULE_RETRY_LEVELS] = { 300000, 900000, 1800000, 3300000 };
    power_schedule_t schedule;
    power_schedule_reset(&schedule);

    assert(power_schedule_report_failed(&schedule, retry_delays_ms) == 300000);
    assert(power_schedule_report_failed(&schedule, retry_delays_ms) == 900000);
    assert(power_schedule_report_failed(&schedule, retry_delays_ms) == 1800000);
    assert(power_schedule_report_failed(&schedule, retry_delays_ms) == 3300000);
    assert(power_schedule_report_failed(&schedule, retry_delays_ms) == 3300000);
    assert(schedule.retry_index == POWER_SCHEDULE_RETRY_LEVELS - 1);

    power_schedule_report_succeeded(&schedule, 0, 3300000, 21600000, false);
    assert(schedule.retry_index == 0);
}

int main(void)
{
    test_u24_conversion();
    test_u48_conversion();
    test_scaled_summation();
    test_battery_percent();
    test_battery_calibration();
    test_power_schedule();
    test_power_schedule_retry();
    return 0;
}
