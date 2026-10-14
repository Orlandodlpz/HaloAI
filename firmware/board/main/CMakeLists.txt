# One build flag picks the panel, never ifdefs inside UI code.
#   idf.py -DHALO_PANEL=crowpanel build   (default, the development board)
#   idf.py -DHALO_PANEL=waveshare build   (the badge; a logging stub until it arrives)
if(NOT DEFINED HALO_PANEL)
    set(HALO_PANEL "crowpanel")
endif()
# Bench build: lets serial walk the state machine with no buttons and no radio.
# Off by default so it can never ship. Turn on with -DHALO_BENCH_INJECT=1
if(HALO_BENCH_INJECT)
    add_compile_definitions(HALO_BENCH_INJECT)
endif()

idf_component_register(
    SRCS "main.cpp" "ble.cpp" "display_${HALO_PANEL}.cpp"
    INCLUDE_DIRS "."
    REQUIRES driver esp_timer core bt nvs_flash
    EMBED_FILES "test_clip.pcm")
