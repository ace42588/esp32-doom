#include "frame_broadcaster.h"
#include "websocket_server.h"
#include "network_transmission.h"
#include "esp_log.h"
#include <stdbool.h>

static const char *TAG = "Frame Broadcaster";

esp_err_t broadcast_framebuffer(const void *data, size_t len, uint8_t palette_index) {
    if (!data || len == 0) {
        ESP_LOGW(TAG, "Invalid framebuffer data: data=%p, len=%zu", data, len);
        return ESP_ERR_INVALID_ARG;
    }
    
    // Check if WebSocket server is ready
    if (!websocket_server_is_ready()) {
        ESP_LOGW(TAG, "WebSocket server not ready");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Check if network transmission is ready
    if (!network_transmission_is_ready()) {
        ESP_LOGW(TAG, "Network transmission not ready");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Check if there are connected clients
    int client_count = websocket_get_client_count();
    if (client_count == 0) {
        // No clients connected, this is normal, just return success
        return ESP_OK;
    }
    
    ESP_LOGD(TAG, "Broadcasting framebuffer: size=%zu, palette=%d, clients=%d", 
             len, palette_index, client_count);

    // Queue frame for asynchronous transmission to all clients
    esp_err_t ret = network_queue_frame(data, len, palette_index, -1); // -1 means broadcast to all
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to queue frame for transmission: %s", esp_err_to_name(ret));
    }
    
    return ret;
}

bool has_connected_clients(void) {
    return websocket_get_client_count() > 0;
}

int get_client_count(void) {
    return websocket_get_client_count();
} 