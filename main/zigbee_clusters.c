#include "zigbee_clusters.h"

#include <inttypes.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_zigbee_cluster.h"
#include "zcl/esp_zigbee_zcl_basic.h"
#include "zcl/esp_zigbee_zcl_command.h"
#include "zcl/esp_zigbee_zcl_metering.h"
#include "zcl/esp_zigbee_zcl_ota.h"
#include "zcl/esp_zigbee_zcl_poll_control.h"
#include "zcl/esp_zigbee_zcl_power_config.h"

#include "battery.h"
#include "meter_math.h"
#include "meter_state.h"
#include "ota.h"

#define ESP_MANUFACTURER_NAME "ZigbeeHive"
#define ESP_MODEL_IDENTIFIER "WaterMeter"
#define MS_TO_QUARTER_SECONDS(ms) (((ms) + 249U) / 250U)

static const char *TAG = "ZIGBEE_CLUSTERS";

typedef struct {
    esp_zb_uint48_t current_summation;
    esp_zb_uint48_t scaled_summation;
    esp_zb_uint24_t multiplier;
    esp_zb_uint24_t divisor;
    uint32_t scale_multiplier;
    uint32_t scale_divisor;
} meter_zcl_attrs_t;

static meter_zcl_attrs_t s_meter_attrs;

static uint64_t refresh_meter_attr_mirrors(const meter_state_t *state)
{
    uint64_t scaled = meter_scaled_summation(state->pulse_count, state->multiplier, state->divisor);

    s_meter_attrs.current_summation = uint64_to_zb_u48(state->pulse_count);
    s_meter_attrs.scaled_summation = uint64_to_zb_u48(scaled);
    s_meter_attrs.multiplier = uint32_to_zb_u24(state->multiplier);
    s_meter_attrs.divisor = uint32_to_zb_u24(state->divisor);
    s_meter_attrs.scale_multiplier = state->multiplier;
    s_meter_attrs.scale_divisor = state->divisor;

    return scaled;
}

static esp_err_t report_attr(uint16_t cluster_id, uint16_t attr_id, bool manufacturer_specific)
{
    esp_zb_zcl_report_attr_cmd_t cmd = {
        .zcl_basic_cmd = {
            .src_endpoint = WATERMETER_ENDPOINT,
            .dst_endpoint = WATERMETER_ENDPOINT,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_DST_ADDR_ENDP_NOT_PRESENT,
        .clusterID = cluster_id,
        .manuf_specific = manufacturer_specific ? 1 : 0,
        .direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI,
        .dis_default_resp = 1,
        .manuf_code = manufacturer_specific ? OTA_MANUFACTURER_CODE : 0,
        .attributeID = attr_id,
    };
    esp_err_t err = esp_zb_zcl_report_attr_cmd_req(&cmd);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Report attr 0x%04x/0x%04x failed: %s", cluster_id, attr_id, esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Report attr 0x%04x/0x%04x queued", cluster_id, attr_id);
    }
    return err;
}

esp_err_t zigbee_clusters_report_meter_attribute(uint16_t attr_id)
{
    return report_attr(ESP_ZB_ZCL_CLUSTER_ID_METERING, attr_id, false);
}

void zigbee_clusters_report_battery(void)
{
    report_attr(ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
                ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_ID,
                false);
    report_attr(ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
                ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID,
                false);
}

