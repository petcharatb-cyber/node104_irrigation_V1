idf_component_register(
    SRCS
        "main.c"
        "deep_sleep.c"
        "ds3231.c"
        "firebase_client.c"
        "time_sync.c"
        "wifi_manager.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "firebase_root.pem"
    REQUIRES
        esp_http_client
        esp_driver_gpio
        esp_driver_i2c
        esp_event
        esp_hw_support
        esp_netif
        esp_wifi
        espressif__cjson
        nvs_flash
)
