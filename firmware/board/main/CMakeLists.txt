# One build flag picks the panel, never ifdefs inside UI code.
#   idf.py -DHALO_PANEL=crowpanel build   (default, the development board)
#   idf.py -DHALO_PANEL=waveshare build   (the badge; a logging stub until it arrives)
if(NOT DEFINED HALO_PANEL)
    set(HALO_PANEL "crowpanel")
endif()
# Bench build: lets serial walk the state machine with no buttons and no radio.
# Turn on with -DHALO_BENCH_INJECT=1
#
# "Off by default" is only true of a fresh build directory. The flag is a CMake
# cache variable, so once it has been passed it STAYS passed: a later plain
# `idf.py build` still produces a bench binary. Clear it with `idf.py fullclean`,
# and trust the boot log, which says which build is running, over the command
# line that produced it.
if(HALO_BENCH_INJECT)
    add_compile_definitions(HALO_BENCH_INJECT)
    message(WARNING "HALO_BENCH_INJECT is ON: gestures can be injected over serial. "
                    "This flag is sticky in the CMake cache. Run `idf.py fullclean` "
                    "to get an ordinary build back.")
endif()

idf_component_register(
    SRCS "main.cpp" "ble.cpp" "display_${HALO_PANEL}.cpp"
    INCLUDE_DIRS "."
    REQUIRES driver esp_timer core bt nvs_flash
    EMBED_FILES "test_clip.pcm")
