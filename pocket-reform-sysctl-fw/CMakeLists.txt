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
	"${CMAKE_CURRENT_BINARY_DIR}/inc-generated/ds20-descriptor.h"
)

target_include_directories(sysctl PRIVATE
	inc
	"${CMAKE_CURRENT_BINARY_DIR}/inc-generated"
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
	tinyusb_device
)

pico_enable_stdio_uart(sysctl 0)
pico_add_extra_outputs(sysctl)

target_compile_definitions(sysctl PUBLIC PICO_RP2040_USB_DEVICE_ENUMERATION_FIX=1)
target_compile_options(sysctl PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(sysctl PUBLIC
	PICO_XOSC_STARTUP_DELAY_MULTIPLIER=64
)

if (DEFINED ENV{MNTRE_FIRMWARE_VERSION} AND (NOT MNTRE_FIRMWARE_VERSION))
	set(MNTRE_FIRMWARE_VERSION $ENV{MNTRE_FIRMWARE_VERSION})
    set(FWCAB_DEVELOPER_NAME "MNT Research GmbH")
    set(FWCAB_RELEASE_SOURCE_URL "https://source.mnt.re/reform/pocket-reform/-/tags/${MNTRE_FIRMWARE_VERSION}")
    set(FWCAB_FIRMWARE_DESCRIPTION "<p>MNT Research Pocket Reform System Controller Firmware Update (Release Build)</p>")
    set(FWCAB_RELEASE_DESCRIPTION_FILE "${CMAKE_CURRENT_LIST_DIR}/description-release.html")
    message("Using MNTRE_FIRMWARE_VERSION from environment ('${MNTRE_FIRMWARE_VERSION}')")
endif ()
if (NOT MNTRE_FIRMWARE_VERSION)
	string(TIMESTAMP TODAY "%m%d")
	set(MNTRE_FIRMWARE_VERSION "${TODAY}")
    set(FWCAB_DEVELOPER_NAME "J. Random Hacker")
    if (DEFINED ENV{CI_COMMIT_BRANCH} AND (NOT MNTRE_FIRMWARE_BRANCH))
      set(MNTRE_FIRMWARE_BRANCH $ENV{CI_COMMIT_BRANCH})
    else()
      set(MNTRE_FIRMWARE_BRANCH "FIXME")
    endif()
    set(FWCAB_RELEASE_SOURCE_URL "https://source.mnt.re/reform/pocket-reform/branch/${MNTRE_FIRMWARE_BRANCH}")
    set(FWCAB_FIRMWARE_DESCRIPTION "<p>Development build for testing</p>")
    set(FWCAB_RELEASE_DESCRIPTION_FILE "${CMAKE_CURRENT_LIST_DIR}/description-debug.html")
    message("Using MNTRE_FIRMWARE_VERSION ${MNTRE_FIRMWARE_VERSION} for debug build")
    message("Using FWCAB_RELEASE_SOURCE_URL ${FWCAB_RELEASE_SOURCE_URL} for debug build")
endif ()

target_compile_definitions(sysctl PRIVATE -DMNTRE_FIRMWARE_VERSION=${MNTRE_FIRMWARE_VERSION})

pico_set_program_version(sysctl ${MNTRE_FIRMWARE_VERSION})
set(FWCAB_RELEASE_VERSION ${MNTRE_FIRMWARE_VERSION})
set(FWCAB_FIRMWARE_ID "com.mntre.pocket_reform.sysctl10.firmware")
set(FWCAB_FIRMWARE_NAME "Pocket Reform System Controller 1.0 Firmware")
set(FWCAB_SUMMARY "MNT Pocket Reform System Controller")
set(FWCAB_CATEGORY "X-EmbeddedController")
set(FWCAB_FIRMWARE_LICENSE "GPL-3.0-or-later")
set(FWCAB_RELEASE_VERSION_FORMAT "number")
set(FWCAB_DEVICE_GUID "df0c9fc8-88d6-5100-ba66-0f5c436fc522")
set(FWCAB_DEVICE_INTEGRITY "unsigned")
set(FWCAB_UPDATE_PROTOCOL "com.microsoft.uf2")
set(FWCAB_HOMEPAGE "https://mntre.com")

add_custom_command(
	TARGET sysctl
	POST_BUILD
    VERBATIM
    COMMAND "${CMAKE_CURRENT_LIST_DIR}/../scripts/firmware_packager.py" --bin "${CMAKE_CURRENT_BINARY_DIR}/sysctl.uf2" --out "${CMAKE_CURRENT_BINARY_DIR}/sysctl.cab" --firmware-id "${FWCAB_FIRMWARE_ID}" --firmware-name "${FWCAB_FIRMWARE_NAME}" --firmware-description "${FWCAB_FIRMWARE_DESCRIPTION}" --firmware-license "${FWCAB_FIRMWARE_LICENSE}" --device-guid "${FWCAB_DEVICE_GUID}" --device-integrity "${FWCAB_DEVICE_INTEGRITY}" --homepage "${FWCAB_HOMEPAGE}" --firmware-summary "${FWCAB_SUMMARY}" --category "${FWCAB_CATEGORY}" --version-format "${FWCAB_RELEASE_VERSION_FORMAT}" --release-version "${FWCAB_RELEASE_VERSION}" --release-description-file "${FWCAB_RELEASE_DESCRIPTION_FILE}" --release-source-url "${FWCAB_RELEASE_SOURCE_URL}" --developer-name "${FWCAB_DEVELOPER_NAME}" --update-protocol "${FWCAB_UPDATE_PROTOCOL}"
)

configure_file("ds20.quirk.tpl" "ds20.quirk" @ONLY)

add_custom_command(
	OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/inc-generated/ds20-descriptor.h"
	COMMAND "${CMAKE_CURRENT_LIST_DIR}/../scripts/generate-ds20.py" --output "${CMAKE_CURRENT_BINARY_DIR}/inc-generated/ds20-descriptor.h" "${CMAKE_CURRENT_BINARY_DIR}/ds20.quirk"
	DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/ds20.quirk"
)
