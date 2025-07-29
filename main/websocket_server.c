#include "websocket_server.h"
#include "network_transmission.h"
#include "http_handlers.h"

// Force delta encoding to be disabled
#undef ENABLE_DELTA_ENCODING
#define ENABLE_DELTA_ENCODING 0

// Framebuffer size constant
#define FRAMEBUFFER_SIZE (320 * 240)  // 76,800 bytes

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

// WebSocket server state
httpd_handle_t g_server_handle = NULL;
int ws_client_fds[MAX_WS_CLIENTS];
int ws_client_count = 0;

// Initialize client file descriptors to invalid values
static void init_client_fds(void) {
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        ws_client_fds[i] = -1;
    }
    ws_client_count = 0;
}

// Web page buffer (moved to PSRAM to save internal RAM)
char *index_html = NULL;

// Network transmission message structure - defined in network_transmission.h

/* ============================================================================
 * WEB SOCKET CLIENT MANAGEMENT
 * ============================================================================ */

void ws_add_client(int fd) {
    // Validate file descriptor
    if (fd < 0) {
        ESP_LOGW(TAG, "Invalid file descriptor %d, cannot add client", fd);
        return;
    }
    
    // Check if client is already present
    for (int i = 0; i < ws_client_count; ++i) {
        if (ws_client_fds[i] == fd) {
            ESP_LOGD(TAG, "Client FD %d already present", fd);
            return; // Already present
        }
    }
    
    // Add new client
    if (ws_client_count < MAX_WS_CLIENTS) {
        ws_client_fds[ws_client_count++] = fd;
        ESP_LOGI(TAG, "Client added (FD: %d), total clients: %d", fd, ws_client_count);
    } else {
        ESP_LOGW(TAG, "Maximum number of clients reached, cannot add client FD %d", fd);
    }
}

void ws_remove_client(int fd) {
    // Validate file descriptor
    if (fd < 0) {
        ESP_LOGW(TAG, "Invalid file descriptor %d, cannot remove client", fd);
        return;
    }
    
    for (int i = 0; i < ws_client_count; ++i) {
        if (ws_client_fds[i] == fd) {
            // Move last client to this position and decrement count
            ws_client_fds[i] = ws_client_fds[--ws_client_count];
            // Mark the last position as invalid
            if (ws_client_count > 0) {
                ws_client_fds[ws_client_count] = -1;
            }
            ESP_LOGI(TAG, "Client removed (FD: %d), total clients: %d", fd, ws_client_count);
            return;
        }
    }
    ESP_LOGW(TAG, "Client FD %d not found in client list", fd);
}

// Add a new function to validate client state
bool ws_is_client_valid(int client_index) {
    return (client_index >= 0 && 
            client_index < MAX_WS_CLIENTS && 
            client_index < ws_client_count && 
            ws_client_fds[client_index] >= 0);
}

// Add a function to clean up invalid client entries
void ws_cleanup_invalid_clients(void) {
    int removed_count = 0;
    
    // Ensure client count is valid
    if (ws_client_count < 0) {
        ESP_LOGE(TAG, "Invalid client count: %d, resetting to 0", ws_client_count);
        ws_client_count = 0;
        return;
    }
    
    if (ws_client_count > MAX_WS_CLIENTS) {
        ESP_LOGE(TAG, "Invalid client count: %d, resetting to %d", ws_client_count, MAX_WS_CLIENTS);
        ws_client_count = MAX_WS_CLIENTS;
    }
    
    for (int i = ws_client_count - 1; i >= 0; i--) {
        if (ws_client_fds[i] < 0) {
            // Invalid entry found, remove it
            ESP_LOGW(TAG, "Removing invalid client entry at index %d", i);
            
            // Move last client to this position and decrement count
            if (i < ws_client_count - 1) {
                ws_client_fds[i] = ws_client_fds[ws_client_count - 1];
            }
            ws_client_count--;
            removed_count++;
        }
    }
    
    if (removed_count > 0) {
        ESP_LOGI(TAG, "Cleaned up %d invalid client entries, remaining clients: %d", 
                 removed_count, ws_client_count);
    }
}

/**
 * @brief Validate client array state and log any issues
 */
