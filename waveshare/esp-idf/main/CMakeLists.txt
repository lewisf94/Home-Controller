# --- Keep albums.c in sync with the master list automatically ---------------
# albums.c is GENERATED from spotify-albums-list.txt (repo root) by
# scripts/gen_albums.py. Running it here means "edit the list, then build" is
# all that's needed -- never hand-edit albums.c, never run gen_albums.py by hand.
# CMake re-runs configuration whenever the list changes (CMAKE_CONFIGURE_DEPENDS)
# and regenerates albums.c below. (Cover ART is separate: after adding an image
# to scripts/input_albums/, run scripts/embed_albums_idf.py to refresh the thumb
# blob; a newly listed album with no cover yet shows a placeholder tile.)
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
        message(STATUS "albums.c regenerated from spotify-albums-list.txt")
    else()
        message(WARNING "gen_albums.py failed (rc=${_gen_albums_rc}); using existing albums.c")
    endif()
else()
    message(STATUS "spotify-albums-list.txt not found; using committed albums.c")
endif()

# Sources/deps are introduced per checkpoint so each stage links on its own.
# The full stack (display + WiFi + TLS + http + jpegdec + littlefs) linked at
# once overflows internal SRAM by ~451 B (static .bss/.sdata), so we stage it.
idf_component_register(
    SRCS
        "main.c"
        "spotify.c"       # cp3: Spotify Web API client (+ esp_http_client/esp-tls/mbedtls)
        "sonos.c"         # local UPnP control for a (restricted) Sonos -- Spotify can't drive it
        "albums.c"        # cp3: album list / metadata
        "ui.c"            # cp4: ported from cyd, re-laid-out 800x480
        "album_thumbs.c"  # cp4: embedded browser thumbnails (+ EMBED_FILES album_thumbs.bin)
        "album_art.cpp"   # cp5: JPEGDEC now-playing art (+ bitbank2__jpegdec, vfs)
        "littlefs.c"      # cp5: album-art scratch (+ joltwallet__littlefs)
        "lv_font_pixel_16.c"  # cp6: 1bpp Press Start 2P 16px for PIXEL retro theme
        "lv_font_pixel_24.c"  # cp6: 1bpp Press Start 2P 24px for PIXEL retro theme
        "lv_font_arvo_24.c"   # SLAB font choice: Arvo Bold 24px (gen_lvgl_font.py)
        "lv_font_arvo_28.c"   # SLAB font choice: Arvo Bold 28px (gen_lvgl_font.py)
        "lv_font_dot_24.c"      # GLYPH dot-matrix HEADING font 24px (gen_lvgl_font.py --dots)
        "lv_font_dot_sym_24.c"  # dotted FontAwesome fallback of dot_24 (symbols in headings)
        "lv_font_mono_16.c"     # PAPER teletype text font 16px (unscii-8, gen_lvgl_font.py)
        "lv_font_mono_24.c"     # PAPER teletype text font 24px (unscii-8, gen_lvgl_font.py)
        "audio.c"             # synthesised UI sound effects via ES8311 speaker
        "home_controller.pb.c" # nanopb generated: RP2040 co-MCU protocol messages
        "knob.c"               # UART driver for haptic RP2040 co-MCU
        "knob_input.c"         # context-aware input mapper (4-menu model)
        # --- added in later checkpoints (see README.md roadmap) ---
        # "input.c"         # cp6: touch dispatch
    INCLUDE_DIRS "." "../include"
    EMBED_FILES
        "album_thumbs.bin"          # cp4: browser thumbnail blob (gitignored; baked in private)
        "Montserrat-Medium.ttf"     # runtime TTF for title/artist labels (primary)
        "DejaVuSans.ttf"            # runtime TTF fallback for accented/non-Latin glyphs
        "Arvo-Bold.ttf"             # cp7: slab-serif font choice (Settings → FONT → SLAB)
    PRIV_REQUIRES
        nvs_flash
        lvgl__lvgl
        esp32_p4_wifi6_touch_lcd_4_3   # vendored BSP (../components/); pulls st7701 + gt911 + esp_lvgl_adapter + lvgl
        # --- re-added per checkpoint ---
        esp_wifi          # cp2: provides esp_wifi.h; implementation routed to C6 via esp_wifi_remote
        esp_wifi_remote   # cp2: redirects esp_wifi calls to the onboard C6 over SDIO
        esp_netif         # cp2
        esp_event         # cp2
        esp_http_client   # cp3: Spotify Web API HTTPS
        esp-tls           # cp3: TLS + cert bundle
        mbedtls           # cp3: base64 (token auth), TLS
        esp_timer         # cp3: token-expiry timing
        vfs                   # cp5: POSIX fopen/fread for album_art_decode_file
        joltwallet__littlefs  # cp5: album-art JPEG scratch (storage partition)
        bitbank2__jpegdec     # cp5: now-playing art JPEG decode
        esp_codec_dev         # UI sound effects: ES8311 speaker (audio.c)
        nanopb                # vendored protobuf runtime (components/nanopb/)
)

# Link libm for cosf/sinf used by the WiFi orbit animation in ui.c.
idf_component_get_property(main_lib main COMPONENT_LIB)
target_link_libraries(${main_lib} PRIVATE m)

# Silence warnings from the third-party bitbank JPEGDEC library (pointer-to-int
# casts, shift-of-negative, unused statics). They're benign and not our code;
# this targets only that component's lib so our own warnings still surface.
# Lives here (not in managed_components) so it survives dependency regeneration.
idf_component_get_property(jpegdec_lib bitbank2__jpegdec COMPONENT_LIB)
if(jpegdec_lib)
    target_compile_options(${jpegdec_lib} PRIVATE -w)
endif()
