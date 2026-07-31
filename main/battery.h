#pragma once

#include <stdint.h>

#include "esp_err.h"

esp_err_t battery_init(void);
void battery_update_zigbee_attrs(uint8_t endpoint);
uint8_t *battery_voltage_zcl_attr(void);
uint8_t *battery_percent_zcl_attr(void);
