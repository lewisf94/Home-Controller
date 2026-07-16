# ESP32-P4 (Waveshare 4.3") -- Music Controller, Home Assistant backend.
#
# Same board and UI as waveshare/esp-idf/, but the backend is Home Assistant
# over a local WebSocket (Music Assistant media_player entity) instead of the
# Spotify Web API. HA OS on a Pi handles the Spotify integration, eliminating
# on-device OAuth refresh, fixing phone volume, and enabling real-time push state.
#
# Toolchain: ESP-IDF 5.5.x (NOT 5.4 / 6.0) -- same constraint as the non-HA build.
cmake_minimum_required(VERSION 3.16)

# Pull in the shared P4 component (ui, audio, albums, fonts, knob protocol)
# that lives one level up. Per-build files (main.c, ha_client.c) stay in main/.
list(APPEND EXTRA_COMPONENT_DIRS ../components)

# The vendored BSP sits inside the non-HA build tree; point to it rather than
# duplicating it. Components in any listed directory are found automatically.
list(APPEND EXTRA_COMPONENT_DIRS ../esp-idf/components)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(music_controller_p4_ha)

# Force LV_USE_OBSERVER=1 (same workaround as the non-HA build).
idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
if(lvgl_lib)
    target_compile_definitions(${lvgl_lib} PUBLIC LV_USE_OBSERVER=1)
endif()

# The shared UI is linked into both P4 builds. Only the HA build has a backend
# for light.* entities, so only this target exposes the Lights entry point.
# P4_BACKEND_HA additionally tells the shared UI which backend it serves
# (currently: which credential rows Settings > SETUP shows -- the HA build
# carries no Spotify credentials by design).
idf_component_get_property(p4_shared_lib p4_shared COMPONENT_LIB)
if(p4_shared_lib)
    target_compile_definitions(${p4_shared_lib} PUBLIC P4_HAS_HA_LIGHTS=1 P4_BACKEND_HA=1)
endif()

# sendspin-cpp 0.6.1 trips a GCC 14 false positive while move-assigning a
# std::optional<std::string> inside ShadowSlot. Keep warnings-as-errors for our
# components; demote only this one diagnostic on the third-party target.
idf_component_get_property(sendspin_lib sendspin__sendspin-cpp COMPONENT_LIB)
if(sendspin_lib)
    target_compile_options(${sendspin_lib} PRIVATE -Wno-error=maybe-uninitialized)
endif()
