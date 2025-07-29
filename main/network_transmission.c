#include "network_transmission.h"
#include "websocket_server.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <sys/socket.h>
#include <sys/time.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "esp_http_server.h"

static const char *TAG = "Network Transmission";

// Network transmission instance
static network_transmission_t g_network_transmission = {0};

// Socket timeout configuration
static struct timeval g_socket_timeout = {
    .tv_sec = 0,
    .tv_usec = 50000  // 50ms timeout
};

/* ============================================================================
 * BUFFER POOL MANAGEMENT
 * ============================================================================ */

static esp_err_t init_buffer_pool(void) {
    if (g_network_transmission.is_initialized) {
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Initializing buffer pool with %d buffers of %d bytes each", 
             BUFFER_POOL_SIZE, MAX_BUFFER_SIZE);
    
    for (int i = 0; i < BUFFER_POOL_SIZE; i++) {
        // Try to allocate from PSRAM first, fallback to internal RAM
        if (esp_psram_is_initialized()) {
            g_network_transmission.buffer_pool[i].buffer = heap_caps_malloc(MAX_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        } else {
            g_network_transmission.buffer_pool[i].buffer = malloc(MAX_BUFFER_SIZE);
        }
        
        if (!g_network_transmission.buffer_pool[i].buffer) {
            ESP_LOGE(TAG, "Failed to allocate buffer %d in pool (%d bytes)", i, MAX_BUFFER_SIZE);
            // Clean up already allocated buffers
            for (int j = 0; j < i; j++) {
                if (esp_psram_is_initialized()) {
                    heap_caps_free(g_network_transmission.buffer_pool[j].buffer);
                } else {
                    free(g_network_transmission.buffer_pool[j].buffer);
                }
                g_network_transmission.buffer_pool[j].buffer = NULL;
            }
            return ESP_ERR_NO_MEM;
        }
        g_network_transmission.buffer_pool[i].in_use = false;
        g_network_transmission.buffer_pool[i].size = MAX_BUFFER_SIZE;
    }
    
    ESP_LOGI(TAG, "Buffer pool initialized with %d buffers of size %d", BUFFER_POOL_SIZE, MAX_BUFFER_SIZE);
    return ESP_OK;
}

static void cleanup_buffer_pool(void) {
    for (int i = 0; i < BUFFER_POOL_SIZE; i++) {
        if (g_network_transmission.buffer_pool[i].buffer) {
            if (esp_psram_is_initialized()) {
                heap_caps_free(g_network_transmission.buffer_pool[i].buffer);
            } else {
                free(g_network_transmission.buffer_pool[i].buffer);
            }
            g_network_transmission.buffer_pool[i].buffer = NULL;
        }
        g_network_transmission.buffer_pool[i].in_use = false;
    }
    ESP_LOGI(TAG, "Buffer pool cleaned up");
}

uint8_t* network_get_buffer(size_t size) {
    if (size > MAX_BUFFER_SIZE) {
        // Requested size too large for pool, use direct allocation
        if (esp_psram_is_initialized()) {
            return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        } else {
            return malloc(size);
        }
    }
    
    for (int i = 0; i < BUFFER_POOL_SIZE; i++) {
        if (!g_network_transmission.buffer_pool[i].in_use) {
            g_network_transmission.buffer_pool[i].in_use = true;
            return g_network_transmission.buffer_pool[i].buffer;
        }
    }
    
    // No available buffer in pool, fall back to malloc
    ESP_LOGW(TAG, "Buffer pool exhausted, falling back to malloc");
    if (esp_psram_is_initialized()) {
        return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    } else {
        return malloc(size);
    }
}

void network_return_buffer(uint8_t *buffer) {
    for (int i = 0; i < BUFFER_POOL_SIZE; i++) {
        if (g_network_transmission.buffer_pool[i].buffer == buffer) {
            g_network_transmission.buffer_pool[i].in_use = false;
            return;
        }
    }
    
    // Buffer not from pool, free it
    if (esp_psram_is_initialized()) {
        heap_caps_free(buffer);
    } else {
        free(buffer);
    }
}

/* ============================================================================
 * NETWORK TRANSMISSION MANAGEMENT
 * ============================================================================ */

esp_err_t network_transmission_init(void) {
    if (g_network_transmission.is_initialized) {
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Initializing network transmission");
    
    // Initialize buffer pool
    esp_err_t pool_ret = init_buffer_pool();
    if (pool_ret != ESP_OK) {
        ESP_LOGW(TAG, "Buffer pool initialization failed, continuing without pool");
    }
    
    // Create fragmentation mutex
    g_network_transmission.fragmentation_mutex = xSemaphoreCreateMutex();
    if (!g_network_transmission.fragmentation_mutex) {
        ESP_LOGE(TAG, "Failed to create fragmentation mutex");
        cleanup_buffer_pool();
        return ESP_ERR_NO_MEM;
    }

    // Create queue for network messages
    g_network_transmission.message_queue = xQueueCreate(NETWORK_QUEUE_SIZE, sizeof(network_message_t));
    if (!g_network_transmission.message_queue) {
        ESP_LOGE(TAG, "Failed to create network queue");
        vSemaphoreDelete(g_network_transmission.fragmentation_mutex);
        cleanup_buffer_pool();
        return ESP_ERR_NO_MEM;
    }
    
    // Create network transmission task
    BaseType_t task_created = xTaskCreatePinnedToCore(
        network_transmission_task,
        "network_tx",
        NETWORK_TASK_STACK_SIZE,
        NULL,
        NETWORK_TASK_PRIORITY,
        &g_network_transmission.task_handle,
        NETWORK_TASK_CORE
    );  
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create network transmission task");
        vQueueDelete(g_network_transmission.message_queue);
        vSemaphoreDelete(g_network_transmission.fragmentation_mutex);
        cleanup_buffer_pool();
        return ESP_ERR_NO_MEM;
    }

    g_network_transmission.is_initialized = true;
    g_network_transmission.frames_sent = 0;
    g_network_transmission.frames_dropped = 0;
    
    ESP_LOGI(TAG, "Network transmission initialized successfully");
    return ESP_OK;
}

void network_transmission_cleanup(void) {
    if (g_network_transmission.task_handle) {
        vTaskDelete(g_network_transmission.task_handle);
        g_network_transmission.task_handle = NULL;
    }
    
    if (g_network_transmission.message_queue) {
        vQueueDelete(g_network_transmission.message_queue);
        g_network_transmission.message_queue = NULL;
    }
    
    if (g_network_transmission.fragmentation_mutex) {
        vSemaphoreDelete(g_network_transmission.fragmentation_mutex);
        g_network_transmission.fragmentation_mutex = NULL;
    }
    
    cleanup_buffer_pool();
    g_network_transmission.is_initialized = false;
    ESP_LOGI(TAG, "Network transmission cleaned up");
}

bool network_transmission_is_ready(void) {
    return g_network_transmission.is_initialized && g_network_transmission.message_queue != NULL;
}

/* ============================================================================
 * FRAME TRANSMISSION
 * ============================================================================ */

esp_err_t network_queue_frame(const uint8_t *data, size_t len, uint8_t palette_index, int client_fd) {
    if (!g_network_transmission.is_initialized || !data || len == 0 || client_fd < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Check queue status
    UBaseType_t queue_spaces = uxQueueSpacesAvailable(g_network_transmission.message_queue);
    if (queue_spaces == 0) {
        ESP_LOGW(TAG, "Network queue full, dropping frame");
        g_network_transmission.frames_dropped++;
        return ESP_ERR_NO_MEM;
    }
    
    // Create network message
    network_message_t msg = {
        .data = (uint8_t*)data,
        .len = len,
        .palette_index = palette_index,
        .is_delta = false,
        .client_fd = client_fd
    };
    
    BaseType_t result = xQueueSend(g_network_transmission.message_queue, &msg, pdMS_TO_TICKS(1));
    if (result == pdPASS) {
        ESP_LOGD(TAG, "Queued frame: %zu bytes, palette %d, client %d", len, palette_index, client_fd);
    } else {
        ESP_LOGW(TAG, "Failed to queue frame");
        g_network_transmission.frames_dropped++;
    }
    
    return (result == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t network_send_frame_sync(const uint8_t *data, size_t len, uint8_t palette_index, int client_fd) {
    if (!websocket_server_is_ready() || client_fd < 0 || !data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // For small frames, send directly
    if (len <= FRAGMENT_SIZE) {
        // Prepend palette index
        uint8_t *frame_with_palette = network_get_buffer(len + 1);
        if (!frame_with_palette) {
            return ESP_ERR_NO_MEM;
        }
        
        frame_with_palette[0] = palette_index;
        memcpy(frame_with_palette + 1, data, len);
        
        esp_err_t ret = websocket_send_binary_frame(client_fd, frame_with_palette, len + 1);
        network_return_buffer(frame_with_palette);
        
        if (ret == ESP_OK) {
            g_network_transmission.frames_sent++;
        }
        
        return ret;
    }
    
    // For large frames, use fragmentation
    return websocket_send_fragmented_frame(client_fd, data, len, palette_index);
}

/* ============================================================================
 * NETWORK TRANSMISSION TASK
 * ============================================================================ */

void network_transmission_task(void *pvParameters) {
    ESP_LOGI(TAG, "Network transmission task started");
    
    network_message_t msg;
    uint32_t processed_frames = 0;
    uint32_t last_log_time = 0;
    
    while (1) {
        if (xQueueReceive(g_network_transmission.message_queue, &msg, pdMS_TO_TICKS(10)) == pdPASS) {
            processed_frames++;
            
            // Check if we have valid client connections
            if (!websocket_server_is_ready()) {
                ESP_LOGW(TAG, "WebSocket server not ready, skipping frame");
                g_network_transmission.frames_dropped++;
                continue;
            }
            
            // Send frame to all connected clients
            int client_count = websocket_get_client_count();
            if (client_count == 0) {
                ESP_LOGW(TAG, "No WebSocket clients connected, skipping frame");
                g_network_transmission.frames_dropped++;
                continue;
            }
            
            // Send to specific client if specified, otherwise broadcast to all
            if (msg.client_fd >= 0) {
                esp_err_t ret = network_send_frame_sync(msg.data, msg.len, msg.palette_index, msg.client_fd);
                if (ret != ESP_OK) {
                    ESP_LOGW(TAG, "Failed to send frame to client %d: %s", msg.client_fd, esp_err_to_name(ret));
                }
            } else {
                // Broadcast to all clients
                for (int i = 0; i < client_count; i++) {
                    int client_fd = websocket_get_client_fd(i);
                    if (client_fd >= 0) {
                        esp_err_t ret = network_send_frame_sync(msg.data, msg.len, msg.palette_index, client_fd);
                        if (ret != ESP_OK) {
                            ESP_LOGW(TAG, "Failed to send frame to client %d: %s", client_fd, esp_err_to_name(ret));
                        }
                    }
                }
            }
        } else {
            // Log periodically when no frames are being processed
            uint32_t current_time = esp_timer_get_time() / 1000000; // Convert to seconds
            if (current_time - last_log_time > 30) { // Log every 30 seconds
                ESP_LOGI(TAG, "Network task heartbeat: processed %lu frames, clients: %d", 
                         processed_frames, websocket_get_client_count());
                last_log_time = current_time;
            }
        }
    }
}

/* ============================================================================
 * STATISTICS AND MONITORING
 * ============================================================================ */

void network_get_stats(uint32_t *frames_sent, uint32_t *frames_dropped) {
    if (frames_sent) {
        *frames_sent = g_network_transmission.frames_sent;
    }
    if (frames_dropped) {
        *frames_dropped = g_network_transmission.frames_dropped;
    }
}

void network_reset_stats(void) {
    g_network_transmission.frames_sent = 0;
    g_network_transmission.frames_dropped = 0;
    ESP_LOGI(TAG, "Network transmission statistics reset");
} 