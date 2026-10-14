# One build flag picks the panel, never ifdefs inside UI code.
#   idf.py -DHALO_PANEL=crowpanel build   (default, the development board)
#   idf.py -DHALO_PANEL=waveshare build   (the badge; a logging stub until it arrives)
if(NOT DEFINED HALO_PANEL)
    set(HALO_PANEL "crowpanel")
endif()
idf_component_register(
    SRCS "main.cpp" "display_${HALO_PANEL}.cpp"
    INCLUDE_DIRS "."
    REQUIRES driver esp_timer core)
