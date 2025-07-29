#include "network_transmission_optimizations.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "Network Optimizations";

// Compression configuration
static compression_config_t compression_config = {
    .algorithm = COMPRESSION_RLE,
    .enable_adaptive = true,
    .min_size_for_compression = 1024,  // 1KB minimum
    .min_compression_ratio = 0.8f       // Must achieve at least 20% reduction
};

// Compression buffers
static uint8_t *compression_buffer = NULL;
static size_t compression_buffer_size = 0;
static bool compression_initialized = false;

// Statistics
static compression_stats_t global_stats = {0};

// Heatshrink configuration (if available)
#define HEATSHRINK_WINDOW_BITS 8
#define HEATSHRINK_LOOKAHEAD_BITS 4

// LZ4 configuration
#define LZ4_ACCELERATION 1

esp_err_t init_compression_system(void) {
    if (compression_initialized) {
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Initializing compression system");
    
    // Allocate compression buffer (large enough for full framebuffer + overhead)
    compression_buffer_size = 102400;  // 100KB to handle full framebuffer + compression overhead
    compression_buffer = heap_caps_malloc(compression_buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!compression_buffer) {
        ESP_LOGW(TAG, "Failed to allocate compression buffer in PSRAM, trying internal RAM");
        compression_buffer = malloc(compression_buffer_size);
        if (!compression_buffer) {
            ESP_LOGE(TAG, "Failed to allocate compression buffer");
            return ESP_ERR_NO_MEM;
        }
    }
    
    compression_initialized = true;
    ESP_LOGI(TAG, "Compression system initialized with %zu byte buffer in PSRAM", compression_buffer_size);
    return ESP_OK;
}

void cleanup_compression_system(void) {
    if (compression_buffer) {
        if (heap_caps_check_integrity_all(true)) {
            heap_caps_free(compression_buffer);
        } else {
            free(compression_buffer);
        }
        compression_buffer = NULL;
    }
    compression_initialized = false;
    ESP_LOGI(TAG, "Compression system cleaned up");
}

// LZ4-like compression optimized for DOOM framebuffers
static esp_err_t compress_lz4_optimized(const uint8_t *input, size_t input_len,
                                       uint8_t *output, size_t *output_len) {
    if (!input || !output || !output_len) {
        return ESP_ERR_INVALID_ARG;
    }
    
    size_t out_pos = 0;
    size_t in_pos = 0;
    
    // Use 8KB sliding window for better compression of DOOM textures
    const size_t window_size = 8192;
    
    while (in_pos < input_len) {
        // Find longest match in previous data
        size_t best_match_len = 0;
        size_t best_match_pos = 0;
        
        // Search in sliding window
        size_t search_start = (in_pos > window_size) ? in_pos - window_size : 0;
        for (size_t i = search_start; i < in_pos; i++) {
            size_t match_len = 0;
            
            // Find match length
            while (in_pos + match_len < input_len && 
                   i + match_len < in_pos &&
                   match_len < 255 &&
                   input[i + match_len] == input[in_pos + match_len]) {
                match_len++;
            }
            
            // Require longer matches for framebuffer data (at least 6 bytes)
            if (match_len > best_match_len && match_len >= 6) {
                best_match_len = match_len;
                best_match_pos = i;
            }
        }
        
        if (best_match_len >= 6) {
            // Encode match
            if (out_pos + 3 >= *output_len) {
                return ESP_ERR_NO_MEM;
            }
            
            // LZ4-style encoding: [match_len][offset_high][offset_low]
            output[out_pos++] = 0x80 | best_match_len;  // Match marker with length
            output[out_pos++] = (best_match_pos >> 8) & 0xFF;  // Offset high byte
            output[out_pos++] = best_match_pos & 0xFF;  // Offset low byte
            
            in_pos += best_match_len;
        } else {
            // Encode literal
            if (out_pos + 1 >= *output_len) {
                return ESP_ERR_NO_MEM;
            }
            output[out_pos++] = input[in_pos++];
        }
    }
    
    *output_len = out_pos;
    return ESP_OK;
}

esp_err_t compress_frame_data(const uint8_t *input, size_t input_len, 
                             uint8_t *output, size_t *output_len,
                             compression_algorithm_t algorithm,
                             compression_stats_t *stats) {
    if (!input || !output || !output_len) {
        return ESP_ERR_INVALID_ARG;
    }
    
    uint64_t start_time = esp_timer_get_time();
    esp_err_t ret = ESP_OK;
    
    switch (algorithm) {
        case COMPRESSION_LZ4:
            ret = compress_lz4_optimized(input, input_len, output, output_len);
            break;
            
        case COMPRESSION_NONE:
        default:
            if (*output_len < input_len) {
                return ESP_ERR_NO_MEM;
            }
            memcpy(output, input, input_len);
            *output_len = input_len;
            break;
    }
    
    if (ret == ESP_OK && stats) {
        stats->original_size = input_len;
        stats->compressed_size = *output_len;
        stats->compression_ratio = (float)*output_len / input_len;
        stats->compression_time_us = (uint32_t)(esp_timer_get_time() - start_time);
        stats->algorithm = algorithm;
        
        // Update global stats
        global_stats = *stats;
    }
    
    return ret;
}

esp_err_t compress_frame_adaptive(const uint8_t *input, size_t input_len, 
                                 uint8_t *output, size_t *output_len, 
                                 compression_stats_t *stats) {
    if (!compression_initialized || !input || !output || !output_len || !stats) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Check if input is too large for our buffer
    if (input_len > compression_buffer_size) {
        ESP_LOGW(TAG, "Input too large for compression buffer (%zu > %zu), skipping compression", 
                 input_len, compression_buffer_size);
        return ESP_ERR_INVALID_SIZE;
    }
    
    ESP_LOGD(TAG, "Attempting LZ4-like compression for %zu bytes", input_len);
    
    // Try LZ4-like compression (optimized for DOOM framebuffers)
    size_t lz4_len = compression_buffer_size;
    esp_err_t lz4_ret = compress_lz4_optimized(input, input_len, compression_buffer, &lz4_len);
    if (lz4_ret == ESP_OK && lz4_len < input_len) {
        compression_stats_t lz4_stats = {
            .original_size = input_len,
            .compressed_size = lz4_len,
            .compression_ratio = (float)lz4_len / input_len,
            .algorithm = COMPRESSION_LZ4,
            .compression_time_us = 0
        };
        
        // Check if compression is beneficial (at least 5% reduction)
        if (lz4_stats.compression_ratio < 0.95f) {
            *output_len = lz4_len;
            *stats = lz4_stats;
            memcpy(output, compression_buffer, lz4_len);
            
            ESP_LOGI(TAG, "LZ4 compression: %zu → %zu bytes (%.1f%%)", 
                     input_len, lz4_len, lz4_stats.compression_ratio * 100);
            
            return ESP_OK;
        } else {
            ESP_LOGD(TAG, "LZ4 compression not beneficial: %zu → %zu bytes (%.1f%%)", 
                     input_len, lz4_len, lz4_stats.compression_ratio * 100);
        }
    }
    
    // No beneficial compression found
    ESP_LOGD(TAG, "No beneficial compression found for %zu bytes", input_len);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t decompress_frame_data(const uint8_t *input, size_t input_len,
                               uint8_t *output, size_t *output_len,
                               compression_algorithm_t algorithm) {
    // Decompression implementation would go here
    // For now, just copy data (no compression in current implementation)
    if (*output_len < input_len) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(output, input, input_len);
    *output_len = input_len;
    return ESP_OK;
}

void set_compression_config(const compression_config_t *config) {
    if (config) {
        compression_config = *config;
        ESP_LOGI(TAG, "Compression config updated: algorithm=%d, adaptive=%d, min_size=%zu, min_ratio=%.2f",
                 config->algorithm, config->enable_adaptive, config->min_size_for_compression, config->min_compression_ratio);
    }
}

void get_compression_config(compression_config_t *config) {
    if (config) {
        *config = compression_config;
    }
}

void get_compression_stats(compression_stats_t *stats) {
    if (stats) {
        *stats = global_stats;
    }
} 