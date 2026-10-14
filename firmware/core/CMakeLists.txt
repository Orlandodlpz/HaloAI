# ESP-IDF component wrapper. The host build (firmware/CMakeLists.txt) lists these
# same sources directly and never reads this file.
idf_component_register(
    SRCS "src/gestures.cpp" "src/state_machine.cpp" "src/names.cpp" "src/layout.cpp" "src/audio.cpp"
    INCLUDE_DIRS "include")
