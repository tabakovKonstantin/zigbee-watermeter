#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_zigbee_core.h"

#define SENSOR_PIN          GPIO_NUM_13
#define ED_AGING_TIMEOUT    ESP_ZB_ED_AGING_TIMEOUT_64MIN
#define ED_KEEP_ALIVE       3000 
#define HA_ENDPOINT         1
#define ESP_ZB_PRIMARY_CHANNEL_MASK ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK
#define ESP_ZB_HA_METER_INTERFACE_DEVICE_ID 0x0501
#define DEBOUNCE_US         50000 // 50ms
#define ESP_MANUFACTURER_NAME "ZigbeeHive"
#define ESP_MODEL_IDENTIFIER  "WaterMeter"

#ifndef ESP_ZB_HA_SIMPLE_SENSOR_DEVICE_ID
#define ESP_ZB_HA_SIMPLE_SENSOR_DEVICE_ID 0x000C
#endif

static const char *TAG = "ZIGBEE_METER";

// Flow measurement variables
#define CIRCULAR_BUFFER_SIZE 5
static volatile uint64_t total_pulses = 0;
static uint64_t last_saved_pulses = 0;
static volatile uint64_t last_pulse_time_us = 0;
static volatile uint32_t pulse_intervals[CIRCULAR_BUFFER_SIZE] = {0};
static volatile uint8_t interval_idx = 0;
static volatile uint8_t interval_count = 0;
static portMUX_TYPE flow_lock = portMUX_INITIALIZER_UNLOCKED;

static void IRAM_ATTR gpio_isr_handler(void* arg) {
    uint64_t current_time = esp_timer_get_time();

    // Initialize last_time on first run
    if (last_pulse_time_us == 0) {
        last_pulse_time_us = current_time;
        return;
    }
    
    if ((current_time - last_pulse_time_us) <= DEBOUNCE_US) {
        return;
    }

    portENTER_CRITICAL_ISR(&flow_lock);
    total_pulses++;

    // Calculate delta
    uint32_t dt = (uint32_t)(current_time - last_pulse_time_us);

    // If gap is too long (e.g. > 30s), reset buffer to avoid "drag" from idle time
    if (dt > 30000000) {
        interval_count = 0;
        interval_idx = 0;
    } else {
        // Store interval
        pulse_intervals[interval_idx] = dt;
        interval_idx = (interval_idx + 1) % CIRCULAR_BUFFER_SIZE;
        if (interval_count < CIRCULAR_BUFFER_SIZE) {
            interval_count++;
        }
    }
    
    last_pulse_time_us = current_time;
    portEXIT_CRITICAL_ISR(&flow_lock);
}

