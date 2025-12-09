#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_zigbee_core.h"
#include "esp_zigbee_cluster.h"
#include "zcl/esp_zigbee_zcl_basic.h"
#include "zcl/esp_zigbee_zcl_drlc.h"
#include "zcl/esp_zigbee_zcl_metering.h"
#include "zcl/esp_zigbee_zcl_common.h"
#include "nwk/esp_zigbee_nwk.h"

#define SENSOR_PIN          GPIO_NUM_13
#define HA_ENDPOINT         1
#define ESP_MANUFACTURER_NAME "ZigbeeHive"
#define ESP_MODEL_IDENTIFIER  "WaterMeter"
#define BOOT_BUTTON_PIN     GPIO_NUM_9
#define BUTTON_HOLD_TIME_MS 3000

// Update interval in milliseconds
#define UPDATE_INTERVAL_MS  10000 

static const char *TAG = "ZIGBEE_METER";

// Global atomic pulse counter
volatile uint64_t pulse_count = 0;
portMUX_TYPE pulse_mux = portMUX_INITIALIZER_UNLOCKED;

static void IRAM_ATTR gpio_isr_handler(void* arg) {
    portENTER_CRITICAL_ISR(&pulse_mux);
    pulse_count++;
    portEXIT_CRITICAL_ISR(&pulse_mux);
}

static void bdb_start_top_level_commissioning_cb(uint8_t mode_mask) {
    ESP_ERROR_CHECK(esp_zb_bdb_start_top_level_commissioning(mode_mask));
}

