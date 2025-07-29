#include "websocket_server.h"
#include "http_handlers.h"

#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "WebSocket Server";

// WebSocket server instance
static websocket_server_t g_websocket_server = {0};

// Web page buffer (moved to PSRAM to save internal RAM)
static char *g_index_html = NULL;
static char *g_palette_js = NULL;

/* ============================================================================
 * WEB SOCKET CLIENT MANAGEMENT
 * ============================================================================ */

esp_err_t websocket_add_client(int fd) {
    if (fd < 0) {
        ESP_LOGW(TAG, "Invalid file descriptor %d, cannot add client", fd);
        return ESP_ERR_INVALID_ARG;
    }
    
    // Check if client is already present
    for (int i = 0; i < g_websocket_server.client_count; ++i) {
        if (g_websocket_server.client_fds[i] == fd) {
            ESP_LOGD(TAG, "Client FD %d already present", fd);
            return ESP_OK; // Already present
        }
    }
    
    // Add new client
    if (g_websocket_server.client_count < MAX_WS_CLIENTS) {
        g_websocket_server.client_fds[g_websocket_server.client_count++] = fd;
        ESP_LOGI(TAG, "Client added (FD: %d), total clients: %d", fd, g_websocket_server.client_count);
        return ESP_OK;
    } else {
        ESP_LOGW(TAG, "Maximum number of clients reached, cannot add client FD %d", fd);
        return ESP_ERR_NO_MEM;
    }
}

esp_err_t websocket_remove_client(int fd) {
    if (fd < 0) {
        ESP_LOGW(TAG, "Invalid file descriptor %d, cannot remove client", fd);
        return ESP_ERR_INVALID_ARG;
    }
    
    for (int i = 0; i < g_websocket_server.client_count; ++i) {
        if (g_websocket_server.client_fds[i] == fd) {
            // Move last client to this position and decrement count
            g_websocket_server.client_fds[i] = g_websocket_server.client_fds[--g_websocket_server.client_count];
            // Mark the last position as invalid
            if (g_websocket_server.client_count > 0) {
                g_websocket_server.client_fds[g_websocket_server.client_count] = -1;
            }
            ESP_LOGI(TAG, "Client removed (FD: %d), total clients: %d", fd, g_websocket_server.client_count);
            return ESP_OK;
        }
    }
    ESP_LOGW(TAG, "Client FD %d not found in client list", fd);
    return ESP_ERR_NOT_FOUND;
}

int websocket_get_client_count(void) {
    return g_websocket_server.client_count;
}

int websocket_get_client_fd(int index) {
    if (index >= 0 && index < g_websocket_server.client_count) {
        return g_websocket_server.client_fds[index];
    }
    return -1;
}

bool websocket_is_client_valid(int client_index) {
    return (client_index >= 0 && 
            client_index < MAX_WS_CLIENTS && 
            client_index < g_websocket_server.client_count && 
            g_websocket_server.client_fds[client_index] >= 0);
}

/* ============================================================================
 * WEB SOCKET FRAME HANDLING
 * ============================================================================ */

esp_err_t websocket_send_binary_frame(int client_fd, const uint8_t *data, size_t len) {
    if (!g_websocket_server.is_initialized || client_fd < 0 || !data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_BINARY;
    ws_pkt.final = 1;
    ws_pkt.fragmented = 0;
    ws_pkt.payload = (uint8_t*)data;
    ws_pkt.len = len;
    
    esp_err_t ret = httpd_ws_send_frame_async(g_websocket_server.server_handle, client_fd, &ws_pkt);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send binary frame to client %d: %s", client_fd, esp_err_to_name(ret));
    }
    
    return ret;
}