void zigbee_clusters_refresh_meter(bool include_battery)
{
    meter_state_t state;
    meter_state_snapshot(&state);

    uint64_t scaled = refresh_meter_attr_mirrors(&state);

    esp_zb_zcl_set_attribute_val(WATERMETER_ENDPOINT,
                                 ESP_ZB_ZCL_CLUSTER_ID_METERING,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 ESP_ZB_ZCL_ATTR_METERING_CURRENT_SUMMATION_DELIVERED_ID,
                                 &s_meter_attrs.current_summation,
                                 false);
    esp_zb_zcl_set_attribute_val(WATERMETER_ENDPOINT,
                                 ESP_ZB_ZCL_CLUSTER_ID_METERING,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 ESP_ZB_ZCL_ATTR_METERING_MULTIPLIER_ID,
                                 &s_meter_attrs.multiplier,
                                 false);
    esp_zb_zcl_set_attribute_val(WATERMETER_ENDPOINT,
                                 ESP_ZB_ZCL_CLUSTER_ID_METERING,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 ESP_ZB_ZCL_ATTR_METERING_DIVISOR_ID,
                                 &s_meter_attrs.divisor,
                                 false);
    esp_zb_zcl_set_attribute_val(WATERMETER_ENDPOINT,
                                 ESP_ZB_ZCL_CLUSTER_ID_METERING,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 ATTR_SCALED_SUMMATION_ID,
                                 &s_meter_attrs.scaled_summation,
                                 false);
    esp_zb_zcl_set_attribute_val(WATERMETER_ENDPOINT,
                                 ESP_ZB_ZCL_CLUSTER_ID_METERING,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 ATTR_SCALE_MULTIPLIER_ID,
                                 &s_meter_attrs.scale_multiplier,
                                 false);
    esp_zb_zcl_set_attribute_val(WATERMETER_ENDPOINT,
                                 ESP_ZB_ZCL_CLUSTER_ID_METERING,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 ATTR_SCALE_DIVISOR_ID,
                                 &s_meter_attrs.scale_divisor,
                                 false);

    if (include_battery) {
        battery_update_zigbee_attrs(WATERMETER_ENDPOINT);
    }

    ESP_LOGI(TAG, "Meter update: pulses=%" PRIu64 " multiplier=%" PRIu32 " divisor=%" PRIu32 " scaled=%" PRIu64,
             state.pulse_count, state.multiplier, state.divisor, scaled);
}

static esp_zb_attribute_list_t *create_metering_cluster(void)
{
    esp_zb_attribute_list_t *attr_list = esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_METERING);
    static uint8_t metering_device_type = ESP_ZB_ZCL_METERING_WATER_METERING;
    static uint8_t unit_of_measure = ESP_ZB_ZCL_METERING_UNIT_L_LH_BINARY;
    static esp_zb_int24_t instantaneous_demand = { .low = 0, .high = 0 };

    meter_state_t state;
    meter_state_snapshot(&state);
    refresh_meter_attr_mirrors(&state);

    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(attr_list,
                                            ESP_ZB_ZCL_CLUSTER_ID_METERING,
                                            ESP_ZB_ZCL_ATTR_METERING_METERING_DEVICE_TYPE_ID,
                                            ESP_ZB_ZCL_ATTR_TYPE_8BIT_ENUM,
                                            ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                            &metering_device_type));
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(attr_list,
                                            ESP_ZB_ZCL_CLUSTER_ID_METERING,
                                            ESP_ZB_ZCL_ATTR_METERING_UNIT_OF_MEASURE_ID,
                                            ESP_ZB_ZCL_ATTR_TYPE_8BIT_ENUM,
                                            ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                            &unit_of_measure));
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(attr_list,
                                            ESP_ZB_ZCL_CLUSTER_ID_METERING,
                                            ESP_ZB_ZCL_ATTR_METERING_MULTIPLIER_ID,
                                            ESP_ZB_ZCL_ATTR_TYPE_U24,
                                            ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                            &s_meter_attrs.multiplier));
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(attr_list,
                                            ESP_ZB_ZCL_CLUSTER_ID_METERING,
                                            ESP_ZB_ZCL_ATTR_METERING_DIVISOR_ID,
                                            ESP_ZB_ZCL_ATTR_TYPE_U24,
                                            ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                            &s_meter_attrs.divisor));
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(attr_list,
                                            ESP_ZB_ZCL_CLUSTER_ID_METERING,
                                            ESP_ZB_ZCL_ATTR_METERING_INSTANTANEOUS_DEMAND_ID,
                                            ESP_ZB_ZCL_ATTR_TYPE_S24,
                                            ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
                                            &instantaneous_demand));
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(attr_list,
                                            ESP_ZB_ZCL_CLUSTER_ID_METERING,
                                            ESP_ZB_ZCL_ATTR_METERING_CURRENT_SUMMATION_DELIVERED_ID,
                                            ESP_ZB_ZCL_ATTR_TYPE_U48,
                                            ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
                                            &s_meter_attrs.current_summation));
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(attr_list,
                                            ESP_ZB_ZCL_CLUSTER_ID_METERING,
                                            ATTR_SCALED_SUMMATION_ID,
                                            ESP_ZB_ZCL_ATTR_TYPE_U48,
                                            ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
                                            &s_meter_attrs.scaled_summation));
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(attr_list,
                                            ESP_ZB_ZCL_CLUSTER_ID_METERING,
                                            ATTR_SCALE_MULTIPLIER_ID,
                                            ESP_ZB_ZCL_ATTR_TYPE_U32,
                                            ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE,
                                            &s_meter_attrs.scale_multiplier));
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(attr_list,
                                            ESP_ZB_ZCL_CLUSTER_ID_METERING,
                                            ATTR_SCALE_DIVISOR_ID,
                                            ESP_ZB_ZCL_ATTR_TYPE_U32,
                                            ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE,
                                            &s_meter_attrs.scale_divisor));

    return attr_list;
}

