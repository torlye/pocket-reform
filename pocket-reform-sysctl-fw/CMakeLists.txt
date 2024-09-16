cmake_minimum_required(VERSION 3.13)

include(lib/pico_sdk_import.cmake)
include(lib/pico_extras_import.cmake)

project(pocket_sysctl_fw)

# initialize the Raspberry Pi Pico SDK
pico_sdk_init()

add_executable(sysctl
	src/sysctl.c
	src/max17320.c
	src/mp2650.c
	src/fusb302b.c
	src/pd_com.c
	src/spi_com.c
	src/uart_com.c
	src/pd_com.c
	src/reset_interface.c
	src/reform_stdio_usb.c
	src/usb_descriptors.c
)

target_include_directories(sysctl PRIVATE
	inc
)

target_link_libraries(sysctl 
	pico_stdlib 
	hardware_i2c 
	hardware_spi 
	hardware_rtc 
	hardware_sleep 
	hardware_pwm
	pico_stdio
	pico_time
	pico_unique_id
	pico_usb_reset_interface
	tinyusb_device
)

pico_enable_stdio_uart(sysctl 0)
pico_add_extra_outputs(sysctl)

target_compile_definitions(sysctl PUBLIC PICO_RP2040_USB_DEVICE_ENUMERATION_FIX=1)
target_compile_options(sysctl PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(sysctl PUBLIC
	PICO_XOSC_STARTUP_DELAY_MULTIPLIER=64
)
