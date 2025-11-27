#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "esp_zigbee_core.h"

#define SENSOR_PIN          GPIO_NUM_13
#define ED_AGING_TIMEOUT    ESP_ZB_ED_AGING_TIMEOUT_64MIN
#define ED_KEEP_ALIVE       3000 
#define HA_METERING_ENDPOINT 1
#define ESP_ZB_PRIMARY_CHANNEL_MASK ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK
#define ESP_ZB_HA_METER_INTERFACE_DEVICE_ID 0x0501
#define DEBOUNCE_MS         50
#define ESP_MANUFACTURER_NAME "ZigbeeHive"
#define ESP_MODEL_IDENTIFIER  "WaterMeter"

#ifndef ESP_ZB_HA_SIMPLE_SENSOR_DEVICE_ID
#define ESP_ZB_HA_SIMPLE_SENSOR_DEVICE_ID 0x000C
#endif

static const char *TAG = "ZIGBEE_METER";

// Global counter and spinlock
static volatile uint64_t total_liters_count = 0; 
static portMUX_TYPE counter_lock = portMUX_INITIALIZER_UNLOCKED;

static void save_counter_to_nvs(uint64_t count) {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &my_handle);

    if (err != ESP_OK) {
        return;
    }

    nvs_set_u64(my_handle, "water_count", count);
    nvs_commit(my_handle);
    nvs_close(my_handle);
}

static uint64_t load_counter_from_nvs() {
    nvs_handle_t my_handle;
    uint64_t count = 0;
    if (nvs_open("storage", NVS_READONLY, &my_handle) == ESP_OK) {
        nvs_get_u64(my_handle, "water_count", &count);
        nvs_close(my_handle);
    }
    return count;
}

static void IRAM_ATTR gpio_isr_handler(void* arg) {
    static uint32_t last_time = 0;
    uint32_t current_time = xTaskGetTickCountFromISR();
    
    if ((current_time - last_time) <= pdMS_TO_TICKS(DEBOUNCE_MS)) {
        return;
    }

    portENTER_CRITICAL_ISR(&counter_lock);
    total_liters_count += 1;
    portEXIT_CRITICAL_ISR(&counter_lock);
    last_time = current_time;
}

