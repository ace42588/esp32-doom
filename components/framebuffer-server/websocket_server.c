#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <netinet/in.h>
#include <errno.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha1.h>
#include "websocket_server.h"
#include "frame_queue.h"
#include "ws_deflate.h"
#include "miniz.h"

#define TAG "ws_server"

// WebSocket magic string for handshake
const char *WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// Global frame queue - shared between components
frame_queue_t g_frame_queue;

// WebSocket server instance
static websocket_server_t g_websocket_server;

// Function to compute base64 SHA1 for WebSocket handshake
static void base64_sha1(const char *key, char *output, size_t output_len) {
    char combined[128];
    uint8_t sha1[20];
    snprintf(combined, sizeof(combined), "%s%s", key, WS_GUID);
    mbedtls_sha1((const unsigned char *)combined, strlen(combined), sha1);
    size_t olen;
    mbedtls_base64_encode((unsigned char *)output, output_len, &olen, sha1, 20);
}

// Parse WebSocket frame header
static int parse_ws_frame_header(const uint8_t *data, size_t len, 
                                uint8_t *opcode, uint8_t *masked, 
                                uint64_t *payload_len, uint8_t *mask) {
    if (len < 2) {
        ESP_LOGE(TAG, "Frame too short: %zu bytes", len);
        return -1;
    }
    
    *opcode = data[0] & 0x0F;
    *masked = (data[1] & 0x80) != 0;
    uint8_t payload_len_byte = data[1] & 0x7F;
    
    ESP_LOGI(TAG, "Frame: opcode=%d, masked=%d, payload_len_byte=%d", *opcode, *masked, payload_len_byte);
    
    if (payload_len_byte < 126) {
        *payload_len = payload_len_byte;
        if (*masked && len < 6) return -1;
        if (*masked) {
            memcpy(mask, data + 2, 4);
        }
    } else if (payload_len_byte == 126) {
        if (len < 4) return -1;
        *payload_len = (data[2] << 8) | data[3];
        if (*masked && len < 8) return -1;
        if (*masked) {
            memcpy(mask, data + 4, 4);
        }
    } else {
        if (len < 10) return -1;
        *payload_len = 0;
        for (int i = 0; i < 8; i++) {
            *payload_len = (*payload_len << 8) | data[2 + i];
        }
        if (*masked && len < 14) return -1;
        if (*masked) {
            memcpy(mask, data + 10, 4);
        }
    }
    
    return 0;
}

// Non-blocking send with timeout
static int nonblocking_send(int sockfd, const void *buf, size_t len, int timeout_ms) {
    size_t total_sent = 0;
    TickType_t start_time = xTaskGetTickCount();
    
    while (total_sent < len) {
        ssize_t sent = send(sockfd, (const char*)buf + total_sent, len - total_sent, MSG_DONTWAIT);
        
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Socket buffer full, check timeout
                if ((xTaskGetTickCount() - start_time) * portTICK_PERIOD_MS > timeout_ms) {
                    ESP_LOGE(TAG, "Send timeout after %d ms", timeout_ms);
                    return -1;
                }
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            } else {
                ESP_LOGE(TAG, "Send error: errno=%d", errno);
                return -1;
            }
        } else if (sent == 0) {
            // Connection closed
            ESP_LOGE(TAG, "Connection closed during send");
            return -1;
        }
        
        total_sent += sent;
    }
    
    return total_sent;
}

// Non-blocking recv with timeout
static int nonblocking_recv(int sockfd, void *buf, size_t len, int timeout_ms) {
    TickType_t start_time = xTaskGetTickCount();
    
    while (1) {
        ssize_t received = recv(sockfd, buf, len, MSG_DONTWAIT);
        
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // No data available, check timeout
                if ((xTaskGetTickCount() - start_time) * portTICK_PERIOD_MS > timeout_ms) {
                    return 0; // Timeout, no data available
                }
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            } else {
                ESP_LOGE(TAG, "Recv error: errno=%d", errno);
                return -1;
            }
        } else if (received == 0) {
            // Connection closed
            ESP_LOGI(TAG, "Connection closed by peer");
            return -1;
        }
        
        return received;
    }
}

