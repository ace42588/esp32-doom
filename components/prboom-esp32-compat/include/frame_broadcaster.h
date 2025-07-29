#ifndef FRAME_BROADCASTER_H
#define FRAME_BROADCASTER_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Broadcast a framebuffer to all connected WebSocket clients
 * @param data Pointer to framebuffer data
 * @param len Length of framebuffer data
 * @param palette_index Palette index for the frame
 * @return ESP_OK on success, error code on failure
 */
esp_err_t broadcast_framebuffer(const void *data, size_t len, uint8_t palette_index);

/**
 * @brief Check if there are any connected WebSocket clients
 * @return true if clients are connected, false otherwise
 */
bool has_connected_clients(void);

/**
 * @brief Get the number of connected WebSocket clients
 * @return Number of connected clients
 */
int get_client_count(void);

#ifdef __cplusplus
}
#endif

#endif // FRAME_BROADCASTER_H 