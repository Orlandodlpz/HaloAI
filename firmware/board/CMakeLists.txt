# ESP-IDF application for the device.
#
# Build: source ~/esp/esp-idf/export.sh && cd firmware/board && idf.py set-target esp32s3 && idf.py build
# Flash: idf.py -p /dev/cu.usbserial-110 flash monitor
cmake_minimum_required(VERSION 3.16)
# Point at the component itself, not at firmware/, or IDF tries to treat
# firmware/CMakeLists.txt (the host test build) as a component and fails on
# its project() call.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../core")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(halo_device)
