#include "xiao_board.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

#define RF_SWITCH_POWER_GPIO GPIO_NUM_3
#define RF_SWITCH_SELECT_GPIO GPIO_NUM_14
#define RF_SWITCH_SETTLE_US 1000

static const char *TAG = "XIAO_BOARD";

esp_err_t xiao_board_enable_external_antenna(void)
{
    gpio_config_t select_config = {
        .pin_bit_mask = 1ULL << RF_SWITCH_SELECT_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&select_config);
    if (err != ESP_OK) {
        return err;
    }
    ESP_RETURN_ON_ERROR(gpio_set_level(RF_SWITCH_SELECT_GPIO, 1), TAG, "select external antenna");

    gpio_config_t power_config = {
        .pin_bit_mask = 1ULL << RF_SWITCH_POWER_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&power_config), TAG, "configure RF switch power");
    ESP_RETURN_ON_ERROR(gpio_set_level(RF_SWITCH_POWER_GPIO, 0), TAG, "enable RF switch");

    esp_rom_delay_us(RF_SWITCH_SETTLE_US);
    ESP_LOGI(TAG, "External U.FL antenna selected");
    return ESP_OK;
}

void xiao_board_prepare_deep_sleep(void)
{
    gpio_set_level(RF_SWITCH_POWER_GPIO, 1);
    gpio_set_direction(RF_SWITCH_POWER_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(RF_SWITCH_POWER_GPIO, GPIO_FLOATING);
    gpio_set_direction(RF_SWITCH_SELECT_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(RF_SWITCH_SELECT_GPIO, GPIO_FLOATING);
}
