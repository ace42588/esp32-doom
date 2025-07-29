#ifndef NETWORK_TRANSMISSION_OPTIMIZATIONS_H
#define NETWORK_TRANSMISSION_OPTIMIZATIONS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

// Compression algorithms
typedef enum {
    COMPRESSION_NONE = 0,
    COMPRESSION_RLE,      // Run-Length Encoding
    COMPRESSION_LZ4,      // LZ4 fast compression
    COMPRESSION_HEATSHRINK, // Heatshrink for embedded systems
    COMPRESSION_ZLIB,     // Zlib (if available)
    COMPRESSION_MAX
} compression_algorithm_t;

// Compression statistics
typedef struct {
    size_t original_size;
    size_t compressed_size;
    float compression_ratio;
    uint32_t compression_time_us;
    compression_algorithm_t algorithm;
} compression_stats_t;

// Compression configuration
typedef struct {
    compression_algorithm_t algorithm;
    bool enable_adaptive;  // Automatically choose best algorithm
    size_t min_size_for_compression;  // Minimum size to attempt compression
    float min_compression_ratio;  // Minimum ratio to use compression
} compression_config_t;

// Function declarations
esp_err_t init_compression_system(void);
void cleanup_compression_system(void);

// Compression functions
esp_err_t compress_frame_data(const uint8_t *input, size_t input_len, 
                             uint8_t *output, size_t *output_len,
                             compression_algorithm_t algorithm,
                             compression_stats_t *stats);

esp_err_t decompress_frame_data(const uint8_t *input, size_t input_len,
                               uint8_t *output, size_t *output_len,
                               compression_algorithm_t algorithm);

// Adaptive compression - automatically choose best algorithm
esp_err_t compress_frame_adaptive(const uint8_t *input, size_t input_len,
                                 uint8_t *output, size_t *output_len,
                                 compression_stats_t *stats);

// Configuration
void set_compression_config(const compression_config_t *config);
void get_compression_config(compression_config_t *config);

// Statistics
void get_compression_stats(compression_stats_t *stats);

#endif // NETWORK_TRANSMISSION_OPTIMIZATIONS_H 