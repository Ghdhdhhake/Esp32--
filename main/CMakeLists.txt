idf_component_register(SRCS "main.c" "ble_scanner.c" "led_control.c"
                            "gpio_panel.c" "log_ring.c"
                    INCLUDE_DIRS "."
                    REQUIRES bt esp_event esp_http_server esp_netif esp_system
                             esp_timer esp_wifi json nvs_flash spi_flash
                             esp_driver_gpio esp_driver_ledc esp_adc
                    EMBED_TXTFILES "web/index.html")
