#ifndef WEBSOCKET_SERVER_H
#define WEBSOCKET_SERVER_H

#include "esp_http_server.h"
#include "esp_err.h"
#include "esp_event.h"

// WebSocket server configuration
#define MAX_WS_CLIENTS 1
#define ENABLE_DELTA_ENCODING 0  // Disabled for FPS performance

// Global server state (extern declarations)
extern httpd_handle_t g_server_handle;
extern int ws_client_fds[MAX_WS_CLIENTS];
extern int ws_client_count;

// Function declarations
void ws_add_client(int fd);
void ws_remove_client(int fd);
bool ws_is_client_valid(int client_index);
void ws_cleanup_invalid_clients(void);
void ws_validate_client_array(void);
bool ws_check_client_alive(int client_fd);
void ws_cleanup_stale_clients(void);
void ws_broadcast_framebuffer(const void *data, size_t len, uint8_t palette_index);
esp_err_t handle_ws_req(httpd_req_t *req);
httpd_handle_t setup_websocket_server(void);
esp_err_t stop_webserver(httpd_handle_t server);
void cleanup_websocket_resources(void);
void init_web_page_buffer(void);

// Network event handlers
void connect_handler(void* arg, esp_event_base_t event_base,
                    int32_t event_id, void* event_data);
void disconnect_handler(void* arg, esp_event_base_t event_base,
                      int32_t event_id, void* event_data);

#endif // WEBSOCKET_SERVER_H 