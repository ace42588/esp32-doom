#include "delta_encoding.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>

static const char *TAG = "Delta Encoding";

// Pre-allocated delta encoding buffers (avoid dynamic allocation overhead)
static uint8_t *delta_buffer = NULL;
static uint8_t *previous_frame = NULL;
static bool delta_encoding_initialized = false;

// Memory pool for delta compression (avoid malloc/free overhead)
static uint8_t *delta_compression_pool = NULL;
static bool compression_pool_initialized = false;

esp_err_t init_delta_encoding_buffers(void) {
    if (delta_encoding_initialized) {
        return ESP_OK; // Already initialized
    }

    ESP_LOGI(TAG, "Initializing delta encoding buffers");

    // Allocate delta buffer in PSRAM
    delta_buffer = heap_caps_malloc(FRAMEBUFFER_SIZE, MALLOC_CAP_SPIRAM);
    if (!delta_buffer) {
        ESP_LOGE(TAG, "Failed to allocate delta buffer");
        return ESP_ERR_NO_MEM;
    }

    // Allocate previous frame buffer in PSRAM
    previous_frame = heap_caps_malloc(FRAMEBUFFER_SIZE, MALLOC_CAP_SPIRAM);
    if (!previous_frame) {
        // Fallback to regular malloc if PSRAM allocation fails
        previous_frame = malloc(FRAMEBUFFER_SIZE);
        if (!previous_frame) {
            ESP_LOGE(TAG, "Failed to allocate previous frame buffer");
            heap_caps_free(delta_buffer);
            delta_buffer = NULL;
            return ESP_ERR_NO_MEM;
        }
    }

    // Allocate compression pool for delta frames
    delta_compression_pool = heap_caps_malloc(FRAMEBUFFER_SIZE, MALLOC_CAP_SPIRAM);
    if (!delta_compression_pool) {
        // Fallback to regular malloc if PSRAM allocation fails
        delta_compression_pool = malloc(FRAMEBUFFER_SIZE);
        if (!delta_compression_pool) {
            ESP_LOGE(TAG, "Failed to allocate compression pool");
            heap_caps_free(delta_buffer);
            heap_caps_free(previous_frame);
            delta_buffer = NULL;
            previous_frame = NULL;
            return ESP_ERR_NO_MEM;
        }
    }

    delta_encoding_initialized = true;
    compression_pool_initialized = true;
    ESP_LOGI(TAG, "Delta encoding buffers initialized successfully");
    return ESP_OK;
}

void cleanup_delta_encoding_buffers(void) {
    if (delta_buffer) {
        heap_caps_free(delta_buffer);
        delta_buffer = NULL;
    }
    
    if (previous_frame) {
        heap_caps_free(previous_frame);
        previous_frame = NULL;
    }
    
    if (delta_compression_pool) {
        heap_caps_free(delta_compression_pool);
        delta_compression_pool = NULL;
    }
    
    delta_encoding_initialized = false;
    compression_pool_initialized = false;
    ESP_LOGI(TAG, "Delta encoding buffers cleaned up");
}

bool is_delta_encoding_enabled(void) {
    return ENABLE_DELTA_ENCODING == 1;
}

bool is_delta_encoding_initialized(void) {
    return delta_encoding_initialized && delta_buffer != NULL && previous_frame != NULL;
}

uint8_t* get_delta_buffer(void) {
    return delta_buffer;
}

uint8_t* get_previous_frame(void) {
    return previous_frame;
}

void update_previous_frame(const void *data, size_t len) {
    if (previous_frame && data && len <= FRAMEBUFFER_SIZE) {
        memcpy(previous_frame, data, len);
    }
}

size_t create_optimized_delta_frame(const uint8_t *current, size_t len) {
    if (!is_delta_encoding_initialized() || !current) {
        return 0;
    }
    
    size_t delta_size = 0;
    const uint32_t *current_words = (const uint32_t*)current;
    const uint32_t *previous_words = (const uint32_t*)previous_frame;
    const size_t word_count = len / 4;
    
    // Process words (4 bytes at a time) for better performance
    for (size_t i = 0; i < word_count; i++) {
        uint32_t current_word = current_words[i];
        uint32_t previous_word = previous_words[i];
        
        if (current_word != previous_word) {
            // Word has changed - process individual bytes
            size_t base_pos = i * 4;
            for (size_t j = 0; j < 4; j++) {
                uint8_t current_byte = (current_word >> (j * 8)) & 0xFF;
                uint8_t previous_byte = (previous_word >> (j * 8)) & 0xFF;
                
                if (current_byte != previous_byte) {
                    size_t pos = base_pos + j;
                    if (delta_size + 3 <= len) {  // Fixed: +3 for position (2 bytes) + value (1 byte)
                        delta_buffer[delta_size++] = (pos >> 8) & 0xFF; // High byte
                        delta_buffer[delta_size++] = pos & 0xFF;         // Low byte
                        delta_buffer[delta_size++] = current_byte;       // New value
                    }
                }
            }
        }
    }
    
    // Process remaining bytes (if any)
    size_t remaining_start = word_count * 4;
    for (size_t i = remaining_start; i < len; i++) {
        if (current[i] != previous_frame[i]) {
            if (delta_size + 3 <= len) {  // Fixed: +3 for position (2 bytes) + value (1 byte)
                delta_buffer[delta_size++] = (i >> 8) & 0xFF; // High byte
                delta_buffer[delta_size++] = i & 0xFF;         // Low byte
                delta_buffer[delta_size++] = current[i];       // New value
            }
        }
    }
    
    return delta_size;
}

size_t compress_delta_rle(size_t delta_size) {
    if (!compression_pool_initialized || !delta_compression_pool || !delta_buffer) {
        return delta_size;
    }
    
    size_t compressed_size = 0;
    size_t i = 0;
    
    while (i < delta_size) {
        uint8_t current_byte = delta_buffer[i];
        uint8_t count = 1;
        
        // Count consecutive identical bytes
        while (i + count < delta_size && count < 255 && delta_buffer[i + count] == current_byte) {
            count++;
        }
        
        if (count > 3 || current_byte == 0) {
            // Use RLE encoding: 0x00, count, value
            delta_compression_pool[compressed_size++] = 0x00;
            delta_compression_pool[compressed_size++] = count;
            delta_compression_pool[compressed_size++] = current_byte;
        } else {
            // Use literal encoding: raw bytes
            for (uint8_t j = 0; j < count; j++) {
                delta_compression_pool[compressed_size++] = current_byte;
            }
        }
        
        i += count;
    }
    
    // Copy compressed data back to delta buffer
    if (compressed_size < delta_size) {
        memcpy(delta_buffer, delta_compression_pool, compressed_size);
    }
    
    return compressed_size;
} 