static void bdb_start_top_level_commissioning_cb(uint8_t mode_mask) {
    ESP_ERROR_CHECK(esp_zb_bdb_start_top_level_commissioning(mode_mask));
}
static void update_flow_attribute(uint8_t arg) {
    uint32_t intervals_copy[CIRCULAR_BUFFER_SIZE];
    uint8_t count_copy = 0;
    uint64_t current_total_pulses = 0;
    uint64_t last_pulse_time_copy = 0;

    portENTER_CRITICAL(&flow_lock);
    memcpy(intervals_copy, (void*)pulse_intervals, sizeof(pulse_intervals));
    count_copy = interval_count;
    current_total_pulses = total_pulses;
    last_pulse_time_copy = last_pulse_time_us;
    portEXIT_CRITICAL(&flow_lock);

    float flow_rate_m3h = 0.0f;
    uint64_t now = esp_timer_get_time();
    uint64_t time_since_last_pulse = now - last_pulse_time_copy;

    // 1. Calculate Average form Buffer
    float avg_interval_us = 0;
    if (count_copy > 0) {
        uint64_t sum_intervals = 0;
        for (int i = 0; i < count_copy; i++) {
            sum_intervals += intervals_copy[i];
        }
        avg_interval_us = (float)sum_intervals / count_copy;
    }

    // 2. Logic Correction: Handle "Stopping" flow
    // If the time since the last pulse is LONGER than the average,
    // we must use the current elapsed time as the interval, 
    // effectively "slowing down" the calculated flow immediately.
    if (count_copy > 0) {
        float effective_interval = avg_interval_us;
        
        if (time_since_last_pulse > avg_interval_us) {
            effective_interval = (float)time_since_last_pulse;
        }

        // Apply a realistic timeout (e.g. 30s) to force 0
        if (time_since_last_pulse < 30000000) {
             // 1 Pulse = 1 Liter assumption preserved here
             flow_rate_m3h = (3600.0f * 1000.0f) / effective_interval;
        } else {
             flow_rate_m3h = 0;
        }
    }

    int16_t flow_scaled = (int16_t)(flow_rate_m3h * 10.0f);

    esp_zb_zcl_set_attribute_val(HA_ENDPOINT, 
                                ESP_ZB_ZCL_CLUSTER_ID_FLOW_MEASUREMENT, 
                                ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, 
                                ESP_ZB_ZCL_ATTR_FLOW_MEASUREMENT_VALUE_ID, 
                                &flow_scaled, 
                                false);

    // Update Metering Cluster: CurrentSummationDelivered (0x0000)
    // Uint48 type (0x25).
    esp_zb_uint48_t current_summation;
    current_summation.low = (uint32_t)(current_total_pulses & 0xFFFFFFFF);
    current_summation.high = (uint16_t)((current_total_pulses >> 32) & 0xFFFF);

    esp_zb_zcl_status_t status = esp_zb_zcl_set_attribute_val(HA_ENDPOINT, 
                                ESP_ZB_ZCL_CLUSTER_ID_METERING, 
                                ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, 
                                ESP_ZB_ZCL_ATTR_METERING_CURRENT_SUMMATION_DELIVERED_ID, 
                                &current_summation, 
                                false);
                                
    if (status != ESP_ZB_ZCL_STATUS_SUCCESS) {
         ESP_LOGW(TAG, "Failed to update metering attribute: 0x%x", status);
    }

    // Save to NVS if needed
    if ((current_total_pulses - last_saved_pulses) > 100) { // Save every 100 pulses/Liters
        nvs_handle_t my_handle;
        esp_err_t err = nvs_open("storage", NVS_READWRITE, &my_handle);
        if (err == ESP_OK) {
            err = nvs_set_u64(my_handle, "pulses", current_total_pulses);
            if (err == ESP_OK) {
                err = nvs_commit(my_handle);
                if (err == ESP_OK) {
                     last_saved_pulses = current_total_pulses;
                     ESP_LOGI(TAG, "Saved pulses to NVS: %llu", current_total_pulses);
                }
            }
            nvs_close(my_handle);
        }
        
        if (err != ESP_OK) {
             ESP_LOGE(TAG, "Error saving to NVS: %s", esp_err_to_name(err));
        }
    }

    ESP_LOGI(TAG, "Total: %llu, Flow: %.3f m³/h (scaled: %d)", current_total_pulses, flow_rate_m3h, flow_scaled);

    esp_zb_scheduler_alarm((esp_zb_callback_t)update_flow_attribute, 0, 5000); // Update every 5s
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct) {
    uint32_t *p_sg_p = signal_struct->p_app_signal;
    esp_err_t err_status = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t sig_type = *p_sg_p;

    switch (sig_type) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Zigbee Stack Initialized. Starting Commissioning...");
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
        break;
    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (err_status != ESP_OK) {
            ESP_LOGW(TAG, "Failed to initialize Zigbee stack (status: %s)", esp_err_to_name(err_status));
            return;
        }

        ESP_LOGI(TAG, "Device started up in %s factory-reset mode", esp_zb_bdb_is_factory_new() ? "" : "non");
        if (esp_zb_bdb_is_factory_new()) {
            ESP_LOGI(TAG, "Start network steering");
            esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
        } else {
            ESP_LOGI(TAG, "Device rebooted");
        }
        break;
    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (err_status == ESP_OK) {
            ESP_LOGI(TAG, "Joined network successfully!");
        } else {
            ESP_LOGW(TAG, "Network steering failed, retrying in 1s...");
            esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_top_level_commissioning_cb, ESP_ZB_BDB_MODE_NETWORK_STEERING, 1000);
        }
        break;
    default:
        ESP_LOGI(TAG, "ZDO signal: %s (0x%x)", esp_zb_zdo_signal_to_string(sig_type), sig_type);
        break;
    }
}

