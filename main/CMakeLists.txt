# 本组件同时编译固件逻辑，并把两个网页以只读文本资源嵌入最终固件。
idf_component_register(SRCS "main.c"
                            "gpio_panel.c" "log_ring.c"
                            "llm_client.c" "chat_ws.c" "rss_news.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_event esp_http_server esp_netif esp_system
                             esp_timer esp_wifi json nvs_flash spi_flash
                             esp_driver_gpio esp_adc
                             esp_http_client esp-tls mbedtls
                    EMBED_TXTFILES "web/index.html" "web/chat.html")
