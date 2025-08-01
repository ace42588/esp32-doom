idf_component_register(SRCS i_main.c i_network.c i_sound.c i_system.c i_video.c gamepad.c instrumentation_stubs.c
                       INCLUDE_DIRS include
                       REQUIRES esp_driver_i2s spiffs prboom main
                       PRIV_REQUIRES esp_timer framebuffer-server)