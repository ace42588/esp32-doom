idf_component_register(SRCS "main.c"
                     "delta_encoding.c"
                     "http_handlers.c"
                     "network_transmission.c"
                     "network_transmission_optimizations.c"
                     "websocket_server.c"
                    PRIV_REQUIRES esp_http_server esp_wifi nvs_flash spiffs esp_eth fatfs prboom prboom-esp32-compat esp_timer esp_psram
                    INCLUDE_DIRS include)

# Flash the WAD file to the wad partition
# The wad partition starts at 0x100000 (1MB offset) based on the partition table
add_custom_target(flash_wad ALL
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/newdoom1_silent.wad" "${CMAKE_BINARY_DIR}/wad_partition.bin"
    COMMAND ${CMAKE_COMMAND} -E echo "WAD file copied to build directory"
    DEPENDS "${CMAKE_SOURCE_DIR}/newdoom1_silent.wad"
    COMMENT "Preparing WAD file for flashing"
)

# Flash the data directory to the storage partition using SPIFFS
spiffs_create_partition_image(storage ../data FLASH_IN_PROJECT)