esp_err_t websocket_send_fragmented_frame(int client_fd, const uint8_t *data, size_t len, uint8_t palette_index) {
    if (!g_websocket_server.is_initialized || client_fd < 0 || !data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // For small frames, send directly
    if (len <= FRAGMENT_SIZE) {
        // Prepend palette index
        uint8_t *frame_with_palette = malloc(len + 1);
        if (!frame_with_palette) {
            return ESP_ERR_NO_MEM;
        }
        
        frame_with_palette[0] = palette_index;
        memcpy(frame_with_palette + 1, data, len);
        
        esp_err_t ret = websocket_send_binary_frame(client_fd, frame_with_palette, len + 1);
        free(frame_with_palette);
        return ret;
    }
    
    // For large frames, use fragmentation
    size_t offset = 0;
    size_t chunk_size = FRAGMENT_SIZE - 1; // Leave room for palette index in first chunk
    bool is_first_chunk = true;
    bool is_last_chunk = false;
    int chunk_count = 0;
    
    ESP_LOGI(TAG, "Starting fragmented transmission: %zu bytes, palette %d", len, palette_index);
    
    while (offset < len) {
        size_t current_chunk_size = (offset + chunk_size > len) ? (len - offset) : chunk_size;
        is_last_chunk = (offset + current_chunk_size >= len);
        chunk_count++;
        
        // Allocate buffer for this chunk
        size_t buffer_size = current_chunk_size + (is_first_chunk ? 1 : 0);
        uint8_t *chunk_buffer = malloc(buffer_size);
        if (!chunk_buffer) {
            ESP_LOGE(TAG, "Failed to allocate chunk buffer");
            return ESP_ERR_NO_MEM;
        }
        
        // Create WebSocket frame
        httpd_ws_frame_t ws_pkt;
        memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
        ws_pkt.type = is_first_chunk ? HTTPD_WS_TYPE_BINARY : HTTPD_WS_TYPE_CONTINUE;
        ws_pkt.final = is_last_chunk ? 1 : 0;
        ws_pkt.fragmented = 1;
        ws_pkt.payload = chunk_buffer;
        
        if (is_first_chunk) {
            // First chunk: add palette index at the beginning
            chunk_buffer[0] = palette_index;
            memcpy(chunk_buffer + 1, data + offset, current_chunk_size);
            ws_pkt.len = current_chunk_size + 1;
            is_first_chunk = false;
        } else {
            // Subsequent chunks: send data directly
            memcpy(chunk_buffer, data + offset, current_chunk_size);
            ws_pkt.len = current_chunk_size;
        }
        
        // Send the chunk
        esp_err_t ret = httpd_ws_send_frame_async(g_websocket_server.server_handle, client_fd, &ws_pkt);
        free(chunk_buffer);
        
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send chunk %d: %s", chunk_count, esp_err_to_name(ret));
            return ret;
        }
        
        // Small delay between chunks for reliability
        if (!is_last_chunk) {
            vTaskDelay(pdMS_TO_TICKS(2));
        }
        
        offset += current_chunk_size;
    }
    
    ESP_LOGI(TAG, "Completed fragmented transmission: %zu bytes in %d chunks", len, chunk_count);
    return ESP_OK;
}

/* ============================================================================
 * WEB PAGE INITIALIZATION
 * ============================================================================ */

static esp_err_t load_static_files(void) {
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = true
    };

    ESP_ERROR_CHECK(esp_vfs_spiffs_register(&conf));

    // Load index.html
    struct stat st;
    if (stat(INDEX_HTML_PATH, &st) == 0) {
        g_index_html = heap_caps_malloc(st.st_size + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (g_index_html) {
            FILE *fp = fopen(INDEX_HTML_PATH, "r");
            if (fp) {
                if (fread(g_index_html, st.st_size, 1, fp) == 1) {
                    g_index_html[st.st_size] = '\0';
                    ESP_LOGI(TAG, "Loaded index.html (%ld bytes)", st.st_size);
                } else {
                    heap_caps_free(g_index_html);
                    g_index_html = NULL;
                }
                fclose(fp);
            } else {
                heap_caps_free(g_index_html);
                g_index_html = NULL;
            }
        }
    }

    // Load doom-palette.js
    if (stat(DOOM_PALETTE_JS_PATH, &st) == 0) {
        g_palette_js = heap_caps_malloc(st.st_size + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (g_palette_js) {
            FILE *fp = fopen(DOOM_PALETTE_JS_PATH, "r");
            if (fp) {
                if (fread(g_palette_js, st.st_size, 1, fp) == 1) {
                    g_palette_js[st.st_size] = '\0';
                    ESP_LOGI(TAG, "Loaded doom-palette.js (%ld bytes)", st.st_size);
                } else {
                    heap_caps_free(g_palette_js);
                    g_palette_js = NULL;
                }
                fclose(fp);
            } else {
                heap_caps_free(g_palette_js);
                g_palette_js = NULL;
            }
        }
    }

    return ESP_OK;
}

/* ============================================================================
 * HTTP REQUEST HANDLERS
 * ============================================================================ */

esp_err_t websocket_index_handler(httpd_req_t *req) {
    if (g_index_html) {
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, g_index_html, strlen(g_index_html));
    } else {
        httpd_resp_send_404(req);
    }
    return ESP_OK;
}

esp_err_t websocket_palette_handler(httpd_req_t *req) {
    if (g_palette_js) {
        httpd_resp_set_type(req, "application/javascript");
        httpd_resp_send(req, g_palette_js, strlen(g_palette_js));
    } else {
        httpd_resp_send_404(req);
    }
    return ESP_OK;
}

