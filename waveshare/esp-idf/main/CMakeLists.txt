# Sources/deps are introduced per checkpoint so each stage links on its own.
# The full stack (display + WiFi + TLS + http + jpegdec + littlefs) linked at
# once overflows internal SRAM by ~451 B (static .bss/.sdata), so we stage it.
idf_component_register(
    SRCS
        "main.c"
        "spotify.c"       # cp3: Spotify Web API client (+ esp_http_client/esp-tls/mbedtls)
        "albums.c"        # cp3: album list / metadata
        # --- added in later checkpoints (see README.md roadmap) ---
        # "album_art.cpp"   # cp5: JPEGDEC now-playing art (+ bitbank2__jpegdec, vfs)
        # "littlefs.c"      # cp5: album-art scratch (+ joltwallet__littlefs)
        # "album_thumbs.c"  # cp5: embedded thumbnails (+ EMBED_FILES album_thumbs.bin)
        # "ui.c"            # cp4: ported from cyd, re-laid-out 800x480
        # "input.c"         # cp6: touch dispatch
    INCLUDE_DIRS "." "../include"
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
        # vfs joltwallet__littlefs bitbank2__jpegdec                     # cp5
)
