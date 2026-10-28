# ESP-IDF application for the device.
#
# Build and flash the badge:
#   source ~/esp/esp-idf/export.sh
#   cd firmware/board
#   idf.py -DHALO_PANEL=waveshare set-target esp32s3
#   idf.py -DHALO_PANEL=waveshare build
#   idf.py -DHALO_PANEL=waveshare -p /dev/cu.usbmodem101 flash monitor
#
# The development board is the same with -DHALO_PANEL=crowpanel (the default)
# and a /dev/cu.usbserial-* port.
cmake_minimum_required(VERSION 3.16)

# One build flag picks the board, everywhere: the display backend, the pin map,
# the flash size, the console and the partition table.
if(NOT DEFINED HALO_PANEL)
    set(HALO_PANEL "crowpanel" CACHE STRING "Target board: crowpanel or waveshare")
endif()
if(NOT HALO_PANEL STREQUAL "crowpanel" AND NOT HALO_PANEL STREQUAL "waveshare")
    message(FATAL_ERROR "HALO_PANEL must be crowpanel or waveshare, not '${HALO_PANEL}'")
endif()

# Common defaults first, then the board's own on top.
set(SDKCONFIG_DEFAULTS
    "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults"
    "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.${HALO_PANEL}")

# A SEPARATE generated sdkconfig per board, which is what stops the two boards
# poisoning each other.
#
# `idf.py build` never applies a changed sdkconfig.defaults to an sdkconfig that
# already exists: the defaults seed it once and are ignored afterwards. With one
# shared sdkconfig, switching boards therefore kept the other board's flash
# size, console and partition table while cheerfully compiling the new board's
# code, and the only symptom is a board that boots and prints nothing. Giving
# each board its own file means the first build for a board always seeds from
# that board's defaults.
set(SDKCONFIG "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.${HALO_PANEL}")

# Point at the component itself, not at firmware/, or IDF tries to treat
# firmware/CMakeLists.txt (the host test build) as a component and fails on
# its project() call.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../core")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(halo_device)
