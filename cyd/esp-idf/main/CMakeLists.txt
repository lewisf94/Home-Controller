idf_component_register(
    SRCS
        "main.c"
        "spotify.c"
    INCLUDE_DIRS "." "../include"
    PRIV_REQUIRES
        esp_http_client
        esp-tls
        mbedtls
        nvs_flash
        esp_wifi
        esp_event
        esp_netif
)