// Handle WebSocket handshake with non-blocking operations
static int websocket_handshake(int client_fd) {
    // Use PSRAM for handshake buffer to save internal memory
    char *buffer = heap_caps_malloc(MAX_HEADER, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate handshake buffer in PSRAM, falling back to internal memory");
        buffer = heap_caps_malloc(MAX_HEADER, MALLOC_CAP_8BIT);
        if (buffer == NULL) {
            ESP_LOGE(TAG, "Failed to allocate handshake buffer");
            return -1;
        }
    }
    
    int len = nonblocking_recv(client_fd, buffer, MAX_HEADER - 1, 5000); // 5 second timeout
    if (len <= 0) {
        heap_caps_free(buffer);
        return -1;
    }

    buffer[len] = 0;
    
    ESP_LOGI(TAG, "Handshake request: %s", buffer);
    
    // Check if this is a WebSocket upgrade request
    if (strstr(buffer, "GET") == NULL || strstr(buffer, "Upgrade: websocket") == NULL) {
        ESP_LOGE(TAG, "Invalid handshake request");
        heap_caps_free(buffer);
        return -1;
    }
    
    const char *key_hdr = "Sec-WebSocket-Key: ";
    char *key_ptr = strstr(buffer, key_hdr);
    if (!key_ptr) {
        heap_caps_free(buffer);
        return -1;
    }

    key_ptr += strlen(key_hdr);
    char *eol = strstr(key_ptr, "\r\n");
    if (!eol) {
        heap_caps_free(buffer);
        return -1;
    }

    char client_key[128] = {0};
    strncpy(client_key, key_ptr, eol - key_ptr);

    char accept_key[128];
    base64_sha1(client_key, accept_key, sizeof(accept_key));

    // Parse extensions for permessage-deflate support
    char extensions_response[256] = "";
    const char *extensions_hdr = "Sec-WebSocket-Extensions: ";
    char *extensions_ptr = strstr(buffer, extensions_hdr);
    if (extensions_ptr) {
        extensions_ptr += strlen(extensions_hdr);
        char *eol = strstr(extensions_ptr, "\r\n");
        if (eol) {
            char extensions[256];
            strncpy(extensions, extensions_ptr, eol - extensions_ptr);
            extensions[eol - extensions_ptr] = 0;
            
#if WS_ENABLE_PERMESSAGE_DEFLATE
            if (websocket_parse_deflate_extension(extensions, extensions_response, sizeof(extensions_response)) == 0) {
                ESP_LOGI(TAG, "Permessage-deflate extension negotiated");
            }
#endif
        }
    }
    
    char response[512];
    if (strlen(extensions_response) > 0) {
        snprintf(response, sizeof(response),
                 "HTTP/1.1 101 Switching Protocols\r\n"
                 "Upgrade: websocket\r\n"
                 "Connection: Upgrade\r\n"
                 "Sec-WebSocket-Accept: %s\r\n"
                 "Sec-WebSocket-Extensions: %s\r\n\r\n", 
                 accept_key, extensions_response);
    } else {
        snprintf(response, sizeof(response),
                 "HTTP/1.1 101 Switching Protocols\r\n"
                 "Upgrade: websocket\r\n"
                 "Connection: Upgrade\r\n"
                 "Sec-WebSocket-Accept: %s\r\n\r\n", accept_key);
    }

    heap_caps_free(buffer);
    return nonblocking_send(client_fd, response, strlen(response), 5000);
}

