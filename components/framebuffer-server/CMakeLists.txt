idf_component_register(SRCS frame_queue.c websocket_server.c miniz.c ws_deflate.c
                       INCLUDE_DIRS include
                       PRIV_REQUIRES esp_netif esp_wifi mbedtls lwip)

# Add conditional compilation for permessage-deflate support
target_compile_definitions(${COMPONENT_LIB} PRIVATE WS_ENABLE_PERMESSAGE_DEFLATE=1)