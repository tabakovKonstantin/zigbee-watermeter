idf_component_register(
    SRCS
        "battery.c"
        "battery_math.c"
        "meter_math.c"
        "meter_state.c"
        "ota.c"
        "watermeter.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES nvs_flash esp_driver_uart ieee802154 esp_timer esp_adc esp_pm app_update
)

if(DEFINED ENV{OTA_FILE_VERSION})
    set(WATERMETER_OTA_FILE_VERSION "$ENV{OTA_FILE_VERSION}")
else()
    set(WATERMETER_OTA_FILE_VERSION "1")
endif()

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    WATERMETER_OTA_FILE_VERSION=${WATERMETER_OTA_FILE_VERSION}
)
