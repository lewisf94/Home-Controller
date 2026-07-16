# HA build per-build sources: HA WebSocket backend only.
# Everything shared (UI, audio, albums, fonts, knob) comes from p4_shared.
idf_component_register(
    SRCS
        "main.c"
        "ha_client.c"   # HA WebSocket backend (replaces spotify.c + sonos.c)
    INCLUDE_DIRS "." "../include"
    PRIV_REQUIRES
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        esp_timer
        esp_websocket_client
        esp_http_client    # art download (HTTP GET to HA image proxy)
        esp-tls            # Spotify HTTPS certificate bundle
        mbedtls            # Spotify Basic auth base64 helper
        vfs
        joltwallet__littlefs
        bitbank2__jpegdec
        p4_shared           # shared UI, audio, albums, fonts, knob protocol
        sendspin_player      # native Music Assistant playback through the ES8311
        app_core             # shared WiFi connect/reconnect + art buffer
        esp32_p4_wifi6_touch_lcd_4_3
        lvgl__lvgl
)

# RP2040 haptic knob: forward the CMake cache flag to the compiler so
# `idf.py build -DKNOB_ENABLED=1` enables the knob without editing main.c.
# The value is STICKY in the CMake cache -- turn it back off with
# `idf.py build -DKNOB_ENABLED=0` (merely omitting the flag keeps it on).
if(KNOB_ENABLED)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE KNOB_ENABLED=1)
endif()