// Send WebSocket binary frame with non-blocking operations
int websocket_send_binary_frame(int client_fd, const uint8_t *data, size_t len) {
    // Find the client structure
    websocket_client_t *client = NULL;
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (g_websocket_server.clients[i].fd == client_fd) {
            client = &g_websocket_server.clients[i];
            break;
        }
    }
    
    if (!client) {
        ESP_LOGE(TAG, "Client not found for fd %d", client_fd);
        return -1;
    }
    
    const uint8_t *frame_data = data;
    size_t frame_len = len;
    uint8_t *compressed_data = NULL;
    
#if WS_ENABLE_PERMESSAGE_DEFLATE
    // Compress data if compression is enabled
    if (client->compression_enabled) {
        compressed_data = heap_caps_malloc(WS_DEFLATE_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (compressed_data) {
            size_t compressed_len = WS_DEFLATE_BUFFER_SIZE;
            if (websocket_compress_frame(client, data, len, compressed_data, &compressed_len) == 0) {
                // Use compressed data if it's smaller
                if (compressed_len < len) {
                    frame_data = compressed_data;
                    frame_len = compressed_len;
                    ESP_LOGI(TAG, "Using compressed frame: %zu -> %zu bytes", len, compressed_len);
                } else {
                    ESP_LOGI(TAG, "Compression not beneficial, using original frame");
                    heap_caps_free(compressed_data);
                    compressed_data = NULL;
                }
            } else {
                ESP_LOGW(TAG, "Compression failed, using original frame");
                heap_caps_free(compressed_data);
                compressed_data = NULL;
            }
        }
    }
#endif
    
    const size_t max_chunk_size = 16384; // 16KB per fragment
    size_t offset = 0;
    int fragment_count = 0;
    
    //ESP_LOGI(TAG, "Sending WebSocket binary frame in fragments: total_size=%zu", len);
    
    while (offset < frame_len) {
        size_t chunk_size = (frame_len - offset < max_chunk_size) ? (frame_len - offset) : max_chunk_size;
        bool is_last = (offset + chunk_size >= frame_len);
        
        uint8_t header[10];
        size_t header_len = 0;
        
        // Set FIN bit only on last fragment, opcode only on first fragment
        // Also set RSV1 bit (0x40) if this is compressed data
        uint8_t rsv1_bit = (compressed_data && fragment_count == 0) ? 0x40 : 0x00;
        if (fragment_count == 0) {
            header[0] = (is_last ? 0x82 : 0x02) | rsv1_bit; // First fragment: binary frame, FIN only if last
        } else {
            header[0] = is_last ? 0x80 : 0x00; // Continuation frame: no opcode, FIN only if last
        }
        
        if (chunk_size <= 125) {
            header[1] = chunk_size;
            header_len = 2;
        } else if (chunk_size <= 65535) {
            header[1] = 126;
            header[2] = (chunk_size >> 8) & 0xff;
            header[3] = chunk_size & 0xff;
            header_len = 4;
        } else {
            header[1] = 127;
            for (int i = 0; i < 8; i++) {
                header[2 + i] = (chunk_size >> ((7 - i) * 8)) & 0xff;
            }
            header_len = 10;
        }
        
        //ESP_LOGI(TAG, "Sending fragment %d: offset=%zu, size=%zu, is_last=%d", 
        //         fragment_count, offset, chunk_size, is_last);
        
        // Send header with non-blocking operation
        if (nonblocking_send(client_fd, header, header_len, 1000) < 0) {
            ESP_LOGE(TAG, "Failed to send WebSocket fragment header");
            if (compressed_data) {
                heap_caps_free(compressed_data);
            }
            return -1;
        }
        
        // Send fragment data with non-blocking operation
        if (nonblocking_send(client_fd, frame_data + offset, chunk_size, 1000) < 0) {
            ESP_LOGE(TAG, "Failed to send WebSocket fragment data");
            if (compressed_data) {
                heap_caps_free(compressed_data);
            }
            return -1;
        }
        
        offset += chunk_size;
        fragment_count++;
        
        // Small delay between chunks to allow lwIP to process
        if (offset < frame_len) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
    
    // Clean up compressed data
    if (compressed_data) {
        heap_caps_free(compressed_data);
    }
    
    //ESP_LOGI(TAG, "WebSocket fragmentation complete: %d fragments sent, total=%zu bytes", 
    //         fragment_count, len);
    return 0;
}

// Send WebSocket text frame with non-blocking operations
int websocket_send_text_frame(int client_fd, const char *text) {
    size_t len = strlen(text);
    uint8_t header[10];
    size_t header_len = 0;

    header[0] = 0x81; // FIN + text frame
    if (len <= 125) {
        header[1] = len;
        header_len = 2;
    } else if (len <= 65535) {
        header[1] = 126;
        header[2] = (len >> 8) & 0xff;
        header[3] = len & 0xff;
        header_len = 4;
    } else {
        header[1] = 127;
        for (int i = 0; i < 8; i++) {
            header[2 + i] = (len >> ((7 - i) * 8)) & 0xff;
        }
        header_len = 10;
    }

    //ESP_LOGI(TAG, "Sending WebSocket text frame: size=%zu", len);
    
    if (nonblocking_send(client_fd, header, header_len, 1000) < 0) {
        ESP_LOGE(TAG, "Failed to send text frame header");
        return -1;
    }
    
    if (nonblocking_send(client_fd, text, len, 1000) < 0) {
        ESP_LOGE(TAG, "Failed to send text frame data");
        return -1;
    }
    
    //ESP_LOGI(TAG, "WebSocket text frame sent successfully: %zu bytes", len);
    return 0;
}

// Send WebSocket ping frame with non-blocking operations
int websocket_send_ping(int client_fd) {
    uint8_t header[2] = {0x89, 0x00}; // FIN + ping frame, no payload
    if (nonblocking_send(client_fd, header, 2, 1000) < 0) {
        ESP_LOGE(TAG, "Failed to send ping frame");
        return -1;
    }
    ESP_LOGI(TAG, "WebSocket ping frame sent successfully");
    return 0;
}

// Send WebSocket close frame with non-blocking operations
int websocket_send_close(int client_fd, uint16_t code) {
    uint8_t header[4] = {0x88, 0x02}; // FIN + close frame, 2-byte payload
    header[2] = (code >> 8) & 0xff;
    header[3] = code & 0xff;
    if (nonblocking_send(client_fd, header, 4, 1000) < 0) {
        ESP_LOGE(TAG, "Failed to send close frame");
        return -1;
    }
    ESP_LOGI(TAG, "WebSocket close frame sent successfully with code: %d", code);
    return 0;
}

#if WS_ENABLE_PERMESSAGE_DEFLATE

// Parse permessage-deflate extension in WebSocket handshake
int websocket_parse_deflate_extension(const char *extensions, char *response, size_t response_len) {
    if (!extensions || !response) {
        return -1;
    }
    
    // Look for permessage-deflate extension
    if (strstr(extensions, "permessage-deflate") != NULL) {
        ESP_LOGI(TAG, "Client supports permessage-deflate extension");
        
        // Parse parameters
        const char *server_no_context_takeover = strstr(extensions, "server_no_context_takeover");
        const char *client_no_context_takeover = strstr(extensions, "client_no_context_takeover");
        (void)strstr(extensions, "server_max_window_bits"); // Suppress unused variable warning
        (void)strstr(extensions, "client_max_window_bits"); // Suppress unused variable warning
        
        // Build response
        snprintf(response, response_len, "permessage-deflate");
        
        if (server_no_context_takeover) {
            strncat(response, "; server_no_context_takeover", response_len - strlen(response) - 1);
        }
        
        if (client_no_context_takeover) {
            strncat(response, "; client_no_context_takeover", response_len - strlen(response) - 1);
        }
        
        ESP_LOGI(TAG, "Permessage-deflate response: %s", response);
        return 0;
    }
    
    return -1;
}

// Custom allocation functions for miniz
static void *miniz_alloc_func(void *opaque, size_t items, size_t size) {
    (void)opaque;
    return heap_caps_malloc(items * size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void miniz_free_func(void *opaque, void *address) {
    (void)opaque;
    heap_caps_free(address);
}

// Initialize compression for a client
int websocket_init_compression(websocket_client_t *client) {
    if (!client) {
        return -1;
    }
    
    // Allocate compression buffers in PSRAM
    client->deflate_buffer_size = WS_DEFLATE_BUFFER_SIZE;
    client->deflate_buffer = heap_caps_malloc(client->deflate_buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!client->deflate_buffer) {
        ESP_LOGE(TAG, "Failed to allocate deflate buffer");
        return -1;
    }
    
    client->inflate_buffer_size = WS_DEFLATE_BUFFER_SIZE;
    client->inflate_buffer = heap_caps_malloc(client->inflate_buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!client->inflate_buffer) {
        ESP_LOGE(TAG, "Failed to allocate inflate buffer");
        heap_caps_free(client->deflate_buffer);
        client->deflate_buffer = NULL;
        return -1;
    }
    
    // Allocate stream contexts in PSRAM
    client->deflate_stream = heap_caps_malloc(sizeof(mz_stream), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!client->deflate_stream) {
        ESP_LOGE(TAG, "Failed to allocate deflate stream");
        heap_caps_free(client->deflate_buffer);
        heap_caps_free(client->inflate_buffer);
        client->deflate_buffer = NULL;
        client->inflate_buffer = NULL;
        return -1;
    }
    
    client->inflate_stream = heap_caps_malloc(sizeof(mz_stream), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!client->inflate_stream) {
        ESP_LOGE(TAG, "Failed to allocate inflate stream");
        heap_caps_free(client->deflate_buffer);
        heap_caps_free(client->inflate_buffer);
        heap_caps_free(client->deflate_stream);
        client->deflate_buffer = NULL;
        client->inflate_buffer = NULL;
        client->deflate_stream = NULL;
        return -1;
    }
    
    // Initialize stream contexts
    memset(client->deflate_stream, 0, sizeof(mz_stream));
    memset(client->inflate_stream, 0, sizeof(mz_stream));
    
    // Set custom allocation functions
    client->deflate_stream->zalloc = miniz_alloc_func;
    client->deflate_stream->zfree = miniz_free_func;
    client->deflate_stream->opaque = NULL;
    
    client->inflate_stream->zalloc = miniz_alloc_func;
    client->inflate_stream->zfree = miniz_free_func;
    client->inflate_stream->opaque = NULL;
    
    int deflate_result = mz_deflateInit2(client->deflate_stream, 1, MZ_DEFLATED, -15, 8, MZ_DEFAULT_STRATEGY);
    if (deflate_result != MZ_OK) {
        ESP_LOGE(TAG, "Failed to initialize deflate stream: %d", deflate_result);
        websocket_cleanup_compression(client);
        return -1;
    }
    
    int inflate_result = mz_inflateInit2(client->inflate_stream, -15);
    if (inflate_result != MZ_OK) {
        ESP_LOGE(TAG, "Failed to initialize inflate stream: %d", inflate_result);
        websocket_cleanup_compression(client);
        return -1;
    }
    
    client->compression_enabled = 1;
    ESP_LOGI(TAG, "Compression initialized for client");
    return 0;
}

// Cleanup compression for a client
void websocket_cleanup_compression(websocket_client_t *client) {
    if (!client) {
        return;
    }
    
    if (client->deflate_stream) {
        mz_deflateEnd(client->deflate_stream);
        heap_caps_free(client->deflate_stream);
        client->deflate_stream = NULL;
    }
    
    if (client->inflate_stream) {
        mz_inflateEnd(client->inflate_stream);
        heap_caps_free(client->inflate_stream);
        client->inflate_stream = NULL;
    }
    
    if (client->deflate_buffer) {
        heap_caps_free(client->deflate_buffer);
        client->deflate_buffer = NULL;
    }
    
    if (client->inflate_buffer) {
        heap_caps_free(client->inflate_buffer);
        client->inflate_buffer = NULL;
    }
    
    client->compression_enabled = 0;
    client->deflate_buffer_size = 0;
    client->inflate_buffer_size = 0;
    
    ESP_LOGI(TAG, "Compression cleaned up for client");
}

// Compress a frame using deflate
int websocket_compress_frame(websocket_client_t *client, const uint8_t *input, size_t input_len, 
                           uint8_t *output, size_t *output_len) {
    if (!client || !client->compression_enabled || !client->deflate_buffer || !client->deflate_stream) {
        return -1;
    }
    
    // Use the ws_deflate implementation which properly handles RFC 7692
    size_t compressed_len = *output_len;
    int result = ws_deflate_compress(input, input_len, client->deflate_buffer, &compressed_len, client->deflate_stream);
    
    if (result == 0) {
        // Only use compression if it actually reduces the size
        if (compressed_len < input_len) {
            memcpy(output, client->deflate_buffer, compressed_len);
            *output_len = compressed_len;
            ESP_LOGI(TAG, "Compressed frame: %zu -> %zu bytes (RFC 7692 compliant)", input_len, *output_len);
            return 0;
        } else {
            ESP_LOGW(TAG, "Compression not beneficial: %zu -> %zu bytes, using original", input_len, compressed_len);
            return -1; // Signal to use original data
        }
    } else {
        ESP_LOGE(TAG, "Deflate compression failed: %d", result);
        return -1;
    }
}

// Decompress a frame using inflate
int websocket_decompress_frame(websocket_client_t *client, const uint8_t *input, size_t input_len, 
                             uint8_t *output, size_t *output_len) {
    if (!client || !client->compression_enabled || !client->inflate_buffer || !client->inflate_stream) {
        return -1;
    }
    
    // Use the ws_deflate implementation which properly handles RFC 7692
    size_t decompressed_len = *output_len;
    int result = ws_deflate_decompress(input, input_len, client->inflate_buffer, &decompressed_len, client->inflate_stream);
    
    if (result == 0) {
        memcpy(output, client->inflate_buffer, decompressed_len);
        *output_len = decompressed_len;
        ESP_LOGI(TAG, "Decompressed frame: %zu -> %zu bytes (RFC 7692 compliant)", input_len, *output_len);
        return 0;
    } else {
        ESP_LOGE(TAG, "Inflate decompression failed: %d", result);
        return -1;
    }
}

#endif

// Handle incoming WebSocket frames with non-blocking operations
static int handle_ws_frame(int client_fd) {
    // Use PSRAM for frame buffer to save internal memory
    uint8_t *buffer = heap_caps_malloc(WS_FRAME_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate frame buffer in PSRAM, falling back to internal memory");
        buffer = heap_caps_malloc(WS_FRAME_BUFFER_SIZE, MALLOC_CAP_8BIT);
        if (buffer == NULL) {
            ESP_LOGE(TAG, "Failed to allocate frame buffer");
            return -1;
        }
    }
    
    int len = nonblocking_recv(client_fd, buffer, WS_FRAME_BUFFER_SIZE, 100); // 100ms timeout
    
    if (len <= 0) {
        heap_caps_free(buffer);
        if (len < 0) {
            ESP_LOGI(TAG, "Connection error: errno=%d", errno);
            return -1; // Connection error
        }
        return 0; // No data available (normal for non-blocking socket)
    }
    
    uint8_t opcode, masked;
    uint64_t payload_len;
    uint8_t mask[4];
    
    if (parse_ws_frame_header(buffer, len, &opcode, &masked, &payload_len, mask) < 0) {
        ESP_LOGE(TAG, "Failed to parse WebSocket frame header");
        heap_caps_free(buffer);
        return -1;
    }
    
    switch (opcode) {
        case WS_FRAME_PING:
            websocket_send_ping(client_fd);
            break;
        case WS_FRAME_CLOSE:
            websocket_send_close(client_fd, 1000);
            heap_caps_free(buffer);
            return -1;
        case WS_FRAME_TEXT:
        case WS_FRAME_BINARY:
            // Handle payload if needed
            break;
    }
    
    heap_caps_free(buffer);
    return 0;
}

// Initialize WebSocket server
void websocket_server_init(websocket_server_t *server) {
    memset(server, 0, sizeof(*server));
    server->server_fd = -1;
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        server->clients[i].fd = -1;
        server->clients[i].active = 0;
        server->clients[i].compression_enabled = 0;
        server->clients[i].deflate_buffer = NULL;
        server->clients[i].inflate_buffer = NULL;
        server->clients[i].deflate_buffer_size = 0;
        server->clients[i].inflate_buffer_size = 0;
    }
}

// Start WebSocket server
void websocket_server_start(websocket_server_t *server) {
    server->server_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (server->server_fd < 0) {
        ESP_LOGE(TAG, "Socket creation failed");
        return;
    }

    int opt = 1;
    setsockopt(server->server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(server->server_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(WS_PORT),
    };

    if (bind(server->server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "Bind failed: errno=%d", errno);
        close(server->server_fd);
        server->server_fd = -1;
        return;
    }

    if (listen(server->server_fd, WS_MAX_CLIENTS) < 0) {
        ESP_LOGE(TAG, "Listen failed: errno=%d", errno);
        close(server->server_fd);
        server->server_fd = -1;
        return;
    }

    // Set non-blocking mode
    fcntl(server->server_fd, F_SETFL, O_NONBLOCK);
    
    server->active = 1;
    ESP_LOGI(TAG, "WebSocket server started on port %d", WS_PORT);
}

// Stop WebSocket server
void websocket_server_stop(websocket_server_t *server) {
    server->active = 0;
    
    // Close all client connections
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (server->clients[i].fd >= 0) {
#if WS_ENABLE_PERMESSAGE_DEFLATE
            // Cleanup compression
            websocket_cleanup_compression(&server->clients[i]);
#endif
            close(server->clients[i].fd);
            server->clients[i].fd = -1;
            server->clients[i].active = 0;
        }
    }
    server->client_count = 0;
    
    // Close server socket
    if (server->server_fd >= 0) {
        close(server->server_fd);
        server->server_fd = -1;
    }
    
    ESP_LOGI(TAG, "WebSocket server stopped");
}

// Main WebSocket server task with improved non-blocking behavior
void websocket_server_task(void *pv) {
    ESP_LOGI(TAG, "WebSocket server task starting...");
    websocket_server_t *server = &g_websocket_server;
    
    // Initialize frame queue first
    frame_queue_init(&g_frame_queue);
    ESP_LOGI(TAG, "Frame queue initialized");
    
    websocket_server_init(server);
    websocket_server_start(server);
    
    if (server->server_fd < 0) {
        ESP_LOGE(TAG, "Failed to start WebSocket server");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "WebSocket server task started");

    while (server->active) {
        // Accept new connections with non-blocking accept
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(server->server_fd, (struct sockaddr *)&client_addr, &addr_len);
        
        if (client_fd >= 0) {
            if (server->client_count >= WS_MAX_CLIENTS) {
                ESP_LOGW(TAG, "Max clients reached, rejecting connection");
                close(client_fd);
                continue;
            }
            
            ESP_LOGI(TAG, "New client connected");
            fcntl(client_fd, F_SETFL, O_NONBLOCK);

            if (websocket_handshake(client_fd) < 0) {
                ESP_LOGW(TAG, "WebSocket handshake failed");
                close(client_fd);
                continue;
            }

            // Add client to list
            for (int i = 0; i < WS_MAX_CLIENTS; i++) {
                if (server->clients[i].fd == -1) {
                    server->clients[i].fd = client_fd;
                    server->clients[i].active = 1;
                    server->client_count++;
                    
#if WS_ENABLE_PERMESSAGE_DEFLATE
                    // Initialize compression if supported
                    if (websocket_init_compression(&server->clients[i]) == 0) {
                        ESP_LOGI(TAG, "Compression initialized for client %d", i);
                    } else {
                        ESP_LOGW(TAG, "Failed to initialize compression for client %d", i);
                    }
#endif
                    
                    break;
                }
            }
            
            ESP_LOGI(TAG, "WebSocket handshake complete, client count: %d", server->client_count);
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            ESP_LOGE(TAG, "Accept error: errno=%d", errno);
        }

        // Handle existing client connections
        for (int i = 0; i < WS_MAX_CLIENTS; i++) {
            if (server->clients[i].fd >= 0 && server->clients[i].active) {
                // Handle incoming frames
                int result = handle_ws_frame(server->clients[i].fd);
                if (result < 0) {
                    ESP_LOGI(TAG, "Client disconnected (frame handling failed)");
                    
#if WS_ENABLE_PERMESSAGE_DEFLATE
                    // Cleanup compression
                    websocket_cleanup_compression(&server->clients[i]);
#endif
                    
                    close(server->clients[i].fd);
                    server->clients[i].fd = -1;
                    server->clients[i].active = 0;
                    server->client_count--;
                    continue;
                } else if (result > 0) {
                    ESP_LOGI(TAG, "Frame handled successfully");
                }
            }
        }

        // Send frame data to all connected clients (only if we have frames and clients)
        uint8_t *frame = frame_queue_get_next_frame(&g_frame_queue);
        if (frame && server->client_count > 0) {
            //ESP_LOGI(TAG, "Sending frame of size %zu bytes to %d clients", (size_t)(FRAME_SIZE + 1), server->client_count);
            for (int i = 0; i < WS_MAX_CLIENTS; i++) {
                if (server->clients[i].fd >= 0 && server->clients[i].active) {
                    //ESP_LOGI(TAG, "Sending frame to client %d, palette index: %d", i, frame[0]);
                    if (websocket_send_binary_frame(server->clients[i].fd, frame, FRAME_SIZE + 1) < 0) {
                        ESP_LOGW(TAG, "Failed to send frame to client %d", i);
                        
#if WS_ENABLE_PERMESSAGE_DEFLATE
                        // Cleanup compression
                        websocket_cleanup_compression(&server->clients[i]);
#endif
                        
                        close(server->clients[i].fd);
                        server->clients[i].fd = -1;
                        server->clients[i].active = 0;
                        server->client_count--;
                    } else {
                        //ESP_LOGI(TAG, "Frame sent successfully to client %d", i);
                    }
                }
            }
            frame_queue_release_frame(&g_frame_queue);
            
            // Small delay between frames to allow client to process
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        // Send ping to keep connection alive
        static int ping_counter = 0;
        ping_counter++;
        if (ping_counter >= 5000) { // Send ping every ~5 seconds
            for (int i = 0; i < WS_MAX_CLIENTS; i++) {
                if (server->clients[i].fd >= 0 && server->clients[i].active) {
                    websocket_send_ping(server->clients[i].fd);
                }
            }
            ping_counter = 0;
        }
        
        // Adaptive delay based on activity
        if (server->client_count == 0 && !frame) {
            // No clients and no frames, longer delay
            vTaskDelay(pdMS_TO_TICKS(10));
        } else {
            // Active, shorter delay
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    websocket_server_stop(server);
    vTaskDelete(NULL);
}
