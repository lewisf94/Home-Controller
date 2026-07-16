idf_component_register(
    SRCS "sendspin_player.cpp"
    INCLUDE_DIRS "."
    PRIV_INCLUDE_DIRS "../p4_shared"
    REQUIRES
        p4_shared
        sendspin-cpp
        mdns
    PRIV_REQUIRES
        esp_wifi
        esp_timer
        esp_hw_support
        nvs_flash
        freertos
)

