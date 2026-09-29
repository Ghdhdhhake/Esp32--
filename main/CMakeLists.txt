idf_component_register(SRCS "main.c" "ble_scanner.c"
                    INCLUDE_DIRS "."
                    REQUIRES bt esp_event esp_http_server esp_netif esp_system
                             esp_timer esp_wifi json nvs_flash spi_flash
                    EMBED_TXTFILES "web/index.html")
