#ifndef DELTA_ENCODING_H
#define DELTA_ENCODING_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Delta encoding configuration
#define FRAMEBUFFER_SIZE (320 * 240)  // 76,800 bytes

#ifndef ENABLE_DELTA_ENCODING
#define ENABLE_DELTA_ENCODING 0 // disabled for FPS performance
#endif

/**
 * @brief Initialize delta encoding buffers (pre-allocated to avoid overhead)
 * @return ESP_OK on success, ESP_ERR_* on failure
 */
esp_err_t init_delta_encoding_buffers(void);

/**
 * @brief Clean up delta encoding buffers
 */
void cleanup_delta_encoding_buffers(void);

/**
 * @brief Check if delta encoding is enabled at compile time
 * @return true if enabled, false otherwise
 */
bool is_delta_encoding_enabled(void);

/**
 * @brief Check if delta encoding is initialized
 * @return true if initialized, false otherwise
 */
bool is_delta_encoding_initialized(void);

/**
 * @brief Get the delta buffer
 * @return Pointer to delta buffer
 */
uint8_t* get_delta_buffer(void);

/**
 * @brief Get the previous frame buffer
 * @return Pointer to previous frame buffer
 */
uint8_t* get_previous_frame(void);

/**
 * @brief Update the previous frame with new data
 * @param data New frame data
 * @param len Length of frame data
 */
void update_previous_frame(const void *data, size_t len);

/**
 * @brief Create optimized delta frame with word-aligned comparisons
 * @param current Current frame data
 * @param len Length of frame data
 * @return Size of delta data in bytes
 */
size_t create_optimized_delta_frame(const uint8_t *current, size_t len);

/**
 * @brief RLE compress delta frame data
 * @param delta_size Input delta size
 * @return Size of compressed data
 */
size_t compress_delta_rle(size_t delta_size);

#ifdef __cplusplus
}
#endif

#endif // DELTA_ENCODING_H 