static void add_basic_cluster(esp_zb_cluster_list_t *cluster_list)
{
    esp_zb_basic_cluster_cfg_t basic_cfg = {
        .zcl_version = 0x02,
        .power_source = ESP_ZB_ZCL_BASIC_POWER_SOURCE_BATTERY,
    };
    esp_zb_attribute_list_t *attr_list = esp_zb_basic_cluster_create(&basic_cfg);
    esp_zb_basic_cluster_add_attr(attr_list,
                                  ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID,
                                  (void *)"\x0A" ESP_MANUFACTURER_NAME);
    esp_zb_basic_cluster_add_attr(attr_list,
                                  ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID,
                                  (void *)"\x0A" ESP_MODEL_IDENTIFIER);
    esp_zb_cluster_list_add_basic_cluster(cluster_list, attr_list, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
}

static void add_power_config_cluster(esp_zb_cluster_list_t *cluster_list)
{
    esp_zb_attribute_list_t *attr_list = esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG);
    static uint8_t battery_size = ESP_ZB_ZCL_POWER_CONFIG_BATTERY_SIZE_BUILT_IN;
    static uint8_t battery_quantity = 1;
    static uint8_t battery_rated_voltage = 37;

    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(attr_list,
                                            ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
                                            ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_ID,
                                            ESP_ZB_ZCL_ATTR_TYPE_U8,
                                            ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
                                            battery_voltage_zcl_attr()));
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(attr_list,
                                            ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
                                            ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID,
                                            ESP_ZB_ZCL_ATTR_TYPE_U8,
                                            ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
                                            battery_percent_zcl_attr()));
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(attr_list,
                                            ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
                                            ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_SIZE_ID,
                                            ESP_ZB_ZCL_ATTR_TYPE_8BIT_ENUM,
                                            ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                            &battery_size));
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(attr_list,
                                            ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
                                            ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_QUANTITY_ID,
                                            ESP_ZB_ZCL_ATTR_TYPE_U8,
                                            ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                            &battery_quantity));
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(attr_list,
                                            ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
                                            ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_RATED_VOLTAGE_ID,
                                            ESP_ZB_ZCL_ATTR_TYPE_U8,
                                            ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                            &battery_rated_voltage));
    esp_zb_cluster_list_add_power_config_cluster(cluster_list, attr_list, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
}

