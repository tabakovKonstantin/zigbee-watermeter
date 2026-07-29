#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_zigbee_core.h"
#include "zcl/esp_zigbee_zcl_command.h"
#include "zcl/esp_zigbee_zcl_ota.h"

#define OTA_MANUFACTURER_CODE 0x131B
#define OTA_IMAGE_TYPE 0x0001
#define OTA_QUERY_INTERVAL_MINUTES 60
#define OTA_MAX_DATA_SIZE 0xff

#ifndef WATERMETER_OTA_FILE_VERSION
#define WATERMETER_OTA_FILE_VERSION 1
#endif

typedef void (*ota_activity_callback_t)(bool active);

void ota_set_activity_callback(ota_activity_callback_t callback);
bool ota_is_in_progress(void);
esp_err_t ota_mark_running_app_valid(void);
esp_err_t ota_upgrade_handler(esp_zb_zcl_ota_upgrade_value_message_t *message);
