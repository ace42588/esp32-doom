#include "network_transmission.h"
#include "websocket_server.h"
#include "network_transmission_optimizations.h"
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
#include <errno.h> // Required for errno
#include "esp_http_server.h"

static const char *TAG = "Network Transmission";

// Mutex for ensuring single fragmented transmission per client
static SemaphoreHandle_t fragmentation_mutex = NULL;

// Network transmission configuration
#define NETWORK_QUEUE_SIZE 32  // Increased from 16 to handle more frames
#define BUFFER_POOL_SIZE 8
#define MAX_BUFFER_SIZE 16384

typedef struct {
    uint8_t *buffer;
    bool in_use;
    size_t size;
} buffer_pool_entry_t;

static buffer_pool_entry_t buffer_pool[BUFFER_POOL_SIZE] = {0};
static bool buffer_pool_initialized = false;

// Flag to track if optimizations are available
static bool optimizations_available = false;

// Buffer pool management functions
static esp_err_t init_buffer_pool(void) {
    if (buffer_pool_initialized) {
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Initializing buffer pool with %d buffers of %d bytes each", BUFFER_POOL_SIZE, MAX_BUFFER_SIZE);
    
    for (int i = 0; i < BUFFER_POOL_SIZE; i++) {
        // Log memory before each buffer allocation
        size_t free_heap_before = esp_get_free_heap_size();
        size_t free_internal_before = heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
        size_t free_psram_before = esp_psram_is_initialized() ? heap_caps_get_free_size(MALLOC_CAP_SPIRAM) : 0;
        
        // Try to allocate from PSRAM first, fallback to internal RAM
        if (esp_psram_is_initialized()) {
            buffer_pool[i].buffer = heap_caps_malloc(MAX_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        } else {
            buffer_pool[i].buffer = malloc(MAX_BUFFER_SIZE);
        }
        
        if (!buffer_pool[i].buffer) {
            ESP_LOGE(TAG, "Failed to allocate buffer %d in pool (%d bytes). Memory - Heap: %zu, Internal: %zu, PSRAM: %zu", 
                     i, MAX_BUFFER_SIZE, free_heap_before, free_internal_before, free_psram_before);
            // Clean up already allocated buffers
            for (int j = 0; j < i; j++) {
                if (esp_psram_is_initialized()) {
                    heap_caps_free(buffer_pool[j].buffer);
                } else {
                    free(buffer_pool[j].buffer);
                }
                buffer_pool[j].buffer = NULL;
            }
            return ESP_ERR_NO_MEM;
        }
        buffer_pool[i].in_use = false;
        buffer_pool[i].size = MAX_BUFFER_SIZE;
    }
    
    buffer_pool_initialized = true;
    ESP_LOGI(TAG, "Buffer pool initialized with %d buffers of size %d", BUFFER_POOL_SIZE, MAX_BUFFER_SIZE);
    return ESP_OK;
}

static void cleanup_buffer_pool(void) {
    for (int i = 0; i < BUFFER_POOL_SIZE; i++) {
        if (buffer_pool[i].buffer) {
            if (esp_psram_is_initialized()) {
                heap_caps_free(buffer_pool[i].buffer);
            } else {
                free(buffer_pool[i].buffer);
            }
            buffer_pool[i].buffer = NULL;
        }
        buffer_pool[i].in_use = false;
    }
    buffer_pool_initialized = false;
    ESP_LOGI(TAG, "Buffer pool cleaned up");
}

static uint8_t* get_buffer_from_pool(size_t required_size) {
    if (!buffer_pool_initialized) {
        // Buffer pool not available, use direct allocation
        if (esp_psram_is_initialized()) {
            return heap_caps_malloc(required_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        } else {
            return malloc(required_size);
        }
    }
    
    if (required_size > MAX_BUFFER_SIZE) {
        // Requested size too large for pool, use direct allocation
        if (esp_psram_is_initialized()) {
            return heap_caps_malloc(required_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        } else {
            return malloc(required_size);
        }
    }
    
    for (int i = 0; i < BUFFER_POOL_SIZE; i++) {
        if (!buffer_pool[i].in_use) {
            buffer_pool[i].in_use = true;
            return buffer_pool[i].buffer;
        }
    }
    
    // No available buffer in pool, fall back to malloc
    ESP_LOGW(TAG, "Buffer pool exhausted, falling back to malloc");
    if (esp_psram_is_initialized()) {
        return heap_caps_malloc(required_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    } else {
        return malloc(required_size);
    }
}

static void return_buffer_to_pool(uint8_t *buffer) {
    if (!buffer_pool_initialized) {
        if (esp_psram_is_initialized()) {
            heap_caps_free(buffer);
        } else {
            free(buffer);
        }
        return;
    }
    
    for (int i = 0; i < BUFFER_POOL_SIZE; i++) {
        if (buffer_pool[i].buffer == buffer) {
            buffer_pool[i].in_use = false;
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


// Asynchronous network transmission
static QueueHandle_t network_queue = NULL;
static TaskHandle_t network_task_handle = NULL;
static bool async_network_initialized = false;
static uint32_t frames_skipped = 0;  // Track skipped frames for monitoring
static uint32_t consecutive_skips = 0;  // Track consecutive frame skips

// Set socket timeout for WebSocket transmission with error handling
struct timeval timeout = {
    .tv_sec = 0,
    .tv_usec = 50000  // 50ms timeout for faster transmission
};

#define QUEUE_SIZE 32  // Reduced from 128 to prevent memory issues
#define MAX_CONSECUTIVE_SKIPS 5  // Allow more consecutive skips before forcing transmission

esp_err_t init_async_network_transmission(void) {
    if (async_network_initialized) {
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Initializing asynchronous network transmission");
    
    // Log memory before network init
    size_t free_heap_before = esp_get_free_heap_size();
    size_t free_internal_ram_before = heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    size_t free_psram_before = esp_psram_is_initialized() ? heap_caps_get_free_size(MALLOC_CAP_SPIRAM) : 0;
    ESP_LOGI(TAG, "Memory before network init - Internal RAM: %zu, PSRAM: %zu", free_internal_ram_before, free_psram_before);
    
    // Initialize compression system
    esp_err_t comp_ret = init_compression_system();
    if (comp_ret != ESP_OK) {
        ESP_LOGW(TAG, "Compression system initialization failed, continuing without compression");
    } else {
        ESP_LOGI(TAG, "Compression system initialized successfully");
    }
    
    // Initialize buffer pool
    esp_err_t pool_ret = init_buffer_pool();
    if (pool_ret != ESP_OK) {
        ESP_LOGW(TAG, "Buffer pool initialization failed, continuing without pool");
        // Continue without buffer pool - will use direct allocation
    }
    
    // Log memory after buffer pool
    size_t free_heap_after_pool = esp_get_free_heap_size();
    size_t free_internal_ram_after_pool = heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    size_t free_psram_after_pool = esp_psram_is_initialized() ? heap_caps_get_free_size(MALLOC_CAP_SPIRAM) : 0;
    ESP_LOGI(TAG, "Memory after buffer pool - Internal RAM: %zu (used: %zu), PSRAM: %zu (used: %zu)", 
             free_internal_ram_after_pool, free_internal_ram_before - free_internal_ram_after_pool,
             free_psram_after_pool, free_psram_before - free_psram_after_pool);
    
    // Create fragmentation mutex
    fragmentation_mutex = xSemaphoreCreateMutex();
    if (!fragmentation_mutex) {
        ESP_LOGE(TAG, "Failed to create fragmentation mutex");
        cleanup_buffer_pool();
        return ESP_ERR_NO_MEM;
    }
    
    // Log memory after mutex
    size_t free_heap_after_mutex = esp_get_free_heap_size();
    size_t free_internal_ram_after_mutex = heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "Memory after mutex - Internal RAM: %zu (used: %zu)", 
             free_internal_ram_after_mutex, free_internal_ram_after_pool - free_internal_ram_after_mutex);
    
    // Create queue for network messages
    network_queue = xQueueCreate(NETWORK_QUEUE_SIZE, sizeof(network_message_t));
    if (!network_queue) {
        ESP_LOGE(TAG, "Failed to create network queue");
        cleanup_buffer_pool();
        vSemaphoreDelete(fragmentation_mutex);
        fragmentation_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }
    
    // Log memory after queue
    size_t free_heap_after_queue = esp_get_free_heap_size();
    size_t free_internal_ram_after_queue = heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "Memory after queue - Internal RAM: %zu (used: %zu)", 
             free_internal_ram_after_queue, free_internal_ram_after_mutex - free_internal_ram_after_queue);
    
    // Create network transmission task with adequate stack size
    BaseType_t task_created = xTaskCreatePinnedToCore(
        network_transmission_task,
        "network_tx",
        4096,  // Restored to 4096 to prevent stack overflow
        NULL,
        5,
        &network_task_handle,
        0  // Pin to Core 0 (WiFi core)
    );
    
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create network transmission task");
        vQueueDelete(network_queue);
        network_queue = NULL;
        vSemaphoreDelete(fragmentation_mutex);
        fragmentation_mutex = NULL;
        cleanup_buffer_pool();
        return ESP_ERR_NO_MEM;
    }
    
    // Log memory after task creation
    size_t free_heap_after_task = esp_get_free_heap_size();
    size_t free_internal_ram_after_task = heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "Memory after task - Internal RAM: %zu (used: %zu)", 
             free_internal_ram_after_task, free_internal_ram_after_queue - free_internal_ram_after_task);
    
    async_network_initialized = true;
    ESP_LOGI(TAG, "Asynchronous network transmission initialized successfully");
    ESP_LOGI(TAG, "Total internal RAM used: %zu bytes, PSRAM used: %zu bytes", 
             free_internal_ram_before - free_internal_ram_after_task,
             free_psram_before - free_psram_after_pool);
    return ESP_OK;
}

void cleanup_async_network_transmission(void) {
    if (network_task_handle) {
        vTaskDelete(network_task_handle);
        network_task_handle = NULL;
    }
    
    if (network_queue) {
        vQueueDelete(network_queue);
        network_queue = NULL;
    }
    
    if (fragmentation_mutex) {
        vSemaphoreDelete(fragmentation_mutex);
        fragmentation_mutex = NULL;
    }
    
    cleanup_buffer_pool();
    async_network_initialized = false;
    ESP_LOGI(TAG, "Asynchronous network transmission cleaned up");
}

bool is_network_transmission_initialized(void) {
    return async_network_initialized && network_queue != NULL;
}

// Function to set optimization availability
void set_optimizations_available(bool available) {
    optimizations_available = available;
    ESP_LOGI(TAG, "Network optimizations %s", available ? "enabled" : "disabled");
}

BaseType_t queue_network_message(network_message_t *msg) {
    // Check queue status first
    UBaseType_t queue_spaces = uxQueueSpacesAvailable(network_queue);
    UBaseType_t queue_messages = uxQueueMessagesWaiting(network_queue);
    
    if (queue_spaces == 0) {
        // Queue is full, wait a bit for the task to process some frames
        ESP_LOGW(TAG, "Network queue full (%lu messages), waiting for processing...", queue_messages);
        vTaskDelay(pdMS_TO_TICKS(10)); // Wait 10ms for task to process
        
        // Check again after waiting
        queue_spaces = uxQueueSpacesAvailable(network_queue);
        if (queue_spaces == 0) {
            // Still full, clear it to prevent blocking
            ESP_LOGW(TAG, "Queue still full after waiting, clearing to prevent blocking");
            xQueueReset(network_queue);
        }
    }
    
    // Re-enable basic network transmission for testing
    BaseType_t result = xQueueSend(network_queue, msg, pdMS_TO_TICKS(1));
    
    if (result == pdPASS) {
        ESP_LOGD(TAG, "Queued frame: %zu bytes, palette %d (queue: %lu/%lu)", 
                 msg->len, msg->palette_index, queue_messages + 1, NETWORK_QUEUE_SIZE);
    } else {
        static uint32_t dropped_count = 0;
        dropped_count++;
        if (dropped_count % 100 == 0) {
            ESP_LOGW(TAG, "Network queue full, dropped %lu frames", dropped_count);
        }
    }
    
    return result;
}

void network_transmission_task(void *pvParameters) {
    ESP_LOGI(TAG, "Network transmission task started");
    
    network_message_t msg;
    uint32_t processed_frames = 0;
    uint32_t last_log_time = 0;
    uint32_t heartbeat_counter = 0;
    
    while (1) {
        heartbeat_counter++;
        
        if (xQueueReceive(network_queue, &msg, pdMS_TO_TICKS(10)) == pdPASS) { // Reduced from 100ms to 10ms
            processed_frames++;
            
            // Check if we have valid client connections
            if (ws_client_count == 0) {
                ESP_LOGW(TAG, "No WebSocket clients connected, skipping frame transmission (processed: %lu)", processed_frames);
                continue;
            }
            
            ESP_LOGI(TAG, "Processing frame: %zu bytes, palette %d, clients: %d (processed: %lu)", 
                     msg.len, msg.palette_index, ws_client_count, processed_frames);
            
            // Use optimized transmission with buffer pool and PSRAM
            esp_err_t ret = send_zero_copy_websocket_frame(g_server_handle, ws_client_fds[0], 
                                                          msg.data, msg.len, msg.palette_index);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to send optimized frame: %s", esp_err_to_name(ret));
                
                // Log memory status for debugging
                size_t free_heap = esp_get_free_heap_size();
                size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
                size_t free_psram = esp_psram_is_initialized() ? heap_caps_get_free_size(MALLOC_CAP_SPIRAM) : 0;
                ESP_LOGW(TAG, "Memory status - Heap: %zu, Internal: %zu, PSRAM: %zu", 
                         free_heap, free_internal, free_psram);
            } else {
                ESP_LOGI(TAG, "Successfully sent frame to client");
            }
        } else {
            // Log periodically when no frames are being processed
            uint32_t current_time = esp_timer_get_time() / 1000000; // Convert to seconds
            if (current_time - last_log_time > 10) { // Log every 10 seconds
                ESP_LOGI(TAG, "Network task heartbeat: processed %lu frames, clients: %d, heartbeat: %lu", 
                         processed_frames, ws_client_count, heartbeat_counter);
                last_log_time = current_time;
            }
        }
    }
}

esp_err_t send_optimized_websocket_frame(httpd_handle_t server_handle, int client_fd, httpd_ws_frame_t *ws_pkt) {
    
    // Validate client connection before sending
    if (client_fd < 0) {
        ESP_LOGE(TAG, "Invalid client file descriptor: %d", client_fd);
        return ESP_ERR_INVALID_ARG;
    }
    
    int sock_ret = setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    if (sock_ret != 0) {
        ESP_LOGW(TAG, "Failed to set socket timeout for client %d: %s", client_fd, strerror(errno));
        // Continue anyway, but log the issue
    }
    
    // Optimize socket for faster transmission
    int tcp_nodelay = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &tcp_nodelay, sizeof(tcp_nodelay));
    
    // Send frame with optimized parameters
    esp_err_t ret = httpd_ws_send_frame_async(server_handle, client_fd, ws_pkt);
    
    if (ret != ESP_OK) {
        // Enhanced error logging with more context
        ESP_LOGE(TAG, "Failed to send optimized WebSocket frame to client %d: %s (errno: %d)", 
                 client_fd, esp_err_to_name(ret), errno);
        
        // Check for specific socket errors that indicate disconnection
        if (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN || errno == 128) {
            ESP_LOGW(TAG, "Socket error indicates client %d has disconnected (errno: %d)", client_fd, errno);
            return ESP_ERR_INVALID_ARG; // Signal disconnection
        }
    }
    
    return ret;
}

esp_err_t send_zero_copy_websocket_frame(httpd_handle_t server_handle, int client_fd, 
                                        const uint8_t *data, size_t total_len, uint8_t palette_index) {
    if (server_handle == NULL || client_fd < 0 || data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Log frame size for debugging
    if (total_len > 10000) {
        ESP_LOGI(TAG, "Sending large frame: %zu bytes, palette %d", total_len, palette_index);
    }
    
    // Try compression first
    uint8_t *compressed_data = NULL;
    size_t compressed_len = 0;
    compression_stats_t comp_stats = {0};
    bool compression_used = false;
    
    // Only attempt compression for frames larger than minimum size
    if (total_len > 512) {  // Lowered from 1KB to 512 bytes for more aggressive compression
        // Log memory before compression attempt
        size_t free_heap_before = esp_get_free_heap_size();
        size_t free_internal_before = heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
        size_t free_psram_before = esp_psram_is_initialized() ? heap_caps_get_free_size(MALLOC_CAP_SPIRAM) : 0;
        
        // Allocate buffer for compressed data (worst case: same size as original)
        compressed_data = get_buffer_from_pool(total_len + 1);  // +1 for palette index
        if (compressed_data) {
            compressed_len = total_len + 1;  // Start with worst case size
            
            // Try adaptive compression
            esp_err_t comp_ret = compress_frame_adaptive(data, total_len, 
                                                       compressed_data + 1, &compressed_len, &comp_stats);
            if (comp_ret == ESP_OK && comp_stats.compression_ratio < 0.95f) {  // At least 5% reduction (lowered from 10%)
                // Add palette index to compressed data
                compressed_data[0] = palette_index;
                compressed_len++;  // Include palette index in total size
                compression_used = true;
                
                ESP_LOGD(TAG, "Compression successful: %zu → %zu bytes (%.1f%%) using %s", 
                         total_len, compressed_len - 1, comp_stats.compression_ratio * 100,
                         comp_stats.algorithm == COMPRESSION_RLE ? "RLE" :
                         comp_stats.algorithm == COMPRESSION_LZ4 ? "LZ4" :
                         comp_stats.algorithm == COMPRESSION_HEATSHRINK ? "Heatshrink" : "Unknown");
            } else {
                // Compression not beneficial, return buffer
                return_buffer_to_pool(compressed_data);
                compressed_data = NULL;
            }
        } else {
            ESP_LOGW(TAG, "Failed to allocate compression buffer (%zu bytes). Memory - Heap: %zu, Internal: %zu, PSRAM: %zu", 
                     total_len + 1, free_heap_before, free_internal_before, free_psram_before);
        }
    }
    
    // Use compressed data if available, otherwise use original
    const uint8_t *frame_data = compression_used ? compressed_data : data;
    size_t frame_len = compression_used ? compressed_len : total_len + 1;
    uint8_t actual_palette_index = compression_used ? compressed_data[0] : palette_index;
    
    // For large frames (>16KB), use fragmentation if compression failed
    if (frame_len > MAX_BUFFER_SIZE) {
        // If compression failed or wasn't attempted, use fragmentation
        if (!compression_used) {
            ESP_LOGI(TAG, "Large frame (%zu bytes) without compression, using fragmentation", frame_len);
            return send_fragmented_websocket_frame(server_handle, client_fd, data, total_len, palette_index);
        }
        
        // Log memory before large frame allocation
        size_t free_heap_before = esp_get_free_heap_size();
        size_t free_internal_before = heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
        size_t free_psram_before = esp_psram_is_initialized() ? heap_caps_get_free_size(MALLOC_CAP_SPIRAM) : 0;
        
        // Allocate large buffer directly from PSRAM
        uint8_t *large_buffer = NULL;
        if (esp_psram_is_initialized()) {
            large_buffer = heap_caps_malloc(frame_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            ESP_LOGD(TAG, "Allocated large frame buffer (%zu bytes) from PSRAM", frame_len);
        } else {
            large_buffer = malloc(frame_len);
            ESP_LOGD(TAG, "Allocated large frame buffer (%zu bytes) from internal RAM", frame_len);
        }
        
        if (!large_buffer) {
            ESP_LOGE(TAG, "Failed to allocate large frame buffer of size %zu. Memory - Heap: %zu, Internal: %zu, PSRAM: %zu", 
                     frame_len, free_heap_before, free_internal_before, free_psram_before);
            if (compressed_data) {
                return_buffer_to_pool(compressed_data);
            }
            // Fall back to fragmentation
            ESP_LOGW(TAG, "Falling back to fragmentation for large frame");
            return send_fragmented_websocket_frame(server_handle, client_fd, data, total_len, palette_index);
        }
        
        // Copy frame data to large buffer
        memcpy(large_buffer, frame_data, frame_len);
        
        // Create WebSocket frame
        httpd_ws_frame_t ws_pkt;
        memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
        ws_pkt.type = HTTPD_WS_TYPE_BINARY;
        ws_pkt.final = 1;  // Set FIN bit for complete message
        ws_pkt.fragmented = 0;  // Not fragmented for single frames
        ws_pkt.payload = large_buffer;
        ws_pkt.len = frame_len;
        
        ESP_LOGI(TAG, "WebSocket frame: type=%d, final=%d, fragmented=%d, len=%zu, payload=%p", 
                 ws_pkt.type, ws_pkt.final, ws_pkt.fragmented, ws_pkt.len, ws_pkt.payload);
        
        // Send the frame
        esp_err_t ret = send_optimized_websocket_frame(server_handle, client_fd, &ws_pkt);
        
        // Free the large buffer
        if (esp_psram_is_initialized()) {
            heap_caps_free(large_buffer);
        } else {
            free(large_buffer);
        }
        
        // Return compressed buffer to pool if used
        if (compressed_data) {
            return_buffer_to_pool(compressed_data);
        }
        
        if (ret == ESP_OK) {
            if (compression_used) {
                ESP_LOGI(TAG, "Successfully sent compressed large frame: %zu → %zu bytes with palette %d", 
                         total_len, frame_len, actual_palette_index);
            } else {
                ESP_LOGI(TAG, "Successfully sent large frame: %zu bytes with palette %d", total_len, palette_index);
            }
        } else {
            ESP_LOGE(TAG, "Failed to send large frame: %zu bytes with palette %d, error: %s", 
                     total_len, palette_index, esp_err_to_name(ret));
        }
        
        return ret;
    }
    
    // For smaller frames, use buffer pool for palette index prepending (if not already compressed)
    uint8_t *frame_with_palette = NULL;
    if (!compression_used) {
        frame_with_palette = get_buffer_from_pool(total_len + 1);
        if (!frame_with_palette) {
            ESP_LOGE(TAG, "Failed to get frame buffer from pool for %zu bytes", total_len);
            if (compressed_data) {
                return_buffer_to_pool(compressed_data);
            }
            return ESP_ERR_NO_MEM;
        }
        
        // Place palette index at the beginning, then copy data
        frame_with_palette[0] = palette_index;
        memcpy(frame_with_palette + 1, data, total_len);
        frame_data = frame_with_palette;
        frame_len = total_len + 1;
    }
    
    // Create WebSocket frame
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_BINARY;
    ws_pkt.final = 1;  // Set FIN bit for complete message
    ws_pkt.fragmented = 0;  // Not fragmented for single frames
    ws_pkt.payload = (uint8_t*)frame_data;
    ws_pkt.len = frame_len;
    
    // Send the frame
    esp_err_t ret = send_optimized_websocket_frame(server_handle, client_fd, &ws_pkt);
    
    // Return buffers to pool after sending
    if (frame_with_palette) {
        return_buffer_to_pool(frame_with_palette);
    }
    if (compressed_data) {
        return_buffer_to_pool(compressed_data);
    }
    
    if (ret == ESP_OK) {
        if (compression_used) {
            ESP_LOGD(TAG, "Successfully sent compressed frame: %zu → %zu bytes with palette %d", 
                     total_len, frame_len, actual_palette_index);
        } else {
            ESP_LOGD(TAG, "Successfully sent frame: %zu bytes with palette %d", total_len, palette_index);
        }
    } else {
        ESP_LOGE(TAG, "Failed to send frame: %zu bytes with palette %d, error: %s", 
                 total_len, palette_index, esp_err_to_name(ret));
    }
    
    return ret;
}

esp_err_t send_fragmented_websocket_frame(httpd_handle_t server_handle, int client_fd, 
                                         const uint8_t *data, size_t total_len, uint8_t palette_index) {
    if (server_handle == NULL || client_fd < 0 || data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Acquire fragmentation mutex to ensure single fragmented transmission
    if (fragmentation_mutex && xSemaphoreTake(fragmentation_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire fragmentation mutex, dropping frame");
        return ESP_ERR_TIMEOUT;
    }
    
    ESP_LOGD(TAG, "Acquired fragmentation mutex for client %d", client_fd);
    
    // Send large frames using proper WebSocket fragmentation
    size_t offset = 0;
    size_t chunk_size = MAX_BUFFER_SIZE - 1;  // Leave room for palette index in first chunk
    bool is_first_chunk = true;
    bool is_last_chunk = false;
    int chunk_count = 0;
    
    ESP_LOGI(TAG, "Starting fragmented transmission: %zu bytes, palette %d", total_len, palette_index);
    
    // Calculate total number of chunks for debugging
    size_t total_chunks = (total_len + chunk_size - 1) / chunk_size;
    ESP_LOGI(TAG, "Will send %zu chunks of size %zu", total_chunks, chunk_size);
    
    while (offset < total_len) {
        // Calculate chunk size (last chunk may be smaller)
        size_t current_chunk_size = (offset + chunk_size > total_len) ? 
                                   (total_len - offset) : chunk_size;
        
        // Check if this is the last chunk
        is_last_chunk = (offset + current_chunk_size >= total_len);
        chunk_count++;
        
        // Get buffer from pool for this chunk
        uint8_t *chunk_buffer = get_buffer_from_pool(current_chunk_size + (is_first_chunk ? 1 : 0));
        if (!chunk_buffer) {
            ESP_LOGE(TAG, "Failed to get chunk buffer from pool");
            if (fragmentation_mutex) {
                xSemaphoreGive(fragmentation_mutex);
            }
            return ESP_ERR_NO_MEM;
        }
        
        // Create WebSocket frame
        httpd_ws_frame_t ws_pkt;
        memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
        ws_pkt.type = is_first_chunk ? HTTPD_WS_TYPE_BINARY : HTTPD_WS_TYPE_CONTINUE;
        ws_pkt.final = is_last_chunk ? 1 : 0;  // Set FIN bit only on last chunk
        ws_pkt.fragmented = 1;  // Mark as fragmented according to ESP-IDF docs
        ws_pkt.payload = chunk_buffer;
        
        // Validate frame parameters
        if (ws_pkt.payload == NULL || ws_pkt.len == 0) {
            ESP_LOGE(TAG, "Invalid WebSocket frame parameters: payload=%p, len=%zu", 
                     ws_pkt.payload, ws_pkt.len);
            return_buffer_to_pool(chunk_buffer);
            if (fragmentation_mutex) {
                xSemaphoreGive(fragmentation_mutex);
            }
            return ESP_ERR_INVALID_ARG;
        }
        
        if (is_first_chunk) {
            // First chunk: add palette index at the beginning
            chunk_buffer[0] = palette_index;
            memcpy(chunk_buffer + 1, data + offset, current_chunk_size);
            ws_pkt.len = current_chunk_size + 1;  // data + palette index
            
            ESP_LOGD(TAG, "Sending first chunk %d: %zu bytes, palette %d, FIN=%d, fragmented=%d", 
                     chunk_count, ws_pkt.len, palette_index, ws_pkt.final, ws_pkt.fragmented);
            is_first_chunk = false;
        } else {
            // Subsequent chunks: send data directly (no palette index)
            memcpy(chunk_buffer, data + offset, current_chunk_size);
            ws_pkt.len = current_chunk_size;
            
            ESP_LOGD(TAG, "Sending chunk %d: %zu bytes, FIN=%d, fragmented=%d", 
                     chunk_count, ws_pkt.len, ws_pkt.final, ws_pkt.fragmented);
        }
        
        // Send the chunk
        esp_err_t ret = send_optimized_websocket_frame(server_handle, client_fd, &ws_pkt);
        return_buffer_to_pool(chunk_buffer);
        
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send chunk %d at offset %zu: %s", 
                     chunk_count, offset, esp_err_to_name(ret));
            if (fragmentation_mutex) {
                xSemaphoreGive(fragmentation_mutex);
            }
            return ret;
        }
        
        // Add a longer delay between chunks to ensure proper sequencing
        if (!is_last_chunk) {
            vTaskDelay(pdMS_TO_TICKS(5));  // 5ms delay between chunks for better reliability
        }
        
        offset += current_chunk_size;
    }
    
    // Release fragmentation mutex
    if (fragmentation_mutex) {
        xSemaphoreGive(fragmentation_mutex);
        ESP_LOGD(TAG, "Released fragmentation mutex for client %d", client_fd);
    }
    
    ESP_LOGI(TAG, "Completed fragmented transmission: %zu bytes in %d chunks", total_len, chunk_count);
    return ESP_OK;
} 