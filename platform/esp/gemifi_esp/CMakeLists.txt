# gemifi_esp: the gemifi ESP platform (ESP32 / S2 / S3) as an ESP-IDF component:
# gm_hal.h and config_store.h on ESP-IDF, plus the controller core and the device
# config, so a firmware project's main only calls gm_esp_begin() and gm_poll().
#
# firmware/esp/gemifi_esp.py (PlatformIO's pre: script) sets, before CMake runs:
#   GM_CORE        all: core/src/*.c and the device config; gsx (default): core/src/gsx.c
#   GM_DEVICE_DIR  the generated gm_device_config.h/.c (GM_CORE=all)
#
# SPDX-License-Identifier: MIT
set(gm_root ${CMAKE_CURRENT_LIST_DIR}/../../..)

set(srcs hal_esp.c midi_esp.c config_store_esp.c fw_esp.c ledstrip_esp.c)
set(incs . ${gm_root}/core/include)

if("$ENV{GM_CORE}" STREQUAL "all")
    file(GLOB core_srcs ${gm_root}/core/src/*.c)
    list(APPEND srcs ${core_srcs})
    if(NOT DEFINED ENV{GM_DEVICE_DIR})
        message(FATAL_ERROR "gemifi_esp: GM_CORE=all needs GM_DEVICE_DIR (the generated device config)")
    endif()
    file(GLOB device_srcs $ENV{GM_DEVICE_DIR}/*.c)
    list(APPEND srcs ${device_srcs})
    list(APPEND incs $ENV{GM_DEVICE_DIR})
else()
    list(APPEND srcs ${gm_root}/core/src/gsx.c)
endif()

# Native USB (TinyUSB) on the S2 / S3, Bluetooth (NimBLE) on the ESP32 / S3: required here, only
# linked when the transport uses them (sdkconfig.usb / sdkconfig.ble turn them on).
idf_build_get_property(target IDF_TARGET)
set(reqs esp_timer esp_driver_gpio esp_driver_i2c esp_driver_uart esp_driver_rmt esp_adc
         nvs_flash app_update esp_partition esp_hw_support esp_system)
if(target STREQUAL "esp32s2" OR target STREQUAL "esp32s3")
    list(APPEND reqs esp_tinyusb)
endif()
if(target STREQUAL "esp32" OR target STREQUAL "esp32s3")
    list(APPEND reqs bt)
endif()

# WHOLE_ARCHIVE: the core's weak hal_* defaults (vpin.c) would otherwise keep the strong ones in
# files nothing else pulls in (ledstrip_esp.c) out of the link.
idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS ${incs}
                       PRIV_INCLUDE_DIRS ${gm_root}/core/src
                       PRIV_REQUIRES ${reqs}
                       WHOLE_ARCHIVE)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall)
# hello's board id, for an OTA from it to a device's firmware (README "Test firmware"); a device
# firmware takes its own from gm_device_config.h.
if(DEFINED ENV{GM_BOARD_ID} AND NOT "$ENV{GM_CORE}" STREQUAL "all")
    target_compile_definitions(${COMPONENT_LIB} PUBLIC GM_BOARD_ID=$ENV{GM_BOARD_ID})
endif()
