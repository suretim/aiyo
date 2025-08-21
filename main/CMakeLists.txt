idf_component_register(SRCS http_web.c tflm.cc
                        PRIV_INCLUDE_DIRS .
                        PRIV_REQUIRES nvs_flash esp_psram esp_http_server protocol_examples_common console esp_netif esp_http_server esp_wifi esp32-camera esp-nn esp-tflite-micro esp_timer json lwip esp_eth driver
                        EMBED_FILES "web_content/index_http.html" "web_content/index_socket.html")
