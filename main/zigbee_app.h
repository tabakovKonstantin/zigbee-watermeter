#pragma once

#include <stdbool.h>

#include "esp_err.h"

bool zigbee_app_is_joined(void);
void zigbee_app_report_sensor_pulse(void);
esp_err_t zigbee_app_start(void);
