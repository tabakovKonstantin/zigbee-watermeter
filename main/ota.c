#include "ota.h"

#include <inttypes.h>
#include <stddef.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"

#define HA_ENDPOINT 1
#define OTA_HEADER_LENGTH 56

static const char *TAG = "OTA";

typedef struct {
    esp_ota_handle_t handle;
    const esp_partition_t *partition;
    uint32_t received;
    uint32_t expected_size;
    uint32_t file_version;
    bool in_progress;
} ota_state_t;

static ota_state_t s_ota;
static ota_activity_callback_t s_activity_callback;

static void ota_reset_state(void)
{
    if (s_ota.in_progress && s_ota.handle) {
        esp_ota_abort(s_ota.handle);
    }
    s_ota.handle = 0;
    s_ota.partition = NULL;
    s_ota.received = 0;
    s_ota.expected_size = 0;
    s_ota.file_version = 0;
    s_ota.in_progress = false;
    if (s_activity_callback) {
        s_activity_callback(false);
    }
}

static uint16_t ota_accept_status(const esp_zb_zcl_ota_upgrade_value_message_t *message)
{
    if (!message || message->info.dst_endpoint != HA_ENDPOINT) {
        return ESP_ZB_ZCL_OTA_UPGRADE_STATUS_ERROR;
    }
    if (message->ota_header.manufacturer_code != OTA_MANUFACTURER_CODE ||
        message->ota_header.image_type != OTA_IMAGE_TYPE ||
        message->ota_header.file_version <= WATERMETER_OTA_FILE_VERSION) {
        ESP_LOGW(TAG, "Reject OTA image: manufacturer=0x%04x image_type=0x%04x file_version=%" PRIu32,
                 message->ota_header.manufacturer_code,
                 message->ota_header.image_type,
                 message->ota_header.file_version);
        return ESP_ZB_ZCL_OTA_UPGRADE_STATUS_ERROR;
    }
    return ESP_ZB_ZCL_OTA_UPGRADE_STATUS_OK;
}

void ota_set_activity_callback(ota_activity_callback_t callback)
{
    s_activity_callback = callback;
}

bool ota_is_in_progress(void)
{
    return s_ota.in_progress;
}

esp_err_t ota_mark_running_app_valid(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state = ESP_OTA_IMG_UNDEFINED;
    esp_err_t err = esp_ota_get_state_partition(running, &ota_state);
    if (err == ESP_OK && ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "Marking OTA app valid");
        return esp_ota_mark_app_valid_cancel_rollback();
    }
    return ESP_OK;
}

esp_err_t ota_upgrade_handler(esp_zb_zcl_ota_upgrade_value_message_t *message)
{
    if (!message || message->info.status != ESP_ZB_ZCL_STATUS_SUCCESS) {
        return ESP_OK;
    }

    esp_err_t err = ESP_OK;

    switch (message->upgrade_status) {
    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_START:
        if (s_ota.in_progress) {
            message->upgrade_status = ESP_ZB_ZCL_OTA_UPGRADE_STATUS_BUSY;
            return ESP_OK;
        }
        message->upgrade_status = ota_accept_status(message);
        if (message->upgrade_status != ESP_ZB_ZCL_OTA_UPGRADE_STATUS_OK) {
            return ESP_OK;
        }

        s_ota.partition = esp_ota_get_next_update_partition(NULL);
        if (!s_ota.partition) {
            ESP_LOGE(TAG, "No OTA update partition found");
            message->upgrade_status = ESP_ZB_ZCL_OTA_UPGRADE_STATUS_ERROR;
            return ESP_OK;
        }

        s_ota.expected_size = message->ota_header.image_size;
        s_ota.file_version = message->ota_header.file_version;
        err = esp_ota_begin(s_ota.partition, OTA_WITH_SEQUENTIAL_WRITES, &s_ota.handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
            ota_reset_state();
            message->upgrade_status = ESP_ZB_ZCL_OTA_UPGRADE_STATUS_ERROR;
            return ESP_OK;
        }

        s_ota.in_progress = true;
        if (s_activity_callback) {
            s_activity_callback(true);
        }
        ESP_LOGI(TAG, "OTA started: slot=%s version=%" PRIu32 " size=%" PRIu32,
                 s_ota.partition->label, s_ota.file_version, s_ota.expected_size);
        message->upgrade_status = ESP_ZB_ZCL_OTA_UPGRADE_STATUS_OK;
        break;

    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_RECEIVE:
        if (!s_ota.in_progress || !s_ota.handle || !message->payload || message->payload_size == 0) {
            message->upgrade_status = ESP_ZB_ZCL_OTA_UPGRADE_STATUS_ERROR;
            return ESP_OK;
        }
        err = esp_ota_write(s_ota.handle, message->payload, message->payload_size);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed at %" PRIu32 ": %s", s_ota.received, esp_err_to_name(err));
            ota_reset_state();
            message->upgrade_status = ESP_ZB_ZCL_OTA_UPGRADE_STATUS_ERROR;
            return ESP_OK;
        }
        s_ota.received += message->payload_size;
        message->upgrade_status = ESP_ZB_ZCL_OTA_UPGRADE_STATUS_OK;
        break;

    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_CHECK: {
        uint32_t expected_payload_size = s_ota.expected_size;
        if (expected_payload_size >= OTA_HEADER_LENGTH) {
            expected_payload_size -= OTA_HEADER_LENGTH;
        }
        if (!s_ota.in_progress || (expected_payload_size && s_ota.received != expected_payload_size)) {
            ESP_LOGE(TAG, "OTA size mismatch: received=%" PRIu32 " expected=%" PRIu32,
                     s_ota.received, expected_payload_size);
            ota_reset_state();
            message->upgrade_status = ESP_ZB_ZCL_OTA_UPGRADE_STATUS_ERROR;
            return ESP_OK;
        }
        message->upgrade_status = ESP_ZB_ZCL_OTA_UPGRADE_STATUS_OK;
        break;
    }

    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_APPLY:
    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_FINISH:
        if (!s_ota.in_progress || !s_ota.handle || !s_ota.partition) {
            message->upgrade_status = ESP_ZB_ZCL_OTA_UPGRADE_STATUS_ERROR;
            return ESP_OK;
        }
        err = esp_ota_end(s_ota.handle);
        if (err == ESP_OK) {
            err = esp_ota_set_boot_partition(s_ota.partition);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA finalize failed: %s", esp_err_to_name(err));
            ota_reset_state();
            message->upgrade_status = ESP_ZB_ZCL_OTA_UPGRADE_STATUS_ERROR;
            return ESP_OK;
        }

        ESP_LOGI(TAG, "OTA complete: version=%" PRIu32 " bytes=%" PRIu32 ", rebooting",
                 s_ota.file_version, s_ota.received);
        s_ota.handle = 0;
        message->upgrade_status = ESP_ZB_ZCL_OTA_UPGRADE_STATUS_OK;
        esp_restart();
        break;

    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_ABORT:
        ESP_LOGW(TAG, "OTA aborted");
        ota_reset_state();
        message->upgrade_status = ESP_ZB_ZCL_OTA_UPGRADE_STATUS_OK;
        break;

    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_SERVER_NOT_FOUND:
        ESP_LOGW(TAG, "OTA server not found");
        break;

    default:
        ESP_LOGD(TAG, "OTA callback status=%u", message->upgrade_status);
        break;
    }

    return ESP_OK;
}
