#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// WebSocket server configuration
#define WS_PORT 8080
#define MAX_HEADER 2048
#define WS_MAX_CLIENTS 4
#define WS_FRAME_BUFFER_SIZE 4096

// WebSocket frame types
#define WS_FRAME_CONTINUATION 0x0
#define WS_FRAME_TEXT         0x1
#define WS_FRAME_BINARY       0x2
#define WS_FRAME_CLOSE        0x8
#define WS_FRAME_PING         0x9
#define WS_FRAME_PONG         0xA

// WebSocket server state
typedef struct {
    int server_fd;
    int client_fds[WS_MAX_CLIENTS];
    int client_count;
    int active;
} websocket_server_t;

// Function declarations
void websocket_server_init(websocket_server_t *server);
void websocket_server_start(websocket_server_t *server);
void websocket_server_stop(websocket_server_t *server);
void websocket_server_task(void *pv);
int websocket_send_binary_frame(int client_fd, const uint8_t *data, size_t len);
int websocket_send_text_frame(int client_fd, const char *text);
int websocket_send_ping(int client_fd);
int websocket_send_close(int client_fd, uint16_t code);

#ifdef __cplusplus
}
#endif
