#ifndef NETWORK_TRANSMISSION_H
#define NETWORK_TRANSMISSION_H

#include "esp_http_server.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

// Network transmission configuration
#define MAX_FRAME_SKIP_RATIO 3
#define WEBSOCKET_TIMEOUT_MS 200
#define QUEUE_SIZE 16

// Network message structure
typedef struct {
    uint8_t *data;
    size_t len;
    uint8_t palette_index;
    bool is_delta;
} network_message_t;

// Function declarations
esp_err_t init_async_network_transmission(void);
void cleanup_async_network_transmission(void);
bool is_network_transmission_initialized(void);
BaseType_t queue_network_message(network_message_t *msg);
void network_transmission_task(void *pvParameters);
esp_err_t send_optimized_websocket_frame(httpd_handle_t server_handle, int client_fd, httpd_ws_frame_t *ws_pkt);
esp_err_t send_zero_copy_websocket_frame(httpd_handle_t server_handle, int client_fd, 
                                        const uint8_t *data, size_t total_len, uint8_t palette_index);
esp_err_t send_fragmented_websocket_frame(httpd_handle_t server_handle, int client_fd, 
                                         const uint8_t *data, size_t total_len, uint8_t palette_index);

#endif // NETWORK_TRANSMISSION_H 