#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef struct {
    bool (*is_zigbee_joined)(void);
    void (*report_pulse)(void);
} sensor_callbacks_t;

esp_err_t sensor_start(const sensor_callbacks_t *callbacks);
