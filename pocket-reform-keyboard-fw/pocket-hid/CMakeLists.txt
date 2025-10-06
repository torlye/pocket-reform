cmake_minimum_required(VERSION 3.17)

if (NOT DEFINED ENV{PICO_SDK_PATH} AND (NOT PICO_SDK_PATH))
       set(PICO_SDK_PATH "${CMAKE_SOURCE_DIR}/../../pico-sdk")
       cmake_path(NORMAL_PATH PICO_SDK_PATH)
       message("Using PICO_SDK_PATH=${PICO_SDK_PATH}")
endif()

include(pico_sdk_import.cmake)
set(SHARED_FW_INC "${CMAKE_CURRENT_LIST_DIR}/../../pocket-reform-shared-fw/inc")
set(SHARED_FW_SRC "${CMAKE_CURRENT_LIST_DIR}/../../pocket-reform-shared-fw/src")
set(SCRIPTS "${CMAKE_CURRENT_LIST_DIR}/../../scripts")

set(PROJECT pocket-hid)

project(${PROJECT} C CXX ASM)

pico_sdk_init()

add_executable(${PROJECT})

# Example source
target_sources(${PROJECT} PUBLIC
        src/main.c
        src/usb_descriptors.c
        src/oled.c
        src/remote.c
        src/menu.c
        src/hardfault_handler.c
        src/usb_descriptors.c
        "${CMAKE_CURRENT_BINARY_DIR}/inc-generated/ds20-descriptor.h"
        "${SHARED_FW_SRC}/reset_interface.c"
        )

# Example include
target_include_directories(${PROJECT} PUBLIC
        "${CMAKE_CURRENT_LIST_DIR}/inc"
        "${CMAKE_CURRENT_LIST_DIR}/src"
        "${CMAKE_CURRENT_BINARY_DIR}/inc-generated"
        "${SHARED_FW_INC}"
        )

target_link_libraries(${PROJECT} PRIVATE
        pico_stdlib
        pico_bootsel_via_double_reset
        hardware_i2c
        hardware_pwm
        hardware_pio
        hardware_watchdog
        pico_unique_id
        tinyusb_device
)

pico_add_extra_outputs(${PROJECT})

target_compile_definitions(${PROJECT} PUBLIC PICO_RP2040_USB_DEVICE_ENUMERATION_FIX=1)
target_compile_definitions(${PROJECT} PUBLIC PICO_XOSC_STARTUP_DELAY_MULTIPLIER=64)
target_compile_definitions(${PROJECT} PUBLIC RESET_INTERFACE_ENABLE_RESET_VIA_BAUD_RATE=0)

if (DEFINED ENV{MNTRE_FIRMWARE_VERSION} AND (NOT MNTRE_FIRMWARE_VERSION))
    set(MNTRE_FIRMWARE_VERSION $ENV{MNTRE_FIRMWARE_VERSION})
    set(FWCAB_DEVELOPER_NAME "MNT Research GmbH")
    set(FWCAB_RELEASE_SOURCE_URL "https://source.mnt.re/reform/pocket-reform/-/tags/${MNTRE_FIRMWARE_VERSION}")
    set(FWCAB_FIRMWARE_DESCRIPTION "<p>MNT Research Pocket Reform Input Firmware Update (Release Build). The Input (HID) Controller manages the built-in keyboard, trackball and OLED status display. Firmware updates can improve responsiveness and power usage.</p>")
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

target_compile_definitions(${PROJECT} PRIVATE -DMNTRE_FIRMWARE_VERSION="${MNTRE_FIRMWARE_VERSION}")

pico_set_program_version(${PROJECT} ${MNTRE_FIRMWARE_VERSION})
set(FWCAB_RELEASE_VERSION ${MNTRE_FIRMWARE_VERSION})
set(FWCAB_FIRMWARE_ID "com.mntre.pocket_reform.input10.firmware")
set(FWCAB_FIRMWARE_NAME "Pocket Reform Input 1.0 Firmware")
set(FWCAB_SUMMARY "MNT Pocket Reform Input")
set(FWCAB_CATEGORY "X-Keyboard")
set(FWCAB_FIRMWARE_LICENSE "GPL-3.0-or-later")
set(FWCAB_RELEASE_VERSION_FORMAT "number")
set(FWCAB_DEVICE_GUID "a515e8b3-d950-5228-a136-ebaed3e376e8") # GUID(USB\VID_1209&PID_6D06)
set(FWCAB_DEVICE_INTEGRITY "unsigned")
set(FWCAB_UPDATE_PROTOCOL "com.microsoft.uf2")
set(FWCAB_INSTALL_DURATION "120")
set(FWCAB_HOMEPAGE "https://mntre.com")

add_custom_command(
    TARGET ${PROJECT}
    POST_BUILD
    VERBATIM
    COMMAND "${SCRIPTS}/firmware_packager.py" --bin "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT}.uf2" --out "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT}.cab" --firmware-id "${FWCAB_FIRMWARE_ID}" --firmware-name "${FWCAB_FIRMWARE_NAME}" --firmware-description "${FWCAB_FIRMWARE_DESCRIPTION}" --firmware-license "${FWCAB_FIRMWARE_LICENSE}" --device-guid "${FWCAB_DEVICE_GUID}" --device-integrity "${FWCAB_DEVICE_INTEGRITY}" --homepage "${FWCAB_HOMEPAGE}" --firmware-summary "${FWCAB_SUMMARY}" --category "${FWCAB_CATEGORY}" --version-format "${FWCAB_RELEASE_VERSION_FORMAT}" --release-version "${FWCAB_RELEASE_VERSION}" --release-description-file "${FWCAB_RELEASE_DESCRIPTION_FILE}" --release-source-url "${FWCAB_RELEASE_SOURCE_URL}" --developer-name "${FWCAB_DEVELOPER_NAME}" --update-protocol "${FWCAB_UPDATE_PROTOCOL}" --install-duration "${FWCAB_INSTALL_DURATION}"
)

configure_file("ds20.quirk.tpl" "ds20.quirk" @ONLY)

add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/inc-generated/ds20-descriptor.h"
    COMMAND "${SCRIPTS}/generate-ds20.py" --output "${CMAKE_CURRENT_BINARY_DIR}/inc-generated/ds20-descriptor.h" "${CMAKE_CURRENT_BINARY_DIR}/ds20.quirk"
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/ds20.quirk"
)
