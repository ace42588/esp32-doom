#ifndef WEBSOCKET_SERVER_H
#define WEBSOCKET_SERVER_H

#include "esp_http_server.h"
#include "esp_err.h"
#include "esp_event.h"

// WebSocket server configuration
#define MAX_WS_CLIENTS 1
#define INDEX_HTML_PATH "/spiffs/index.html"
#define DOOM_PALETTE_JS_PATH "/spiffs/doom-palette.js"

#define FRAGMENT_SIZE 16384

// WebSocket server state
typedef struct {
    httpd_handle_t server_handle;
    int client_fds[MAX_WS_CLIENTS];
    int client_count;
    bool is_initialized;
} websocket_server_t;

// Function declarations for WebSocket server management
esp_err_t websocket_server_init(void);
esp_err_t websocket_server_start(void);
esp_err_t websocket_server_stop(void);
void websocket_server_cleanup(void);

// Client management functions
esp_err_t websocket_add_client(int fd);
esp_err_t websocket_remove_client(int fd);
int websocket_get_client_count(void);
int websocket_get_client_fd(int index);
bool websocket_is_client_valid(int client_index);

// WebSocket frame handling
esp_err_t websocket_send_binary_frame(int client_fd, const uint8_t *data, size_t len);
esp_err_t websocket_send_fragmented_frame(int client_fd, const uint8_t *data, size_t len, uint8_t palette_index);

// Network event handlers
void websocket_connect_handler(void* arg, esp_event_base_t event_base,
                             int32_t event_id, void* event_data);
void websocket_disconnect_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data);

// HTTP request handlers
esp_err_t websocket_index_handler(httpd_req_t *req);
esp_err_t websocket_palette_handler(httpd_req_t *req);
esp_err_t websocket_ws_handler(httpd_req_t *req);

// Utility functions
bool websocket_server_is_ready(void);
httpd_handle_t websocket_get_server_handle(void);

#endif // WEBSOCKET_SERVER_H 