# Host build of the hardware independent layer and its tests.
#
# This builds on a laptop with nothing but a C++17 compiler: core/ contains no
# board headers, no vendor library and no framework. The ESP32 build (Arduino core
# or ESP-IDF, not chosen yet) compiles the same sources.
cmake_minimum_required(VERSION 3.16)
project(halo_firmware_core CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(halo_core
    core/src/gestures.cpp
    core/src/state_machine.cpp
    core/src/names.cpp
)
target_include_directories(halo_core PUBLIC core/include)
target_compile_options(halo_core PRIVATE -Wall -Wextra -Wpedantic -Werror)

enable_testing()
add_executable(halo_tests
    test/test_main.cpp
    test/test_gestures.cpp
    test/test_state_machine.cpp
)
target_link_libraries(halo_tests PRIVATE halo_core)
target_include_directories(halo_tests PRIVATE test)
target_compile_options(halo_tests PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME core COMMAND halo_tests)