static void add_identify_cluster(esp_zb_cluster_list_t *cluster_list)
{
    esp_zb_identify_cluster_cfg_t identify_cfg = { .identify_time = 0 };
    esp_zb_attribute_list_t *attr_list = esp_zb_identify_cluster_create(&identify_cfg);
    esp_zb_cluster_list_add_identify_cluster(cluster_list, attr_list, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
}

static void add_poll_control_cluster(esp_zb_cluster_list_t *cluster_list)
{
    esp_zb_poll_control_cluster_cfg_t poll_control_cfg = {
        .check_in_interval = MS_TO_QUARTER_SECONDS(CONFIG_WATERMETER_ZIGBEE_KEEP_ALIVE_MS),
        .long_poll_interval = MS_TO_QUARTER_SECONDS(CONFIG_WATERMETER_ZIGBEE_KEEP_ALIVE_MS),
        .short_poll_interval = MS_TO_QUARTER_SECONDS(CONFIG_WATERMETER_OTA_POLL_INTERVAL_MS),
        .fast_poll_timeout = MS_TO_QUARTER_SECONDS(CONFIG_WATERMETER_DELIVERY_WINDOW_MS),
        .check_in_interval_min = 240,
        .long_poll_interval_min = 4,
        .fast_poll_timeout_max = 240,
    };
    esp_zb_attribute_list_t *attr_list = esp_zb_poll_control_cluster_create(&poll_control_cfg);
    esp_zb_cluster_list_add_poll_control_cluster(cluster_list, attr_list, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
}

static void add_ota_cluster(esp_zb_cluster_list_t *cluster_list)
{
    esp_zb_ota_cluster_cfg_t ota_cfg = {
        .ota_upgrade_file_version = WATERMETER_OTA_FILE_VERSION,
        .ota_upgrade_manufacturer = OTA_MANUFACTURER_CODE,
        .ota_upgrade_image_type = OTA_IMAGE_TYPE,
        .ota_min_block_reque = ESP_ZB_OTA_UPGRADE_MIN_BLOCK_PERIOD_DEF_VALUE,
        .ota_upgrade_file_offset = ESP_ZB_ZCL_OTA_UPGRADE_FILE_OFFSET_DEF_VALUE,
        .ota_upgrade_downloaded_file_ver = ESP_ZB_ZCL_OTA_UPGRADE_DOWNLOADED_FILE_VERSION_DEF_VALUE,
        .ota_upgrade_server_id = ESP_ZB_ZCL_OTA_UPGRADE_SERVER_DEF_VALUE,
        .ota_image_upgrade_status = ESP_ZB_ZCL_OTA_UPGRADE_IMAGE_STATUS_DEF_VALUE,
    };
    esp_zb_attribute_list_t *attr_list = esp_zb_ota_cluster_create(&ota_cfg);
    static esp_zb_zcl_ota_upgrade_client_variable_t ota_client_variable = {
        .timer_query = OTA_QUERY_INTERVAL_MINUTES,
        .hw_version = 1,
        .max_data_size = OTA_MAX_DATA_SIZE,
    };
    ESP_ERROR_CHECK(esp_zb_ota_cluster_add_attr(attr_list,
                                                ESP_ZB_ZCL_ATTR_OTA_UPGRADE_CLIENT_DATA_ID,
                                                &ota_client_variable));
    esp_zb_cluster_list_add_ota_cluster(cluster_list, attr_list, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
}

esp_zb_ep_list_t *zigbee_clusters_create_endpoint(void)
{
    esp_zb_cluster_list_t *cluster_list = esp_zb_zcl_cluster_list_create();
    esp_zb_attribute_list_t *metering_attr_list = create_metering_cluster();
    esp_zb_cluster_list_add_metering_cluster(cluster_list, metering_attr_list, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    add_basic_cluster(cluster_list);
    add_power_config_cluster(cluster_list);
    add_identify_cluster(cluster_list);
    add_poll_control_cluster(cluster_list);
    add_ota_cluster(cluster_list);

    esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();
    esp_zb_endpoint_config_t endpoint_config = {
        .endpoint = WATERMETER_ENDPOINT,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_METER_INTERFACE_DEVICE_ID,
        .app_device_version = 0,
    };
    esp_zb_ep_list_add_ep(ep_list, cluster_list, endpoint_config);
    return ep_list;
}
