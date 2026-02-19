cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(race_timer_firmware)

target_sources(app PRIVATE
    src/main.c
    src/ble_race_service.c
    src/buttons.c
    src/leds.c
    src/display.c
    src/race_timer.c
    src/race_manager.c
)

target_include_directories(app PRIVATE
    src
)
