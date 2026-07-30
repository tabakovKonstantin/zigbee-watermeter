cmake_minimum_required(VERSION 3.16)

project(watermeter_host_tests C)

enable_testing()

add_executable(watermeter_math_test
    host/test_watermeter_math.c
    ../main/battery_math.c
    ../main/meter_math.c
    ../main/power_schedule.c
)

target_include_directories(watermeter_math_test PRIVATE
    ../main
    ../managed_components/espressif__esp-zigbee-lib/include
)

target_compile_options(watermeter_math_test PRIVATE
    -Wall
    -Wextra
    -Werror
)

add_test(NAME watermeter_math COMMAND watermeter_math_test)