static void update_measurement(uint8_t arg) {
    static uint64_t last_count = 0;
    uint64_t current_count;
    portENTER_CRITICAL(&pulse_mux);
    current_count = pulse_count;
    portEXIT_CRITICAL(&pulse_mux);

    uint64_t diff = current_count - last_count;
    last_count = current_count;

    // Flow rate in L/h
    // 1 pulse = 1L. Interval is 10 seconds, so x360 = L/h
    int32_t flow_rate = (int32_t)(diff * 360);
    esp_zb_int24_t flow_rate_s24;
    flow_rate_s24.low = flow_rate & 0xFFFF;
    flow_rate_s24.high = (flow_rate >> 16) & 0xFF;

    esp_zb_lock_acquire(portMAX_DELAY);

    esp_zb_zcl_set_attribute_val(
        HA_ENDPOINT,
        ESP_ZB_ZCL_CLUSTER_ID_METERING,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        ESP_ZB_ZCL_ATTR_METERING_INSTANTANEOUS_DEMAND_ID,
        &flow_rate_s24,
        false
    );

    // Volume Delta (Liters)
    esp_zb_uint48_t volume_delta_u48;
    volume_delta_u48.low = (uint32_t)(diff & 0xFFFFFFFF);
    volume_delta_u48.high = (uint16_t)((diff >> 32) & 0xFFFF);

    esp_zb_zcl_set_attribute_val(
        HA_ENDPOINT,
        ESP_ZB_ZCL_CLUSTER_ID_METERING,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        ESP_ZB_ZCL_ATTR_METERING_CURRENT_SUMMATION_DELIVERED_ID,
        &volume_delta_u48,
        false
    );


    esp_zb_lock_release();
    esp_zb_scheduler_alarm((esp_zb_callback_t)update_measurement, 0, UPDATE_INTERVAL_MS);
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
        
        // Start measurement updates
        esp_zb_scheduler_alarm((esp_zb_callback_t)update_measurement, 0, UPDATE_INTERVAL_MS);
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
    // Initialize Zigbee stack configuration
    esp_zb_cfg_t zb_nwk_cfg = {
        .esp_zb_role = ESP_ZB_DEVICE_TYPE_ED,
        .install_code_policy = false,
        .nwk_cfg.zed_cfg = {
            .ed_timeout = ESP_ZB_ED_AGING_TIMEOUT_64MIN,
            .keep_alive = 3000,
        },
    };
    esp_zb_init(&zb_nwk_cfg);
    
    // Create Attribute List for Metering Cluster
    esp_zb_attribute_list_t *metering_attr_list = esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_METERING);

    // Attribute 0x0306: Metering Device Type = Water Metering (0x02)
    static uint8_t metering_device_type = ESP_ZB_ZCL_METERING_WATER_METERING; 

    // Attribute 0x0300: Unit of Measure = L/h (binary)
    static uint8_t unit_of_measure = ESP_ZB_ZCL_METERING_UNIT_L_LH_BINARY;

    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(
        metering_attr_list,
        ESP_ZB_ZCL_CLUSTER_ID_METERING,
        ESP_ZB_ZCL_ATTR_METERING_METERING_DEVICE_TYPE_ID,
        ESP_ZB_ZCL_ATTR_TYPE_8BIT_ENUM,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
        &metering_device_type
    ));
    
    // Set Unit of Measure (Liters) - CRITICAL for Home Assistant device_class
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(
        metering_attr_list,
        ESP_ZB_ZCL_CLUSTER_ID_METERING,
        ESP_ZB_ZCL_ATTR_METERING_UNIT_OF_MEASURE_ID,
        ESP_ZB_ZCL_ATTR_TYPE_8BIT_ENUM,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
        &unit_of_measure
    ));

    esp_zb_uint24_t multiplier = { .low = 1, .high = 0 };
    ESP_ERROR_CHECK(
        esp_zb_cluster_add_attr(
            metering_attr_list,
            ESP_ZB_ZCL_CLUSTER_ID_METERING,
            ESP_ZB_ZCL_ATTR_METERING_MULTIPLIER_ID,
            ESP_ZB_ZCL_ATTR_TYPE_U24,
            ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
            &multiplier
        )
    );

    esp_zb_uint24_t divisor = { .low = 1, .high = 0 };
    ESP_ERROR_CHECK(
        esp_zb_cluster_add_attr(
            metering_attr_list,
            ESP_ZB_ZCL_CLUSTER_ID_METERING,
            ESP_ZB_ZCL_ATTR_METERING_DIVISOR_ID,
            ESP_ZB_ZCL_ATTR_TYPE_U24,
            ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
            &divisor
        )
    );

    static esp_zb_int24_t instantaneous_demand = { .low = 0, .high = 0 };
    ESP_ERROR_CHECK(
        esp_zb_cluster_add_attr(
            metering_attr_list,
            ESP_ZB_ZCL_CLUSTER_ID_METERING,
            ESP_ZB_ZCL_ATTR_METERING_INSTANTANEOUS_DEMAND_ID,
            ESP_ZB_ZCL_ATTR_TYPE_S24,
            ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
            &instantaneous_demand
        )
    );

    static esp_zb_uint48_t current_summation = { .low = 0, .high = 0 };
    ESP_ERROR_CHECK(
        esp_zb_cluster_add_attr(
            metering_attr_list,
            ESP_ZB_ZCL_CLUSTER_ID_METERING,
            ESP_ZB_ZCL_ATTR_METERING_CURRENT_SUMMATION_DELIVERED_ID,
            ESP_ZB_ZCL_ATTR_TYPE_U48,
            ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
            &current_summation
        )
    );
    
    // Create Cluster List and Add Metering Cluster
    esp_zb_cluster_list_t *cluster_list = esp_zb_zcl_cluster_list_create();
    esp_zb_cluster_list_add_metering_cluster(cluster_list, metering_attr_list, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    
    // Add Basic and Identify Clusters (Mandatory)
    esp_zb_basic_cluster_cfg_t basic_cfg = { .zcl_version = 0x02, .power_source = ESP_ZB_ZCL_BASIC_POWER_SOURCE_MAINS_SINGLE_PHASE };
    esp_zb_attribute_list_t *basic_attr_list = esp_zb_basic_cluster_create(&basic_cfg);
    esp_zb_basic_cluster_add_attr(basic_attr_list, ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, (void *)"\x0A" ESP_MANUFACTURER_NAME);
    esp_zb_basic_cluster_add_attr(basic_attr_list, ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, (void *)"\x0A" ESP_MODEL_IDENTIFIER);
    
    esp_zb_cluster_list_add_basic_cluster(cluster_list, basic_attr_list, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_identify_cluster_cfg_t identify_cfg = { .identify_time = 0 };
    esp_zb_attribute_list_t *identify_attr_list = esp_zb_identify_cluster_create(&identify_cfg);
    esp_zb_cluster_list_add_identify_cluster(cluster_list, identify_attr_list, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    // Create Endpoint
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

    // Start Zigbee Stack
    esp_zb_start(false); 
    
    esp_zb_stack_main_loop();
}

static void reset_button_task(void *pvParameters) {
    // Configure the button
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);

    ESP_LOGI(TAG, "Button task started. Hold BOOT (GPIO9) for 3s to reset Zigbee.");

    while (1) {
        // Button is Active Low (0 when pressed)
        if (gpio_get_level(BOOT_BUTTON_PIN) == 0) {
            ESP_LOGW(TAG, "Button pressed, hold to reset...");
            
            int hold_count = 0;
            // Check every 100ms if button is still held
            while (gpio_get_level(BOOT_BUTTON_PIN) == 0) {
                vTaskDelay(pdMS_TO_TICKS(100));
                hold_count += 100;

                if (hold_count >= BUTTON_HOLD_TIME_MS) {
                    ESP_LOGW(TAG, "Factory Reset Triggered! Rebooting...");
                    
                    // Optional: Clear your own custom NVS data (pulse counts) if you want a TOTAL reset
                    // nvs_flash_erase(); 

                    // This clears Zigbee creds and reboots the device
                    esp_zb_factory_reset(); 
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100)); // Scan loop delay
    }
}

void app_main(void) {
    esp_zb_platform_config_t config = {
        .radio_config = { .radio_mode = ZB_RADIO_MODE_NATIVE },
        .host_config = { .host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE },
    };

    ESP_ERROR_CHECK(esp_zb_platform_config(&config));

    // Configure Sensor GPIO
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_NEGEDGE;
    io_conf.pin_bit_mask = (1ULL << SENSOR_PIN);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = 1; 
    gpio_config(&io_conf);
    
    gpio_install_isr_service(0);
    gpio_isr_handler_add(SENSOR_PIN, gpio_isr_handler, (void*) SENSOR_PIN);

    // Start Tasks
    xTaskCreate(reset_button_task, "reset_button", 2048, NULL, 10, NULL);
    xTaskCreate(esp_zb_task, "Zigbee_main", 4096, NULL, 5, NULL);
}
