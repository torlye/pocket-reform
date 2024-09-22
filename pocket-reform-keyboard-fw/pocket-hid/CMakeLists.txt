cmake_minimum_required(VERSION 3.17)

include(pico_sdk_import.cmake)

set(PROJECT pocket-hid)

project(${PROJECT} C CXX ASM)

pico_sdk_init()

add_executable(${PROJECT})

# Example source
target_sources(${PROJECT} PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src/main.c
        ${CMAKE_CURRENT_SOURCE_DIR}/src/usb_descriptors.c
        ${CMAKE_CURRENT_SOURCE_DIR}/src/oled.c
        ${CMAKE_CURRENT_SOURCE_DIR}/src/remote.c
        ${CMAKE_CURRENT_SOURCE_DIR}/src/menu.c
        )

# Example include
target_include_directories(${PROJECT} PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        )

target_link_libraries(${PROJECT} PRIVATE
        pico_stdlib pico_bootsel_via_double_reset tinyusb_device tinyusb_board hardware_pwm hardware_i2c hardware_pio hardware_watchdog)

pico_add_extra_outputs(${PROJECT})

target_compile_definitions(${PROJECT} PUBLIC PICO_RP2040_USB_DEVICE_ENUMERATION_FIX=1)

target_compile_definitions(${PROJECT} PUBLIC
	PICO_XOSC_STARTUP_DELAY_MULTIPLIER=64
  )
