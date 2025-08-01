#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <netinet/in.h>
#include <errno.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha1.h>
#include "websocket_server.h"
#include "frame_queue.h"
#include "ws_deflate.h"
#include "full_miniz.h"
#include "input_handler.h"
#include "instrumentation_interface.h"
#include "esp_timer.h"

#define TAG "ws_server"

// WebSocket profiling structures
typedef struct {
    uint32_t total_operations;
    uint32_t total_time_us;
    uint32_t min_time_us;
    uint32_t max_time_us;
    uint32_t avg_time_us;
    uint32_t last_operation_time;
} websocket_profile_stats_t;

// Global profiling stats
static websocket_profile_stats_t handshake_stats = {0};
static websocket_profile_stats_t compression_stats = {0};
static websocket_profile_stats_t frame_send_stats = {0};
static websocket_profile_stats_t frame_recv_stats = {0};
static websocket_profile_stats_t deflate_stats = {0};

// Profiling helper functions
static void update_profile_stats(websocket_profile_stats_t *stats, uint32_t operation_time_us) {
    stats->total_operations++;
    stats->total_time_us += operation_time_us;
    stats->last_operation_time = operation_time_us;
    
    if (operation_time_us < stats->min_time_us || stats->min_time_us == 0) {
        stats->min_time_us = operation_time_us;
    }
    if (operation_time_us > stats->max_time_us) {
        stats->max_time_us = operation_time_us;
    }
    
    stats->avg_time_us = stats->total_time_us / stats->total_operations;
}

static void log_profile_stats(const char *operation, websocket_profile_stats_t *stats) {
    if (stats->total_operations > 0) {
        ESP_LOGI(TAG, "WebSocket %s Profile: ops=%u, avg=%uus, min=%uus, max=%uus, total=%uus",
                 operation, stats->total_operations, stats->avg_time_us, 
                 stats->min_time_us, stats->max_time_us, stats->total_time_us);
    }
}

void log_all_websocket_profiles(void) {
    ESP_LOGI(TAG, "=== WEBSOCKET PROFILING REPORT ===");
    log_profile_stats("Handshake", &handshake_stats);
    log_profile_stats("Compression", &compression_stats);
    log_profile_stats("Frame Send", &frame_send_stats);
    log_profile_stats("Frame Receive", &frame_recv_stats);
    log_profile_stats("Deflate", &deflate_stats);
    ESP_LOGI(TAG, "=== END WEBSOCKET PROFILING ===");
}

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

// Non-blocking send with timeout using select()
static int nonblocking_send(int sockfd, const void *buf, size_t len, int timeout_ms) {
    size_t total_sent = 0;
    TickType_t start_time = xTaskGetTickCount();
    
    while (total_sent < len) {
        // Use select() to check if socket is writable
        fd_set write_fds;
        struct timeval timeout;
        
        FD_ZERO(&write_fds);
        FD_SET(sockfd, &write_fds);
        
        // Calculate remaining timeout
        uint32_t elapsed_ms = (xTaskGetTickCount() - start_time) * portTICK_PERIOD_MS;
        if (elapsed_ms >= timeout_ms) {
            ESP_LOGE(TAG, "Send timeout after %d ms", timeout_ms);
            return -1;
        }
        
        uint32_t remaining_ms = timeout_ms - elapsed_ms;
        timeout.tv_sec = remaining_ms / 1000;
        timeout.tv_usec = (remaining_ms % 1000) * 1000;
        
        int select_result = select(sockfd + 1, NULL, &write_fds, NULL, &timeout);
        
        if (select_result < 0) {
            ESP_LOGE(TAG, "Select error: errno=%d", errno);
            return -1;
        } else if (select_result == 0) {
            ESP_LOGE(TAG, "Send timeout after %d ms", timeout_ms);
            return -1;
        }
        
        // Socket is writable, try to send
        ssize_t sent = send(sockfd, (const char*)buf + total_sent, len - total_sent, MSG_DONTWAIT);
        
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // This shouldn't happen after select() says it's writable, but handle it
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
    
    // Track network throughput for successful sends
    if (total_sent > 0) {
        instrumentation_network_sent_bytes(total_sent);
        instrumentation_network_sent_packet();
    }
    
    return total_sent;
}

// Non-blocking recv with timeout using select()
static int nonblocking_recv(int sockfd, void *buf, size_t len, int timeout_ms) {
    // Use select() to check if socket has data to read
    fd_set read_fds;
    struct timeval timeout;
    
    FD_ZERO(&read_fds);
    FD_SET(sockfd, &read_fds);
    
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    
    int select_result = select(sockfd + 1, &read_fds, NULL, NULL, &timeout);
    
    if (select_result < 0) {
        ESP_LOGE(TAG, "Select error: errno=%d", errno);
        return -1;
    } else if (select_result == 0) {
        return 0; // Timeout, no data available
    }
    
    // Socket has data to read
    ssize_t received = recv(sockfd, buf, len, MSG_DONTWAIT);
    
    if (received < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // This shouldn't happen after select() says it's readable, but handle it
            return 0;
        } else {
            ESP_LOGE(TAG, "Recv error: errno=%d", errno);
            return -1;
        }
    } else if (received == 0) {
        // Connection closed
        ESP_LOGI(TAG, "Connection closed by peer");
        return -1;
    }
    
    // Track network throughput for successful receives
    if (received > 0) {
        instrumentation_network_received_bytes(received);
        instrumentation_network_received_packet();
    }
    
    return received;
}

