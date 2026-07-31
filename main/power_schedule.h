#pragma once

#include <stdbool.h>
#include <stdint.h>

#define POWER_SCHEDULE_RETRY_LEVELS 4

typedef struct {
    uint64_t meter_due_us;
    uint64_t battery_due_us;
    uint8_t retry_index;
} power_schedule_t;

void power_schedule_reset(power_schedule_t *schedule);
bool power_schedule_battery_due(const power_schedule_t *schedule, uint64_t now_us);
void power_schedule_report_succeeded(power_schedule_t *schedule,
                                     uint64_t now_us,
                                     uint32_t meter_interval_ms,
                                     uint32_t battery_interval_ms,
                                     bool battery_attempted);
uint32_t power_schedule_report_failed(power_schedule_t *schedule,
                                      const uint32_t retry_delays_ms[POWER_SCHEDULE_RETRY_LEVELS]);
uint32_t power_schedule_next_wake_ms(const power_schedule_t *schedule, uint64_t now_us);
