#ifndef NETWORK_TRANSMISSION_H
#define NETWORK_TRANSMISSION_H

#include "esp_http_server.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

// Network transmission configuration
#define NETWORK_TASK_STACK_SIZE 8192
#define NETWORK_TASK_PRIORITY 2
#define NETWORK_TASK_CORE 1

#define NETWORK_QUEUE_SIZE 64
#define BUFFER_POOL_SIZE 16
#define MAX_BUFFER_SIZE 32768
#define FRAGMENT_SIZE 16384

// Network message structure
typedef struct {
    uint8_t *data;
    size_t len;
    uint8_t palette_index;
    bool is_delta;
    int client_fd;
} network_message_t;

typedef struct {
    uint8_t *buffer;
    bool in_use;
    size_t size;
} buffer_pool_entry_t;

// Network transmission state
typedef struct {
    QueueHandle_t message_queue;
    TaskHandle_t task_handle;
    SemaphoreHandle_t fragmentation_mutex;
    buffer_pool_entry_t buffer_pool[BUFFER_POOL_SIZE];
    bool is_initialized;
    uint32_t frames_sent;
    uint32_t frames_dropped;
} network_transmission_t;

// Function declarations for network transmission management
esp_err_t network_transmission_init(void);
void network_transmission_cleanup(void);
bool network_transmission_is_ready(void);

// Frame transmission functions
esp_err_t network_queue_frame(const uint8_t *data, size_t len, uint8_t palette_index, int client_fd);
esp_err_t network_send_frame_sync(const uint8_t *data, size_t len, uint8_t palette_index, int client_fd);

// Buffer pool management
uint8_t* network_get_buffer(size_t size);
void network_return_buffer(uint8_t *buffer);

// Network transmission task
void network_transmission_task(void *pvParameters);

// Statistics and monitoring
void network_get_stats(uint32_t *frames_sent, uint32_t *frames_dropped);
void network_reset_stats(void);

#endif // NETWORK_TRANSMISSION_H 