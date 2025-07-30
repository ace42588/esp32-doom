#ifndef INSTRUMENTATION_H
#define INSTRUMENTATION_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Configuration
#define INSTRUMENTATION_TASK_STACK_SIZE 4096
#define INSTRUMENTATION_TASK_PRIORITY 1
#define INSTRUMENTATION_INTERVAL_MS 5000  // Log every 5 seconds

// Safety configuration - disable problematic heap functions
#define INSTRUMENTATION_SAFE_MODE 0  // Use accurate heap functions instead of safe ones

// Lightweight mode - reduce stack usage
#define INSTRUMENTATION_LIGHTWEIGHT_MODE 1  // Use compact logging and smaller buffers

// WiFi throughput counters
typedef struct {
    uint32_t bytes_sent;
    uint32_t bytes_received;
    uint32_t packets_sent;
    uint32_t packets_received;
    uint32_t last_reset_time;
    // WiFi driver statistics
    uint32_t wifi_tx_bytes;
    uint32_t wifi_rx_bytes;
    uint32_t wifi_tx_packets;
    uint32_t wifi_rx_packets;
    uint32_t wifi_tx_errors;
    uint32_t wifi_rx_errors;
    uint32_t wifi_tx_retries;
    uint32_t wifi_rx_dropped;
    int8_t wifi_rssi;
    uint8_t wifi_channel;
    uint8_t wifi_phy_mode;
} wifi_throughput_stats_t;

// Memory usage tracking
typedef struct {
    size_t free_internal_ram;
    size_t min_free_internal_ram;
    size_t free_psram;
    size_t total_psram;
    size_t largest_free_block;
} memory_stats_t;

// Task runtime statistics
typedef struct {
    char task_name[configMAX_TASK_NAME_LEN];
    uint32_t runtime_percentage;
    uint32_t stack_high_water_mark;
    uint32_t stack_size;
} task_runtime_stats_t;

// Function declarations
esp_err_t instrumentation_init(void);
void instrumentation_start(void);
void instrumentation_stop(void);

// WiFi throughput tracking (deprecated - use driver-level tracking instead)
void instrumentation_wifi_sent_bytes(uint32_t bytes);
void instrumentation_wifi_received_bytes(uint32_t bytes);
void instrumentation_wifi_sent_packet(void);
void instrumentation_wifi_received_packet(void);

// WiFi driver statistics tracking
esp_err_t instrumentation_wifi_init(void);
void instrumentation_wifi_update_stats(void);

// Memory tracking
memory_stats_t instrumentation_get_memory_stats(void);
memory_stats_t instrumentation_get_basic_memory_stats(void);
memory_stats_t instrumentation_get_ultra_safe_memory_stats(void);
memory_stats_t instrumentation_get_heap_memory_stats(void);

// Task statistics
esp_err_t instrumentation_log_task_stats(void);

// Configuration logging
void instrumentation_log_configuration(void);

#endif // INSTRUMENTATION_H 