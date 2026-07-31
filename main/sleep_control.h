#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

const char *sleep_control_mode_name(void);
const char *sleep_control_wakeup_cause_name(esp_sleep_wakeup_cause_t cause);
bool sleep_control_handle_early_wakeup(void);
void sleep_control_set_sensor_queue(QueueHandle_t queue);
void sleep_control_set_joined(bool joined);
void sleep_control_set_ota_active(bool active);
void sleep_control_configure_zigbee(void);
esp_err_t sleep_control_init_power_management(void);
void sleep_control_keep_awake_for_pulse_report(void);
void sleep_control_finish_sensor_pulse(void);
void sleep_control_enqueue_sensor_wakeup(void);
void sleep_control_cancel_pending_sleep(void);
void sleep_control_schedule_deep_sleep(uint32_t delay_ms, uint32_t wake_after_ms);
void sleep_control_handle_can_sleep(uint32_t *signal, esp_err_t status);
