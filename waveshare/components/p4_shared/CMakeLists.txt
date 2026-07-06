# Shared P4 component: UI, audio, albums, fonts, knob protocol.
#
# Everything here is common to both waveshare/esp-idf (direct Spotify) and
# waveshare/esp-idf-ha (Home Assistant backend). Per-build files that differ
# between the two builds (main.c, spotify.c / ha_client.c, sonos.c) stay in
# each build's own main/ component.
#
# Include-path layout:
#   p4_shared/include/  -- public headers (ui.h, audio.h, albums.h, …)
#   p4_shared/          -- sources + font .c files + TTF EMBED_FILES
#
# When the build's main/ #includes "ui.h", IDF finds it via this component's
# public INCLUDE_DIRS because main declares REQUIRES (or PRIV_REQUIRES)
# p4_shared in its own idf_component_register.

# --- Keep albums.c in sync with the master list automatically ---------------
# albums.c is GENERATED from spotify-albums-list.txt (repo root) by
# scripts/gen_albums.py. The CMake configure-depends hook here mirrors the
# one that used to live in the non-HA main/CMakeLists.txt.
get_filename_component(_repo_root "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(_albums_list "${_repo_root}/spotify-albums-list.txt")
set(_gen_albums  "${_repo_root}/scripts/gen_albums.py")
if(NOT PYTHON)
    set(PYTHON "python")
endif()
if(EXISTS "${_albums_list}" AND EXISTS "${_gen_albums}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_albums_list}")
    execute_process(
        COMMAND "${PYTHON}" "${_gen_albums}"
        WORKING_DIRECTORY "${_repo_root}"
        RESULT_VARIABLE _gen_albums_rc)
    if(_gen_albums_rc EQUAL 0)
        message(STATUS "p4_shared: albums.c regenerated from spotify-albums-list.txt")
    else()
        message(WARNING "p4_shared: gen_albums.py failed (rc=${_gen_albums_rc}); using existing albums.c")
    endif()
else()
    message(STATUS "p4_shared: spotify-albums-list.txt not found; using committed albums.c")
endif()

# --- Guard: albums.c and album_thumbs.bin must stay 1:1 ----------------------
# The browser indexes thumbnails positionally against albums.c, so a count
# mismatch shows wrong / blank covers. This fails the build loudly if they drift
# apart -- exactly what the p4_shared refactor caused when gen_albums.py /
# embed_albums_idf.py kept writing to the old waveshare/esp-idf/main/ path while
# the build compiled the stale committed albums.c here. If you MOVE these
# generated files, update BOTH scripts' target lists (TARGETS / OUT_TARGETS).
set(_albums_c   "${CMAKE_CURRENT_LIST_DIR}/albums.c")
set(_thumbs_bin "${CMAKE_CURRENT_LIST_DIR}/album_thumbs.bin")
if(EXISTS "${_albums_c}" AND EXISTS "${_thumbs_bin}")
    file(STRINGS "${_albums_c}" _album_lines REGEX "spotify:album:")
    list(LENGTH _album_lines _n_albums)
    file(SIZE "${_thumbs_bin}" _bin_size)
    math(EXPR _n_thumbs "${_bin_size} / (220 * 220 * 2)")   # 220x220 RGB565
    if(NOT _n_albums EQUAL _n_thumbs)
        message(FATAL_ERROR
            "p4_shared: album desync -- albums.c has ${_n_albums} albums but "
            "album_thumbs.bin has ${_n_thumbs} thumbnails. Rebake the blob:\n"
            "  python scripts/embed_albums_idf.py")
    endif()
endif()

idf_component_register(
    SRCS
        "ui.c"
        "audio.c"
        "albums.c"
        "album_catalog.c"
        "album_thumbs.c"
        "album_art.cpp"
        "littlefs.c"
        "lv_font_pixel_16.c"
        "lv_font_pixel_24.c"
        "lv_font_arvo_24.c"
        "lv_font_arvo_28.c"
        "lv_font_dot_24.c"
        "lv_font_dot_sym_24.c"
        "lv_font_mono_16.c"
        "lv_font_mono_24.c"
        "home_controller.pb.c"
        "knob.c"
        "knob_input.c"
    INCLUDE_DIRS "include"
    EMBED_FILES
        "album_thumbs.bin"          # browser thumbnail blob (gitignored; baked in private)
        "Montserrat-Medium.ttf"     # runtime TTF for title/artist labels (primary)
        "DejaVuSans.ttf"            # runtime TTF fallback for accented/non-Latin glyphs
        "Arvo-Bold.ttf"             # slab-serif font choice (Settings -> FONT -> SLAB)
    REQUIRES
        lvgl__lvgl
        esp32_p4_wifi6_touch_lcd_4_3   # vendored BSP (waveshare/esp-idf/components/)
    PRIV_REQUIRES
        nvs_flash
        esp_wifi
        esp_timer
        vfs
        joltwallet__littlefs
        bitbank2__jpegdec
        esp_codec_dev
        nanopb
        esp_hw_support
        esp_system
        freertos
)

# Link libm for cosf/sinf/tanf used by the WiFi orbit animation in ui.c.
idf_component_get_property(p4_shared_lib p4_shared COMPONENT_LIB)
target_link_libraries(${p4_shared_lib} PRIVATE m)
