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

    char response[512];
    snprintf(response, sizeof(response),
             "HTTP/1.1 101 Switching Protocols\r\n"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             "Sec-WebSocket-Accept: %s\r\n\r\n", accept_key);

    heap_caps_free(buffer);
    return nonblocking_send(client_fd, response, strlen(response), 5000);
}

// Send WebSocket binary frame with non-blocking operations
int websocket_send_binary_frame(int client_fd, const uint8_t *data, size_t len) {
    const size_t max_chunk_size = 16384; // 16KB per fragment
    size_t offset = 0;
    int fragment_count = 0;
    
    ESP_LOGI(TAG, "Sending WebSocket binary frame in fragments: total_size=%zu", len);
    
    while (offset < len) {
        size_t chunk_size = (len - offset < max_chunk_size) ? (len - offset) : max_chunk_size;
        bool is_last = (offset + chunk_size >= len);
        
        uint8_t header[10];
        size_t header_len = 0;
        
        // Set FIN bit only on last fragment, opcode only on first fragment
        if (fragment_count == 0) {
            header[0] = is_last ? 0x82 : 0x02; // First fragment: binary frame, FIN only if last
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
        
        ESP_LOGI(TAG, "Sending fragment %d: offset=%zu, size=%zu, is_last=%d", 
                 fragment_count, offset, chunk_size, is_last);
        
        // Send header with non-blocking operation
        if (nonblocking_send(client_fd, header, header_len, 1000) < 0) {
            ESP_LOGE(TAG, "Failed to send WebSocket fragment header");
            return -1;
        }
        
        // Send fragment data with non-blocking operation
        if (nonblocking_send(client_fd, data + offset, chunk_size, 1000) < 0) {
            ESP_LOGE(TAG, "Failed to send WebSocket fragment data");
            return -1;
        }
        
        offset += chunk_size;
        fragment_count++;
        
        // Small delay between chunks to allow lwIP to process
        if (offset < len) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
    
    ESP_LOGI(TAG, "WebSocket fragmentation complete: %d fragments sent, total=%zu bytes", 
             fragment_count, len);
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

    ESP_LOGI(TAG, "Sending WebSocket text frame: size=%zu", len);
    
    if (nonblocking_send(client_fd, header, header_len, 1000) < 0) {
        ESP_LOGE(TAG, "Failed to send text frame header");
        return -1;
    }
    
    if (nonblocking_send(client_fd, text, len, 1000) < 0) {
        ESP_LOGE(TAG, "Failed to send text frame data");
        return -1;
    }
    
    ESP_LOGI(TAG, "WebSocket text frame sent successfully: %zu bytes", len);
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
    
    ESP_LOGI(TAG, "Received %d bytes from client", len);
    
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
        server->client_fds[i] = -1;
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
        if (server->client_fds[i] >= 0) {
            close(server->client_fds[i]);
            server->client_fds[i] = -1;
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
                if (server->client_fds[i] == -1) {
                    server->client_fds[i] = client_fd;
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
            if (server->client_fds[i] >= 0) {
                // Handle incoming frames
                int result = handle_ws_frame(server->client_fds[i]);
                if (result < 0) {
                    ESP_LOGI(TAG, "Client disconnected (frame handling failed)");
                    close(server->client_fds[i]);
                    server->client_fds[i] = -1;
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
            ESP_LOGI(TAG, "Sending frame of size %zu bytes to %d clients", (size_t)(FRAME_SIZE + 1), server->client_count);
            for (int i = 0; i < WS_MAX_CLIENTS; i++) {
                if (server->client_fds[i] >= 0) {
                    ESP_LOGI(TAG, "Sending frame to client %d, palette index: %d", i, frame[0]);
                    if (websocket_send_binary_frame(server->client_fds[i], frame, FRAME_SIZE + 1) < 0) {
                        ESP_LOGW(TAG, "Failed to send frame to client %d", i);
                        close(server->client_fds[i]);
                        server->client_fds[i] = -1;
                        server->client_count--;
                    } else {
                        ESP_LOGI(TAG, "Frame sent successfully to client %d", i);
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
        if (ping_counter >= 1000) { // Send ping every ~1 seconds
            for (int i = 0; i < WS_MAX_CLIENTS; i++) {
                if (server->client_fds[i] >= 0) {
                    websocket_send_ping(server->client_fds[i]);
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