void ws_validate_client_array(void) {
    bool has_issues = false;
    
    // Check for invalid client count
    if (ws_client_count < 0 || ws_client_count > MAX_WS_CLIENTS) {
        ESP_LOGE(TAG, "Invalid client count: %d (should be 0-%d)", ws_client_count, MAX_WS_CLIENTS);
        has_issues = true;
    }
    
    // Check for invalid file descriptors in valid range
    for (int i = 0; i < ws_client_count; i++) {
        if (ws_client_fds[i] < 0) {
            ESP_LOGE(TAG, "Invalid FD at index %d: %d", i, ws_client_fds[i]);
            has_issues = true;
        }
    }
    
    // Check for valid FDs outside valid range
    for (int i = ws_client_count; i < MAX_WS_CLIENTS; i++) {
        if (ws_client_fds[i] >= 0) {
            ESP_LOGE(TAG, "Valid FD outside range at index %d: %d", i, ws_client_fds[i]);
            has_issues = true;
        }
    }
    
    if (has_issues) {
        ESP_LOGE(TAG, "Client array validation failed. Current state:");
        ESP_LOGE(TAG, "  ws_client_count = %d", ws_client_count);
        for (int i = 0; i < MAX_WS_CLIENTS; i++) {
            ESP_LOGE(TAG, "  ws_client_fds[%d] = %d", i, ws_client_fds[i]);
        }
    }
}

void ws_broadcast_framebuffer(const void *data, size_t len, uint8_t palette_index) {
    if (g_server_handle == NULL || ws_client_count == 0) {
        return; // No server or no clients connected
    }
    
    // Safety check: ensure data is valid
    if (data == NULL || len == 0) {
        ESP_LOGW(TAG, "Invalid framebuffer data: data=%p, len=%zu", data, len);
        return;
    }
    
    
    // Additional validation: check for reasonable framebuffer size
    // Expected size is 320x240 = 76,800 bytes
    if (len != FRAMEBUFFER_SIZE) {
        ESP_LOGW(TAG, "Unexpected framebuffer size: %zu bytes (expected 76800)", len);
        // Don't return here, just log the warning
    }
    
    // Safety check: ensure network queue is initialized
    if (!is_network_transmission_initialized()) {
        ESP_LOGW(TAG, "Network transmission not initialized");
        return;
    }
    
    // Debug: Log framebuffer details
    ESP_LOGD(TAG, "Broadcasting framebuffer: size=%zu, palette=%d, first_byte=0x%02x", 
             len, palette_index, ((const uint8_t*)data)[0]);
    
    ESP_LOGD(TAG, "Queuing framebuffer for async transmission, size: %zu bytes, palette: %d", 
              len, palette_index);

    // Full frame transmission (delta encoding removed)
    ESP_LOGI(TAG, "Using full frame transmission");
    goto send_full_frame;

send_full_frame:
    // Queue full frame for asynchronous transmission
    // Cast to uint8_t* to access the extra byte at the end
    uint8_t *framebuffer_with_palette = (uint8_t*)data;
    
    network_message_t msg = {
        .data = framebuffer_with_palette,
        .len = len,
        .palette_index = palette_index,
        .is_delta = false
    };
    
    if (queue_network_message(&msg) != pdTRUE) {
        // Frame was dropped due to queue full
        return;  // Exit early to skip frame processing
    }
}

/**
 * @brief Check if a client connection is still alive by sending a ping
 * @param client_fd File descriptor of the client to check
 * @return true if client is alive, false if disconnected
 */
bool ws_check_client_alive(int client_fd) {
    if (client_fd < 0 || g_server_handle == NULL) {
        return false;
    }
    
    // Try to send a ping frame to check if client is still connected
    httpd_ws_frame_t ping_frame;
    memset(&ping_frame, 0, sizeof(httpd_ws_frame_t));
    ping_frame.type = HTTPD_WS_TYPE_PING;
    ping_frame.len = 0;
    ping_frame.payload = NULL;
    
    esp_err_t ret = httpd_ws_send_frame_async(g_server_handle, client_fd, &ping_frame);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Client health check failed for FD %d: %s", client_fd, esp_err_to_name(ret));
        return false;
    }
    
    return true;
}

/**
 * @brief Periodic cleanup of stale client connections
 */