// Handle WebSocket handshake with non-blocking operations
static int websocket_handshake(int client_fd) {
    uint64_t start_time = esp_timer_get_time();
    
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
    char *key_end = strstr(key_ptr, "\r\n");
    if (!key_end) {
        heap_caps_free(buffer);
        return -1;
    }
    *key_end = 0;
    
    char accept_key[64];
    base64_sha1(key_ptr, accept_key, sizeof(accept_key));
    
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
    
    int result = nonblocking_send(client_fd, response, strlen(response), 5000);
    
    // Update profiling stats
    uint64_t end_time = esp_timer_get_time();
    uint32_t operation_time_us = (uint32_t)(end_time - start_time);
    update_profile_stats(&handshake_stats, operation_time_us);
    
    return result;
}

// Send WebSocket binary frame with non-blocking operations
int websocket_send_binary_frame(int client_fd, const uint8_t *data, size_t len) {
    uint64_t start_time = esp_timer_get_time();
    
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
    
    // Track PSRAM read operation for frame data
    instrumentation_psram_read_operation(len);
    
    const size_t max_chunk_size = WS_MAX_FRAME_CHUNK_SIZE; // Smaller chunks for better reliability
    size_t offset = 0;
    int fragment_count = 0;
    
    //ESP_LOGI(TAG, "Sending WebSocket binary frame in fragments: total_size=%zu", len);
    
    while (offset < frame_len) {
        size_t chunk_size = (frame_len - offset < max_chunk_size) ? (frame_len - offset) : max_chunk_size;
        bool is_last = (offset + chunk_size >= frame_len);
        
        uint8_t header[10];
        size_t header_len = 0;
        
        // Set FIN bit only on last fragment, opcode only on first fragment
        if (fragment_count == 0) {
            header[0] = (is_last ? 0x82 : 0x02); // First fragment: binary frame, FIN only if last
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
        
        if (nonblocking_send(client_fd, header, header_len, WS_SEND_TIMEOUT_MS) < 0) {
            ESP_LOGE(TAG, "Failed to send frame header");
            return -1;
        }
        
        if (nonblocking_send(client_fd, frame_data + offset, chunk_size, WS_SEND_TIMEOUT_MS) < 0) {
            ESP_LOGE(TAG, "Failed to send frame data");
            return -1;
        }
        
        offset += chunk_size;
        fragment_count++;
    }
    
    // Update frame send profiling stats
    uint64_t end_time = esp_timer_get_time();
    uint32_t operation_time_us = (uint32_t)(end_time - start_time);
    update_profile_stats(&frame_send_stats, operation_time_us);
    
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
    
    if (nonblocking_send(client_fd, header, header_len, WS_SEND_TIMEOUT_MS) < 0) {
        ESP_LOGE(TAG, "Failed to send text frame header");
        return -1;
    }
    
    if (nonblocking_send(client_fd, text, len, WS_SEND_TIMEOUT_MS) < 0) {
        ESP_LOGE(TAG, "Failed to send text frame data");
        return -1;
    }
    
    //ESP_LOGI(TAG, "WebSocket text frame sent successfully: %zu bytes", len);
    return 0;
}

// Send WebSocket ping frame with non-blocking operations
int websocket_send_ping(int client_fd) {
    uint8_t header[2] = {0x89, 0x00}; // FIN + ping frame, no payload
    if (nonblocking_send(client_fd, header, 2, WS_SEND_TIMEOUT_MS) < 0) {
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
    void *ptr = heap_caps_malloc(items * size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr) {
        instrumentation_psram_write_operation(items * size);
    }
    return ptr;
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
    uint64_t start_time = esp_timer_get_time();
    
    if (!client || !client->compression_enabled || !client->deflate_buffer || !client->deflate_stream) {
        return -1;
    }
    
    // Track PSRAM read operation for input data
    instrumentation_psram_read_operation(input_len);
    
    // Use the ws_deflate implementation which properly handles RFC 7692
    size_t compressed_len = *output_len;
    int result = ws_deflate_compress(input, input_len, client->deflate_buffer, &compressed_len, client->deflate_stream);
    
    if (result == 0) {
        // Only use compression if it actually reduces the size
        if (compressed_len < input_len) {
            // Track PSRAM write operation for compressed data
            instrumentation_psram_write_operation(compressed_len);
            
            memcpy(output, client->deflate_buffer, compressed_len);
            *output_len = compressed_len;
            ESP_LOGI(TAG, "Compressed frame: %zu -> %zu bytes (RFC 7692 compliant)", input_len, *output_len);
            
            // Update deflate profiling stats
            uint64_t end_time = esp_timer_get_time();
            uint32_t operation_time_us = (uint32_t)(end_time - start_time);
            update_profile_stats(&deflate_stats, operation_time_us);
            
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
    uint64_t start_time = esp_timer_get_time();
    
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
    
    // Track PSRAM read operation for received data
    instrumentation_psram_read_operation(len);
    
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
            // Handle input messages
            if (payload_len > 0) {
                // Calculate payload offset based on frame header size
                size_t header_size = 2; // Basic header size
                if (payload_len >= 126 && payload_len < 65536) {
                    header_size += 2; // Extended payload length
                } else if (payload_len >= 65536) {
                    header_size += 8; // Extended payload length (64-bit)
                }
                if (masked) {
                    header_size += 4; // Mask
                }
                
                if (len >= header_size + payload_len) {
                    // Process the payload
                    uint8_t *payload = buffer + header_size;
                    
                    // Demask if necessary
                    if (masked) {
                        for (size_t i = 0; i < payload_len; i++) {
                            payload[i] ^= mask[i % 4];
                        }
                    }
                    
                    // Handle the input (could be game controls, etc.)
                    // For now, just log the received data
                    ESP_LOGI(TAG, "Received WebSocket frame: opcode=%d, payload_len=%llu", opcode, payload_len);
                }
            }
            break;
        default:
            ESP_LOGW(TAG, "Unhandled WebSocket opcode: %d", opcode);
            break;
    }
    
    heap_caps_free(buffer);
    
    // Update frame receive profiling stats
    uint64_t end_time = esp_timer_get_time();
    uint32_t operation_time_us = (uint32_t)(end_time - start_time);
    update_profile_stats(&frame_recv_stats, operation_time_us);
    
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

    // Initialize input handler
    ESP_LOGI(TAG, "Initializing input handler...");
    ESP_ERROR_CHECK(input_handler_init());
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
