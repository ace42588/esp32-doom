idf_component_register(SRCS frame_queue.c websocket_server.c
                       INCLUDE_DIRS include
                       PRIV_REQUIRES esp_netif esp_wifi mbedtls lwip)