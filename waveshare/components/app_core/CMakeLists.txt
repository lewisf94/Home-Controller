# Shared app scaffolding for the waveshare/esp-idf and waveshare/esp-idf-ha
# builds: WiFi connect + resilient background reconnect, and the
# double-buffered PSRAM album-art buffer lifecycle. Per-build code (the
# backend client, the command queue vocabulary, Sonos local control) stays in
# each build's own main/ component -- see the "app_core" note in
# docs/PENDING.md for what was deliberately NOT extracted and why.
idf_component_register(
    SRCS
        "wifi.c"
        "art_buffer.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        esp_wifi
        esp_netif
        esp_event
        esp_timer
        p4_shared     # ui_art_refresh()
)
