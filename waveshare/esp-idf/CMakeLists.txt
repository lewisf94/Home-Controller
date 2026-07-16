# ESP32-P4 (Waveshare 4.3") -- Music Controller, direct Spotify (no HA).
cmake_minimum_required(VERSION 3.16)

# Pull in the shared P4 component (ui, audio, albums, fonts, knob protocol)
# that lives one level up so both this build and waveshare/esp-idf-ha link
# the same code. Per-build files (main.c, spotify.c, sonos.c) stay in main/.
list(APPEND EXTRA_COMPONENT_DIRS ../components)

# The shared Sendspin player lives beside p4_shared but is HA-first. Keep the
# direct-Spotify image free of its decoder/network dependency until that backend
# has a Music Assistant route of its own.
set(EXCLUDE_COMPONENTS sendspin_player)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(music_controller_p4)

# Force LV_USE_OBSERVER=1 on the LVGL component. LVGL 9.5.0 compiles
# lv_observer.c unconditionally but the types it uses live behind
# #if LV_USE_OBSERVER in the headers; the Kconfig default doesn't reach
# lv_conf_internal.h reliably here, so define the macro directly. (Same
# workaround as the CYD esp-idf build.)
idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
if(lvgl_lib)
    target_compile_definitions(${lvgl_lib} PUBLIC LV_USE_OBSERVER=1)
endif()
