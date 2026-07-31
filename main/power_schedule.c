#include "power_schedule.h"

#include <limits.h>
#include <stddef.h>

void power_schedule_reset(power_schedule_t *schedule)
{
    if (!schedule) {
        return;
    }

    *schedule = (power_schedule_t){ 0 };
}

bool power_schedule_battery_due(const power_schedule_t *schedule, uint64_t now_us)
{
    return !schedule || schedule->battery_due_us == 0 || now_us >= schedule->battery_due_us;
}

void power_schedule_report_succeeded(power_schedule_t *schedule,
                                     uint64_t now_us,
                                     uint32_t meter_interval_ms,
                                     uint32_t battery_interval_ms,
                                     bool battery_attempted)
{
    if (!schedule) {
        return;
    }

    schedule->meter_due_us = now_us + (uint64_t)meter_interval_ms * 1000ULL;
    if (battery_attempted) {
        schedule->battery_due_us = now_us + (uint64_t)battery_interval_ms * 1000ULL;
    }
    schedule->retry_index = 0;
}

uint32_t power_schedule_report_failed(power_schedule_t *schedule,
                                      const uint32_t retry_delays_ms[POWER_SCHEDULE_RETRY_LEVELS])
{
    if (!schedule || !retry_delays_ms) {
        return 0;
    }

    uint8_t retry_index = schedule->retry_index;
    if (retry_index >= POWER_SCHEDULE_RETRY_LEVELS) {
        retry_index = POWER_SCHEDULE_RETRY_LEVELS - 1;
    }

    uint32_t delay_ms = retry_delays_ms[retry_index];
    if (schedule->retry_index < POWER_SCHEDULE_RETRY_LEVELS - 1) {
        schedule->retry_index++;
    }
    return delay_ms;
}

uint32_t power_schedule_next_wake_ms(const power_schedule_t *schedule, uint64_t now_us)
{
    if (!schedule) {
        return 0;
    }

    uint64_t due_us = schedule->meter_due_us;
    if (due_us == 0 || (schedule->battery_due_us != 0 && schedule->battery_due_us < due_us)) {
        due_us = schedule->battery_due_us;
    }
    if (due_us <= now_us) {
        return 0;
    }

    uint64_t delay_ms = (due_us - now_us + 999ULL) / 1000ULL;
    return delay_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)delay_ms;
}