static void esp_zb_task(void *pvParameters) {
    esp_zb_cfg_t zb_nwk_cfg = {
        .esp_zb_role = ESP_ZB_DEVICE_TYPE_ED,
        .install_code_policy = false,
        .nwk_cfg.zed_cfg = {
            .ed_timeout = ED_AGING_TIMEOUT,
            .keep_alive = ED_KEEP_ALIVE,
        },
    };
    esp_zb_init(&zb_nwk_cfg);

    // Create Flow Measurement Cluster (0x0404)
    esp_zb_attribute_list_t *flow_attr_list = esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_FLOW_MEASUREMENT);
    
    // MeasuredValue attribute (0x0000) - int16, scaled by 10
    // Unit: m³/h, so value 100 = 10.0 m³/h
    int16_t measured_flow = 0;
    esp_zb_cluster_add_attr(flow_attr_list, ESP_ZB_ZCL_CLUSTER_ID_FLOW_MEASUREMENT, 
                            ESP_ZB_ZCL_ATTR_FLOW_MEASUREMENT_VALUE_ID, 
                            ESP_ZB_ZCL_ATTR_TYPE_S16, ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING, 
                            &measured_flow);
    
    // MinMeasuredValue attribute (0x0001) - minimum value = 0 m³/h
    uint16_t min_measured_value = 0;
    esp_zb_cluster_add_attr(flow_attr_list, ESP_ZB_ZCL_CLUSTER_ID_FLOW_MEASUREMENT, 
                            ESP_ZB_ZCL_ATTR_FLOW_MEASUREMENT_MIN_VALUE_ID, 
                            ESP_ZB_ZCL_ATTR_TYPE_U16, ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY, 
                            &min_measured_value);
    
    // MaxMeasuredValue attribute (0x0002) - maximum value = 100 m³/h (10000 scaled)
    uint16_t max_measured_value = 10000;
    esp_zb_cluster_add_attr(flow_attr_list, ESP_ZB_ZCL_CLUSTER_ID_FLOW_MEASUREMENT, 
                            ESP_ZB_ZCL_ATTR_FLOW_MEASUREMENT_MAX_VALUE_ID, 
                            ESP_ZB_ZCL_ATTR_TYPE_U16, ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY, 
                            &max_measured_value);

    // Create Endpoint with Flow Measurement Cluster
    esp_zb_cluster_list_t *cluster_list = esp_zb_zcl_cluster_list_create();
    esp_zb_cluster_list_add_flow_meas_cluster(cluster_list, flow_attr_list, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    
    // Create Metering Cluster (0x0702)
    esp_zb_attribute_list_t *metering_attr_list = esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_METERING);

    // CurrentSummationDelivered (0x0000) - Uint48
    esp_zb_uint48_t current_summation = { .low = 0, .high = 0 };
    esp_zb_cluster_add_attr(metering_attr_list, ESP_ZB_ZCL_CLUSTER_ID_METERING, 
                            ESP_ZB_ZCL_ATTR_METERING_CURRENT_SUMMATION_DELIVERED_ID, 
                            ESP_ZB_ZCL_ATTR_TYPE_U48, ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING, 
                            &current_summation);

    // Unit of Measure (0x0300) - 8-bit enum - Liters (0x07)
    uint8_t unit_of_measure = 0x07; 
    esp_zb_cluster_add_attr(metering_attr_list, ESP_ZB_ZCL_CLUSTER_ID_METERING, 
                            ESP_ZB_ZCL_ATTR_METERING_UNIT_OF_MEASURE_ID, 
                            ESP_ZB_ZCL_ATTR_TYPE_8BIT_ENUM, ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY, 
                            &unit_of_measure);

    esp_zb_cluster_list_add_metering_cluster(cluster_list, metering_attr_list, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    // Also add Basic Cluster for Device Info
    esp_zb_attribute_list_t *basic_attr_list = esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_BASIC);
    
    // Add Manufacturer Name
    char manuf_name_zcl[32];
    manuf_name_zcl[0] = (char) strlen(ESP_MANUFACTURER_NAME);
    strcpy(manuf_name_zcl + 1, ESP_MANUFACTURER_NAME);
    esp_zb_basic_cluster_add_attr(basic_attr_list, ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, manuf_name_zcl);

    // Add Model Identifier
    char model_id_zcl[32];
    model_id_zcl[0] = (char) strlen(ESP_MODEL_IDENTIFIER);
    strcpy(model_id_zcl + 1, ESP_MODEL_IDENTIFIER);
    esp_zb_basic_cluster_add_attr(basic_attr_list, ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, model_id_zcl);

    // Add Power Source (0x01 = Mains Single Phase)
    uint8_t power_source = 0x01;
    esp_zb_basic_cluster_add_attr(basic_attr_list, ESP_ZB_ZCL_ATTR_BASIC_POWER_SOURCE_ID, &power_source);

    esp_zb_cluster_list_add_basic_cluster(cluster_list, basic_attr_list, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();

    esp_zb_endpoint_config_t endpoint_config = {
        .endpoint = HA_ENDPOINT,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_METER_INTERFACE_DEVICE_ID,
        .app_device_version = 0
    };
    esp_zb_ep_list_add_ep(ep_list, cluster_list, endpoint_config);

    // Register Device
    esp_zb_device_register(ep_list);

    // Configure Reporting
    // Report Flow MeasuredValue (0x0000)
    // Min Interval: 10s, Max Interval: 1 hour (3600s), Min Change: 10
    esp_zb_zcl_reporting_info_t flow_reporting_info = {
        .direction = ESP_ZB_ZCL_REPORT_DIRECTION_SEND,
        .ep = HA_ENDPOINT,
        .cluster_id = ESP_ZB_ZCL_CLUSTER_ID_FLOW_MEASUREMENT,
        .cluster_role = ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        .dst.profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .dst.endpoint = 1,
        .attr_id = ESP_ZB_ZCL_ATTR_FLOW_MEASUREMENT_VALUE_ID,
        .u.send_info.min_interval = 10,
        .u.send_info.max_interval = 3600,
        .u.send_info.def_min_interval = 10,
        .u.send_info.def_max_interval = 3600,
        .u.send_info.delta.s16 = 1,
        .manuf_code = ESP_ZB_ZCL_ATTR_NON_MANUFACTURER_SPECIFIC,
    };
    esp_zb_zcl_update_reporting_info(&flow_reporting_info);

    // Report Metering CurrentSummationDelivered
    esp_zb_zcl_reporting_info_t metering_reporting_info = {
        .direction = ESP_ZB_ZCL_REPORT_DIRECTION_SEND,
        .ep = HA_ENDPOINT,
        .cluster_id = ESP_ZB_ZCL_CLUSTER_ID_METERING,
        .cluster_role = ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        .dst.profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .attr_id = ESP_ZB_ZCL_ATTR_METERING_CURRENT_SUMMATION_DELIVERED_ID,
        .u.send_info.min_interval = 60,
        .u.send_info.max_interval = 3600,
        .u.send_info.def_min_interval = 60,
        .u.send_info.def_max_interval = 3600,
        .u.send_info.delta.u48 = { .low = 1, .high = 0 },
        .manuf_code = ESP_ZB_ZCL_ATTR_NON_MANUFACTURER_SPECIFIC,
    };
    esp_zb_zcl_update_reporting_info(&metering_reporting_info);

    esp_zb_set_primary_network_channel_set(ESP_ZB_PRIMARY_CHANNEL_MASK);
    ESP_ERROR_CHECK(esp_zb_start(false));

    // Schedule the flow update task
    esp_zb_scheduler_alarm((esp_zb_callback_t)update_flow_attribute, 0, 5000);

    esp_zb_stack_main_loop();
}

void app_main(void) {
    esp_zb_platform_config_t config = {
        .radio_config = { .radio_mode = ZB_RADIO_MODE_NATIVE },
        .host_config = { .host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE },
    };

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // Load saved pulses
    nvs_handle_t my_handle;
    if (nvs_open("storage", NVS_READONLY, &my_handle) == ESP_OK) {
        uint64_t saved_pulses = 0;
        if (nvs_get_u64(my_handle, "pulses", &saved_pulses) == ESP_OK) {
            total_pulses = saved_pulses;
            last_saved_pulses = saved_pulses;
            ESP_LOGI(TAG, "Restored total pulses: %llu", total_pulses);
        }
        nvs_close(my_handle);
    }

    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_NEGEDGE;
    io_conf.pin_bit_mask = (1ULL << SENSOR_PIN);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = 1; 
    gpio_config(&io_conf);
    
    gpio_install_isr_service(0);
    gpio_isr_handler_add(SENSOR_PIN, gpio_isr_handler, (void*) SENSOR_PIN);

    // Start Zigbee Task
    ESP_ERROR_CHECK(esp_zb_platform_config(&config));
    xTaskCreate(esp_zb_task, "Zigbee_main", 4096, NULL, 5, NULL);
}