static void bdb_start_top_level_commissioning_cb(uint8_t mode_mask) {
    ESP_ERROR_CHECK(esp_zb_bdb_start_top_level_commissioning(mode_mask));
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

    esp_zb_attribute_list_t *metering_attr_list = esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_METERING);
    
    uint64_t current_summation = load_counter_from_nvs();
    portENTER_CRITICAL(&counter_lock);
    total_liters_count = current_summation;
    portEXIT_CRITICAL(&counter_lock);

    esp_zb_cluster_add_attr(metering_attr_list, ESP_ZB_ZCL_CLUSTER_ID_METERING, 
                            ESP_ZB_ZCL_ATTR_METERING_CURRENT_SUMMATION_DELIVERED_ID, 
                            ESP_ZB_ZCL_ATTR_TYPE_U48, ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING, 
                            &current_summation);

    // 2b. Unit of Measure (0x0300): 0x02 = Cubic Meters (m3)
    uint8_t unit_of_measure = 0x02; 
    esp_zb_cluster_add_attr(metering_attr_list, ESP_ZB_ZCL_CLUSTER_ID_METERING, 
                            ESP_ZB_ZCL_ATTR_METERING_UNIT_OF_MEASURE_ID, 
                            ESP_ZB_ZCL_ATTR_TYPE_8BIT_ENUM, ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY, 
                            &unit_of_measure);

    // 2c. Multiplier (0x0301) & Divisor (0x0302)
    // 1 m3 = 1000 Liters.
    uint32_t multiplier = 1;
    uint32_t divisor = 1000;
    esp_zb_cluster_add_attr(metering_attr_list, ESP_ZB_ZCL_CLUSTER_ID_METERING, 
                            ESP_ZB_ZCL_ATTR_METERING_MULTIPLIER_ID, 
                            ESP_ZB_ZCL_ATTR_TYPE_U24, ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY, 
                            &multiplier);
    esp_zb_cluster_add_attr(metering_attr_list, ESP_ZB_ZCL_CLUSTER_ID_METERING, 
                            ESP_ZB_ZCL_ATTR_METERING_DIVISOR_ID, 
                            ESP_ZB_ZCL_ATTR_TYPE_U24, ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY, 
                            &divisor);

    // 2d. Summation Formatting (0x0303) - Optional, but helps UI.
    uint8_t summation_formatting = 0x2A; 
    esp_zb_cluster_add_attr(metering_attr_list, ESP_ZB_ZCL_CLUSTER_ID_METERING, 
                            ESP_ZB_ZCL_ATTR_METERING_SUMMATION_FORMATTING_ID, 
                            ESP_ZB_ZCL_ATTR_TYPE_8BITMAP, ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY, 
                            &summation_formatting);

    // 2e. Metering Device Type (0x0306): 0x02 = Water Meter
    uint8_t device_type = 0x02;
    esp_zb_cluster_add_attr(metering_attr_list, ESP_ZB_ZCL_CLUSTER_ID_METERING, 
                            ESP_ZB_ZCL_ATTR_METERING_METERING_DEVICE_TYPE_ID, 
                            ESP_ZB_ZCL_ATTR_TYPE_8BITMAP, ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY, 
                            &device_type);

    // 2f. Status (0x0200) - Mandatory for Metering Cluster
    uint8_t status = 0x00;
    esp_zb_cluster_add_attr(metering_attr_list, ESP_ZB_ZCL_CLUSTER_ID_METERING, 
                            ESP_ZB_ZCL_ATTR_METERING_STATUS_ID, 
                            ESP_ZB_ZCL_ATTR_TYPE_8BITMAP, ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY, 
                            &status);

    // 3. Create Endpoint with Metering Cluster
    esp_zb_cluster_list_t *cluster_list = esp_zb_zcl_cluster_list_create();
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
        .endpoint = HA_METERING_ENDPOINT,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_METER_INTERFACE_DEVICE_ID,
        .app_device_version = 0
    };
    esp_zb_ep_list_add_ep(ep_list, cluster_list, endpoint_config);

    // 4. Register Device
    esp_zb_device_register(ep_list);

    // 5. Configure Reporting
    // Report CurrentSummationDelivered (0x0000)
    // Min Interval: 1s, Max Interval: 60s, Min Change: 1
    esp_zb_zcl_reporting_info_t reporting_info = {
        .direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV,
        .ep = HA_METERING_ENDPOINT,
        .cluster_id = ESP_ZB_ZCL_CLUSTER_ID_METERING,
        .cluster_role = ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        .dst.profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .attr_id = ESP_ZB_ZCL_ATTR_METERING_CURRENT_SUMMATION_DELIVERED_ID,
        .u.send_info.min_interval = 1,
        .u.send_info.max_interval = 60,
        .u.send_info.def_min_interval = 1,
        .u.send_info.def_max_interval = 60,
        .u.send_info.delta.u48.low = 1,
        .u.send_info.delta.u48.high = 0,
        .manuf_code = ESP_ZB_ZCL_ATTR_NON_MANUFACTURER_SPECIFIC,
    };
    esp_zb_zcl_update_reporting_info(&reporting_info);

    esp_zb_set_primary_network_channel_set(ESP_ZB_PRIMARY_CHANNEL_MASK);
    ESP_ERROR_CHECK(esp_zb_start(false));

    esp_zb_stack_main_loop();
}

static void update_task(void *pvParameters) {
    uint64_t last_saved_count = 0;
    uint64_t current_count = 0;
    
    portENTER_CRITICAL(&counter_lock);
    last_saved_count = total_liters_count;
    portEXIT_CRITICAL(&counter_lock);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        
        portENTER_CRITICAL(&counter_lock);
        current_count = total_liters_count;
        portEXIT_CRITICAL(&counter_lock);

        if (current_count == last_saved_count) {
            continue;
        }

        esp_zb_lock_acquire(portMAX_DELAY);
        esp_zb_zcl_set_attribute_val(HA_METERING_ENDPOINT, 
                                    ESP_ZB_ZCL_CLUSTER_ID_METERING, 
                                    ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, 
                                    ESP_ZB_ZCL_ATTR_METERING_CURRENT_SUMMATION_DELIVERED_ID, 
                                    &current_count, 
                                    false);
        esp_zb_lock_release();
        // TODO: maybe save every 10 liters or every minute if changed, to save flash wear.
        save_counter_to_nvs(current_count);
        last_saved_count = current_count;
        ESP_LOGI(TAG, "Updated Water Usage: %llu Liters", current_count);
    }
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
    
    // Start Update Task
    xTaskCreate(update_task, "Update_task", 4096, NULL, 5, NULL);
}