esp_err_t websocket_ws_handler(httpd_req_t *req) {
    // Handle WebSocket handshake
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "WebSocket handshake completed, new connection opened");
        
        // Add client to list
        esp_err_t ret = websocket_add_client(httpd_req_to_sockfd(req));
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to add client to list");
        }
    }

    httpd_ws_frame_t ws_pkt;
    uint8_t *buf = NULL;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    
    // Receive frame
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL || ret == ESP_ERR_INVALID_ARG) {
            ESP_LOGW(TAG, "Client disconnected during frame reception");
            websocket_remove_client(httpd_req_to_sockfd(req));
            return ESP_OK;
        }
        ESP_LOGE(TAG, "httpd_ws_recv_frame failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (ws_pkt.len > 0) {
        buf = calloc(1, ws_pkt.len + 1);
        if (buf == NULL) {
            ESP_LOGE(TAG, "Failed to allocate memory for WebSocket buffer");
            return ESP_ERR_NO_MEM;
        }
        
        ws_pkt.payload = buf;
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret != ESP_OK) {
            if (ret == ESP_FAIL || ret == ESP_ERR_INVALID_ARG) {
                ESP_LOGW(TAG, "Client disconnected during payload reception");
                websocket_remove_client(httpd_req_to_sockfd(req));
                free(buf);
                return ESP_OK;
            }
            ESP_LOGE(TAG, "httpd_ws_recv_frame failed: %s", esp_err_to_name(ret));
            free(buf);
            return ret;
        }
        
        ESP_LOGI(TAG, "Received WebSocket message: %s", ws_pkt.payload);
    }

    // Handle client disconnect
    if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        websocket_remove_client(httpd_req_to_sockfd(req));
        ESP_LOGI(TAG, "Client disconnected");
    }
    
    if (buf) {
        free(buf);
    }
    
    return ESP_OK;
}

/* ============================================================================
 * WEB SERVER SETUP AND MANAGEMENT
 * ============================================================================ */

esp_err_t websocket_server_init(void) {
    if (g_websocket_server.is_initialized) {
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Initializing WebSocket server");
    
    // Initialize client file descriptors
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        g_websocket_server.client_fds[i] = -1;
    }
    g_websocket_server.client_count = 0;
    g_websocket_server.server_handle = NULL;
    
    // Load static files
    esp_err_t ret = load_static_files();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to load static files, continuing anyway");
    }
    
    g_websocket_server.is_initialized = true;
    ESP_LOGI(TAG, "WebSocket server initialized");
    
    return ESP_OK;
}

esp_err_t websocket_server_start(void) {
    if (!g_websocket_server.is_initialized) {
        ESP_LOGE(TAG, "WebSocket server not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (g_websocket_server.server_handle) {
        ESP_LOGW(TAG, "WebSocket server already running");
        return ESP_OK;
    }
    
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.task_priority = 2;
    config.max_uri_handlers = 8;
    config.max_resp_headers = 8;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;
    config.lru_purge_enable = true;
    
    esp_err_t ret = httpd_start(&g_websocket_server.server_handle, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Register URI handlers
    httpd_uri_t uri_get = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = websocket_index_handler,
        .user_ctx = NULL
    };

    httpd_uri_t palette_uri = {
        .uri = "/doom-palette.js",
        .method = HTTP_GET,
        .handler = websocket_palette_handler,
        .user_ctx = NULL
    };

    httpd_uri_t ws_uri = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = websocket_ws_handler,
        .user_ctx = NULL,
        .is_websocket = true
    };

    ret = httpd_register_uri_handler(g_websocket_server.server_handle, &uri_get);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register index handler: %s", esp_err_to_name(ret));
    }
    
    ret = httpd_register_uri_handler(g_websocket_server.server_handle, &palette_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register palette handler: %s", esp_err_to_name(ret));
    }
    
    ret = httpd_register_uri_handler(g_websocket_server.server_handle, &ws_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register WebSocket handler: %s", esp_err_to_name(ret));
    }
    
    ESP_LOGI(TAG, "WebSocket server started successfully");
    return ESP_OK;
}

esp_err_t websocket_server_stop(void) {
    if (g_websocket_server.server_handle) {
        esp_err_t ret = httpd_stop(g_websocket_server.server_handle);
        g_websocket_server.server_handle = NULL;
        return ret;
    }
    return ESP_OK;
}

void websocket_server_cleanup(void) {
    ESP_LOGI(TAG, "Cleaning up WebSocket server resources");
    
    websocket_server_stop();
    
    // Free static file buffers
    if (g_index_html) {
        heap_caps_free(g_index_html);
        g_index_html = NULL;
    }
    
    if (g_palette_js) {
        heap_caps_free(g_palette_js);
        g_palette_js = NULL;
    }
    
    g_websocket_server.is_initialized = false;
    ESP_LOGI(TAG, "WebSocket server resources cleaned up");
}

/* ============================================================================
 * NETWORK EVENT HANDLERS
 * ============================================================================ */

void websocket_connect_handler(void* arg, esp_event_base_t event_base,
                             int32_t event_id, void* event_data) {
    ESP_LOGI(TAG, "Network connected, starting WebSocket server");
    websocket_server_start();
}

void websocket_disconnect_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data) {
    ESP_LOGI(TAG, "Network disconnected, stopping WebSocket server");
    websocket_server_stop();
}

/* ============================================================================
 * UTILITY FUNCTIONS
 * ============================================================================ */

bool websocket_server_is_ready(void) {
    return g_websocket_server.is_initialized && g_websocket_server.server_handle != NULL;
}

httpd_handle_t websocket_get_server_handle(void) {
    return g_websocket_server.server_handle;
} 