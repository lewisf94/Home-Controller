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
        # --- added in later checkpoints (see README.md roadmap) ---
        # "input.c"         # cp6: touch dispatch
    INCLUDE_DIRS "." "../include"
    EMBED_FILES
        "album_thumbs.bin"          # cp4: browser thumbnail blob (gitignored; baked in private)
        "Montserrat-Medium.ttf"     # runtime TTF for title/artist labels (primary)
        "DejaVuSans.ttf"            # runtime TTF fallback for accented/non-Latin glyphs
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
