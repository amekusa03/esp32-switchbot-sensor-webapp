idf_component_register(
    SRCS
        "main.cpp"
        "cds_sensor.c"
        "nvs_config.c"
        "wifi_manager.c"
        "switchbot_api.c"
        "web_server.c"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        esp_wifi
        esp_event
        esp_netif
        esp_http_server
        esp_http_client
        esp-tls
        mbedtls
        json
        log
        lwip
        mdns
        esp_matter
        esp_adc
        driver
)

