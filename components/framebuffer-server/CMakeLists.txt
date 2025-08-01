idf_component_register(SRCS frame_queue.c websocket_server.c ws_deflate.c input_handler.c instrumentation_stubs.c
                       INCLUDE_DIRS include
                       PRIV_REQUIRES esp_netif esp_wifi mbedtls lwip esp_full_miniz)