void ws_cleanup_stale_clients(void) {
    static uint32_t last_cleanup_time = 0;
    uint32_t current_time = (uint32_t)(esp_timer_get_time() / 1000000); // Convert to seconds
    
    // Run cleanup every 30 seconds
    if (current_time - last_cleanup_time < 30) {
        return;
    }
    
    last_cleanup_time = current_time;
    
    if (ws_client_count == 0) {
        return;
    }
    
    ESP_LOGD(TAG, "Running periodic client cleanup, checking %d clients", ws_client_count);
    
    int removed_count = 0;
    for (int i = ws_client_count - 1; i >= 0; i--) {
        if (ws_client_fds[i] >= 0) {
            if (!ws_check_client_alive(ws_client_fds[i])) {
                ESP_LOGW(TAG, "Removing stale client connection FD %d", ws_client_fds[i]);
                ws_remove_client(ws_client_fds[i]);
                removed_count++;
            }
        }
    }
    
    if (removed_count > 0) {
        ESP_LOGI(TAG, "Cleaned up %d stale client connections", removed_count);
    }
}

/* ============================================================================
 * WEB PAGE INITIALIZATION
 * ============================================================================ */

void init_web_page_buffer(void) {
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = true
    };

    ESP_ERROR_CHECK(esp_vfs_spiffs_register(&conf));

    // Get file size first
    struct stat st;
    if (stat(INDEX_HTML_PATH, &st) != 0) {
        ESP_LOGE(TAG, "index.html not found at %s", INDEX_HTML_PATH);
        return;
    }

    // Allocate buffer in PSRAM
    index_html = heap_caps_malloc(st.st_size + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!index_html) {
        ESP_LOGE(TAG, "Failed to allocate index.html buffer in PSRAM");
        return;
    }

    // Load index.html
    FILE *fp = fopen(INDEX_HTML_PATH, "r");
    if (fp == NULL) {
        ESP_LOGE(TAG, "Failed to open %s", INDEX_HTML_PATH);
        free(index_html);
        index_html = NULL;
        return;
    }

    if (fread(index_html, st.st_size, 1, fp) == 0) {
        ESP_LOGE(TAG, "Failed to read %s", INDEX_HTML_PATH);
        free(index_html);
        index_html = NULL;
    } else {
        index_html[st.st_size] = '\0'; // Ensure null termination
        ESP_LOGI(TAG, "Loaded index.html (%ld bytes) to PSRAM", st.st_size);
    }
    
    fclose(fp);
}

/* ============================================================================
 * WEB SOCKET REQUEST HANDLING
 * ============================================================================ */

esp_err_t handle_ws_req(httpd_req_t *req) {
    // Handle WebSocket handshake
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "WebSocket handshake completed, new connection opened");
        
        // Add client to list
        if (ws_client_count < MAX_WS_CLIENTS) {
            ws_client_fds[ws_client_count] = httpd_req_to_sockfd(req);
            ws_client_count++;
            ESP_LOGI(TAG, "Client added, total clients: %d", ws_client_count);
        } else {
            ESP_LOGW(TAG, "Maximum number of clients reached, rejecting connection");
        }
    }

    httpd_ws_frame_t ws_pkt;
    uint8_t *buf = NULL;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    
    // Enhanced error handling for frame reception
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        // Check if this is a client disconnection error
        if (ret == ESP_FAIL || ret == ESP_ERR_INVALID_ARG) {
            ESP_LOGW(TAG, "Client appears to have disconnected during frame reception: %s", 
                     esp_err_to_name(ret));
            
            // Remove client from list if it exists
            int client_fd = httpd_req_to_sockfd(req);
            ws_remove_client(client_fd);
            
            // Return success to prevent further error handling
            return ESP_OK;
        }
        
        ESP_LOGE(TAG, "httpd_ws_recv_frame failed to get frame len: %s", 
                 esp_err_to_name(ret));
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
            // Check if this is a client disconnection error
            if (ret == ESP_FAIL || ret == ESP_ERR_INVALID_ARG) {
                ESP_LOGW(TAG, "Client disconnected during payload reception: %s", 
                         esp_err_to_name(ret));
                
                // Remove client from list if it exists
                int client_fd = httpd_req_to_sockfd(req);
                ws_remove_client(client_fd);
                
                free(buf);
                return ESP_OK;
            }
            
            ESP_LOGE(TAG, "httpd_ws_recv_frame failed: %s", esp_err_to_name(ret));
            free(buf);
            return ret;
        }
        
        ESP_LOGI(TAG, "Received WebSocket message: %s", ws_pkt.payload);
    }

    ESP_LOGD(TAG, "WebSocket frame length: %d", ws_pkt.len);

    // Handle toggle command
    if (ws_pkt.type == HTTPD_WS_TYPE_TEXT &&
        strcmp((char *)ws_pkt.payload, "toggle") == 0) {
        free(buf);
        return trigger_async_send(req->handle, req);
    }
    
    // Handle client disconnect
    if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        int client_fd = httpd_req_to_sockfd(req);
        ws_remove_client(client_fd);
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

