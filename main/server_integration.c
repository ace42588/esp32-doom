#include "server_integration.h"
#include "http_handlers.h"
#include "websocket_server.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ServerIntegration";

// Global HTTP server handle
static httpd_handle_t g_http_server = NULL;

// WebSocket server instance
static websocket_server_t g_websocket_server = {0};

/* ============================================================================
 * HTTP SERVER CONFIGURATION
 * ============================================================================ */

static const httpd_uri_t index_uri = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = http_index_handler,
    .user_ctx = NULL
};

static const httpd_uri_t index_html_uri = {
    .uri = "/index.html",
    .method = HTTP_GET,
    .handler = http_index_handler,
    .user_ctx = NULL
};

static const httpd_uri_t palette_uri = {
    .uri = "/doom-palette.js",
    .method = HTTP_GET,
    .handler = http_palette_handler,
    .user_ctx = NULL
};

static const httpd_uri_t websocket_uri = {
    .uri = "/ws",
    .method = HTTP_GET,
    .handler = server_websocket_upgrade_handler,
    .user_ctx = NULL
};

/* ============================================================================
 * SERVER INTEGRATION FUNCTIONS
 * ============================================================================ */

esp_err_t server_integration_init(void) {
    ESP_LOGI(TAG, "Initializing server integration");
    
    // Load static files
    esp_err_t ret = http_load_static_files();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load static files");
        return ret;
    }
    
    // Initialize WebSocket server
    websocket_server_init(&g_websocket_server);
    
    ESP_LOGI(TAG, "Server integration initialized");
    return ESP_OK;
}

esp_err_t server_integration_start(void) {
    ESP_LOGI(TAG, "Starting server integration");
    
    // HTTP server configuration
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = HTTP_SERVER_PORT;
    config.max_uri_handlers = HTTP_SERVER_MAX_URI_HANDLERS;
    
    // Start HTTP server
    esp_err_t ret = httpd_start(&g_http_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return ret;
    }
    
    // Register URI handlers
    httpd_register_uri_handler(g_http_server, &index_uri);
    httpd_register_uri_handler(g_http_server, &index_html_uri);
    httpd_register_uri_handler(g_http_server, &palette_uri);
    httpd_register_uri_handler(g_http_server, &websocket_uri);
    
    // Start WebSocket server task (it will start the server internally)
    BaseType_t ws_task_created = xTaskCreate(websocket_server_task, "websocket_server", 8192, NULL, 2, NULL);
    if (ws_task_created == pdPASS) {
        ESP_LOGI(TAG, "WebSocket server task created successfully");
    } else {
        ESP_LOGE(TAG, "Failed to create WebSocket server task");
    }
    
    ESP_LOGI(TAG, "Server integration started on port %d", HTTP_SERVER_PORT);
    return ESP_OK;
}

void server_integration_stop(void) {
    ESP_LOGI(TAG, "Stopping server integration");
    
    if (g_http_server) {
        httpd_stop(g_http_server);
        g_http_server = NULL;
    }
    
    websocket_server_stop(&g_websocket_server);
    
    ESP_LOGI(TAG, "Server integration stopped");
}

void server_integration_task(void *pv) {
    ESP_LOGI(TAG, "Starting server integration task");
    
    // Initialize and start the server integration
    esp_err_t ret = server_integration_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize server integration");
        vTaskDelete(NULL);
        return;
    }
    
    ret = server_integration_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start server integration");
        vTaskDelete(NULL);
        return;
    }
    
    // Keep the task running
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ============================================================================
 * WEBSOCKET UPGRADE HANDLER
 * ============================================================================ */

esp_err_t server_websocket_upgrade_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "WebSocket upgrade request received");
    
    // Check if this is a WebSocket upgrade request
    char upgrade_header[64];
    char connection_header[64];
    char ws_key[64];
    
    esp_err_t ret1 = httpd_req_get_hdr_value_str(req, "Upgrade", upgrade_header, sizeof(upgrade_header));
    esp_err_t ret2 = httpd_req_get_hdr_value_str(req, "Connection", connection_header, sizeof(connection_header));
    esp_err_t ret3 = httpd_req_get_hdr_value_str(req, "Sec-WebSocket-Key", ws_key, sizeof(ws_key));
    
    if (ret1 != ESP_OK || ret2 != ESP_OK || ret3 != ESP_OK) {
        ESP_LOGE(TAG, "Missing WebSocket upgrade headers");
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    
    if (strcasecmp(upgrade_header, "websocket") != 0) {
        ESP_LOGE(TAG, "Invalid upgrade header: %s", upgrade_header);
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    
    if (strstr(connection_header, "Upgrade") == NULL) {
        ESP_LOGE(TAG, "Invalid connection header: %s", connection_header);
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    
    // Generate WebSocket accept key (simplified)
    char accept_key[256];
    snprintf(accept_key, sizeof(accept_key), "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", ws_key);
    
    // Send WebSocket upgrade response
    httpd_resp_set_status(req, "101 Switching Protocols");
    httpd_resp_set_hdr(req, "Upgrade", "websocket");
    httpd_resp_set_hdr(req, "Connection", "Upgrade");
    httpd_resp_set_hdr(req, "Sec-WebSocket-Accept", accept_key);
    httpd_resp_send(req, NULL, 0);
    
    ESP_LOGI(TAG, "WebSocket upgrade successful");
    
    // Note: The actual WebSocket communication will be handled by the WebSocket server
    // This handler just performs the HTTP upgrade to WebSocket protocol
    
    return ESP_OK;
} 