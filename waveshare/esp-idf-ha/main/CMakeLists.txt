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
        vfs
        joltwallet__littlefs
        bitbank2__jpegdec
        p4_shared           # shared UI, audio, albums, fonts, knob protocol
        app_core             # shared WiFi connect/reconnect + art buffer
        esp32_p4_wifi6_touch_lcd_4_3
        lvgl__lvgl
)