httpd_handle_t setup_websocket_server(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;  // Increase stack size to prevent crashes
    config.task_priority = 2;   // Lower priority than DOOM task
    config.max_uri_handlers = 8; // Increase handler limit
    config.max_resp_headers = 8; // Increase header limit
    config.recv_wait_timeout = 10; // 10 second receive timeout
    config.send_wait_timeout = 10; // 10 second send timeout
    config.lru_purge_enable = true; // Enable LRU purge for better memory management
    
    static httpd_handle_t server = NULL;
    
    // Initialize client file descriptors
    init_client_fds();

    // Configure URI handlers
    httpd_uri_t uri_get = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_handler,
        .user_ctx = NULL
    };

    httpd_uri_t doom_palette_js_uri = {
        .uri = "/doom-palette.js",
        .method = HTTP_GET,
        .handler = doom_palette_js_handler,
        .user_ctx = NULL
    };

    httpd_uri_t ws = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = handle_ws_req,
        .user_ctx = NULL,
        .is_websocket = true
    };

    esp_err_t ret = httpd_start(&server, &config);
    if (ret == ESP_OK) {
        ret = httpd_register_uri_handler(server, &uri_get);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register index handler: %s", esp_err_to_name(ret));
        }
        
        ret = httpd_register_uri_handler(server, &doom_palette_js_uri);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register palette handler: %s", esp_err_to_name(ret));
        }
        
        ret = httpd_register_uri_handler(server, &ws);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register WebSocket handler: %s", esp_err_to_name(ret));
        }
        
        g_server_handle = server; // Store global reference
        ESP_LOGI(TAG, "WebSocket server started successfully");
    } else {
        ESP_LOGE(TAG, "Failed to start WebSocket server: %s", esp_err_to_name(ret));
    }

    return server;
}

esp_err_t stop_webserver(httpd_handle_t server) {
    if (server) {
        // Stop the httpd server
        return httpd_stop(server);
    }
    return ESP_FAIL;
}

void cleanup_websocket_resources(void) {
    ESP_LOGI(TAG, "Cleaning up WebSocket resources...");
    
    // Free web page buffer
    if (index_html) {
        heap_caps_free(index_html);
        index_html = NULL;
    }
    
    ESP_LOGI(TAG, "WebSocket resources cleaned up");
}

/* ============================================================================
 * NETWORK EVENT HANDLERS
 * ============================================================================ */

/**
 * @brief Handle network disconnect events
 * @param arg Server handle pointer
 * @param event_base Event base
 * @param event_id Event ID
 * @param event_data Event data
 */
void disconnect_handler(void* arg, esp_event_base_t event_base,
                      int32_t event_id, void* event_data) {
    httpd_handle_t* server = (httpd_handle_t*) arg;
    if (*server) {
        ESP_LOGI(TAG, "Network disconnected, stopping web server");
        if (stop_webserver(*server) == ESP_OK) {
            *server = NULL;
            g_server_handle = NULL; // Clear global reference
        } else {
            ESP_LOGE(TAG, "Failed to stop web server");
        }
    }
}

/**
 * @brief Handle network connect events
 * @param arg Server handle pointer
 * @param event_base Event base
 * @param event_id Event ID
 * @param event_data Event data
 */
void connect_handler(void* arg, esp_event_base_t event_base,
                    int32_t event_id, void* event_data) {
    httpd_handle_t* server = (httpd_handle_t*) arg;
    if (*server == NULL) {
        ESP_LOGI(TAG, "Network connected, starting web server");
        *server = setup_websocket_server();
        g_server_handle = *server; // Update global reference
    }
} 