# The board is chosen once, in ../CMakeLists.txt, from -DHALO_PANEL.
#   idf.py -DHALO_PANEL=crowpanel build   (default, the development board)
#   idf.py -DHALO_PANEL=waveshare build   (the badge)
#
# It selects the display backend source here and, through the matching
# HALO_PANEL_* definition, the pin map in board_pins.hpp. Never ifdefs inside
# UI code.
string(TOUPPER "${HALO_PANEL}" HALO_PANEL_UPPER)

idf_component_register(
    SRCS "main.cpp" "ble.cpp" "wallpaper_store.cpp" "mic.cpp" "display_${HALO_PANEL}.cpp"
    INCLUDE_DIRS "."
    REQUIRES driver esp_timer core bt nvs_flash esp_driver_i2c
    EMBED_FILES "test_clip.pcm")

# Definitions go on the component target, AFTER registration, not through
# add_compile_definitions(): component CMakeLists are also evaluated in script
# mode while IDF works out component requirements, and add_compile_definitions
# does not exist there. It fails with "Unknown CMake command", which reads like
# a broken toolchain rather than a misplaced call.
target_compile_definitions(${COMPONENT_LIB} PUBLIC HALO_PANEL_${HALO_PANEL_UPPER})

# Bench build: lets serial walk the state machine with no buttons and no radio.
# Turn on with -DHALO_BENCH_INJECT=1
#
# "Off by default" is only true of a fresh build directory. The flag is a CMake
# cache variable, so once it has been passed it STAYS passed: a later plain
# `idf.py build` still produces a bench binary. Clear it with `idf.py fullclean`,
# and trust the boot log, which says which build is running, over the command
# line that produced it.
if(HALO_BENCH_INJECT)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC HALO_BENCH_INJECT)
    message(WARNING "HALO_BENCH_INJECT is ON: gestures can be injected over serial. "
                    "This flag is sticky in the CMake cache. Run `idf.py fullclean` "
                    "to get an ordinary build back.")
endif()
