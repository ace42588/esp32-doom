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

// Enable permessage-deflate support (optional)
#define WS_ENABLE_PERMESSAGE_DEFLATE 1

// WebSocket frame types
#define WS_FRAME_CONTINUATION 0x0
#define WS_FRAME_TEXT         0x1
#define WS_FRAME_BINARY       0x2
#define WS_FRAME_CLOSE        0x8
#define WS_FRAME_PING         0x9
#define WS_FRAME_PONG         0xA

// Permessage-deflate configuration
#define WS_DEFLATE_WINDOW_BITS 15
#define WS_DEFLATE_MEM_LEVEL 8
#define WS_DEFLATE_STRATEGY 0
#define WS_DEFLATE_BUFFER_SIZE 32768

// Forward declaration for mz_stream
struct mz_stream_s;
typedef struct mz_stream_s mz_stream;

// WebSocket client state with compression support
typedef struct {
    int fd;
    int active;
    int compression_enabled;
    uint8_t *deflate_buffer;
    uint8_t *inflate_buffer;
    size_t deflate_buffer_size;
    size_t inflate_buffer_size;
    mz_stream *deflate_stream;
    mz_stream *inflate_stream;
} websocket_client_t;

// WebSocket server state
typedef struct {
    int server_fd;
    websocket_client_t clients[WS_MAX_CLIENTS];
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

// Permessage-deflate functions (only available if WS_ENABLE_PERMESSAGE_DEFLATE is defined)
#if WS_ENABLE_PERMESSAGE_DEFLATE
int websocket_parse_deflate_extension(const char *extensions, char *response, size_t response_len);
int websocket_init_compression(websocket_client_t *client);
void websocket_cleanup_compression(websocket_client_t *client);
int websocket_compress_frame(websocket_client_t *client, const uint8_t *input, size_t input_len, 
                           uint8_t *output, size_t *output_len);
int websocket_decompress_frame(websocket_client_t *client, const uint8_t *input, size_t input_len, 
                             uint8_t *output, size_t *output_len);
#endif

#ifdef __cplusplus
}
#endif
