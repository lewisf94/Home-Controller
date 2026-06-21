# Per-build sources: direct Spotify backend + Sonos local control.
# Everything shared with the HA build (UI, audio, albums, fonts, knob) lives
# in waveshare/components/p4_shared/ and is pulled in via p4_shared below.
idf_component_register(
    SRCS
        "main.c"
        "spotify.c"       # Spotify Web API client (esp_http_client / mbedtls)
        "sonos.c"         # local UPnP control for a (restricted) Sonos speaker
    INCLUDE_DIRS "." "../include"
    PRIV_REQUIRES
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        esp_http_client
        esp-tls
        mbedtls
        esp_timer
        vfs
        joltwallet__littlefs
        bitbank2__jpegdec
        esp_codec_dev
        nanopb
        p4_shared              # shared UI, audio, albums, fonts, knob protocol
        esp32_p4_wifi6_touch_lcd_4_3
        lvgl__lvgl
)
