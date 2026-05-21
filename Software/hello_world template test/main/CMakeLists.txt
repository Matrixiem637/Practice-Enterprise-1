idf_component_register(SRCS "wifi_manager.c" "main.c"
                       PRIV_REQUIRES esp_driver_gpio esp_wifi esp_event esp_netif nvs_flash
                       INCLUDE_DIRS ".")
