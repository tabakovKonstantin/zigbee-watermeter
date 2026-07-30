#pragma once

#include <stdbool.h>

#include "esp_zigbee_core.h"

#define WATERMETER_ENDPOINT 1
#define ATTR_SCALED_SUMMATION_ID 0xFC00
#define ATTR_SCALE_MULTIPLIER_ID 0xFC01
#define ATTR_SCALE_DIVISOR_ID 0xFC02

esp_zb_ep_list_t *zigbee_clusters_create_endpoint(void);
void zigbee_clusters_update_meter(bool send_report, bool include_battery);
