idf_component_register(SRCS "wifi_manager.c"
                       PRIV_REQUIRES esp_driver_gpio esp_wifi esp_event esp_netif nvs_flash esp_http_client lwip esp_http_server
                       INCLUDE_DIRS "."
                       EMBED_FILES "index.html")
