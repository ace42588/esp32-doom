#ifndef INSTRUMENTATION_H
#define INSTRUMENTATION_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_psram.h"
#include "esp_wifi.h"

// Configuration
#define INSTRUMENTATION_TASK_STACK_SIZE 4096
#define INSTRUMENTATION_TASK_PRIORITY 1
#define INSTRUMENTATION_INTERVAL_MS 5000  // Log every 5 seconds

// Safety configuration - disable problematic heap functions
#define INSTRUMENTATION_SAFE_MODE 0  // Use accurate heap functions instead of safe ones

// Lightweight mode - reduce stack usage
#define INSTRUMENTATION_LIGHTWEIGHT_MODE 1  // Use compact logging and smaller buffers

// CPU usage tracking configuration
#define MAX_TASKS_TO_TRACK 16
#define CPU_USAGE_HISTORY_SIZE 10

// PSRAM bandwidth tracking
typedef struct {
    uint32_t read_operations;
    uint32_t write_operations;
    uint32_t bytes_read;
    uint32_t bytes_written;
    uint32_t cache_hits;
    uint32_t cache_misses;
    uint32_t bandwidth_utilization_percent;
    uint32_t last_reset_time;
} psram_bandwidth_stats_t;

// CPU usage per task tracking
typedef struct {
    char task_name[configMAX_TASK_NAME_LEN];
    uint32_t runtime_percentage;
    uint32_t stack_high_water_mark;
    uint32_t stack_size;
    uint32_t priority;
    uint32_t cpu_usage_percent;
    uint32_t run_count;
    uint32_t total_runtime_ticks;
    uint32_t last_runtime_ticks;
    uint32_t frequency_hz;
    uint32_t min_runtime_ms;
    uint32_t max_runtime_ms;
    uint32_t avg_runtime_ms;
} cpu_task_stats_t;

// Network throughput metrics
typedef struct {
    uint32_t bytes_sent;
    uint32_t bytes_received;
    uint32_t packets_sent;
    uint32_t packets_received;
    uint32_t bytes_per_sec_sent;
    uint32_t bytes_per_sec_received;
    uint32_t packets_per_sec_sent;
    uint32_t packets_per_sec_received;
    uint32_t connection_quality_percent;
    uint32_t retransmission_rate_percent;
    uint32_t last_reset_time;
} network_throughput_stats_t;

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

// Comprehensive system statistics
typedef struct {
    cpu_task_stats_t cpu_stats[MAX_TASKS_TO_TRACK];
    uint32_t cpu_stats_count;
    psram_bandwidth_stats_t psram_stats;
    network_throughput_stats_t network_stats;
    wifi_throughput_stats_t wifi_stats;
    memory_stats_t memory_stats;
    uint32_t total_cpu_usage_percent;
    uint32_t system_uptime_ms;
} system_stats_t;

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

// New comprehensive instrumentation functions
esp_err_t instrumentation_get_cpu_usage_per_task(cpu_task_stats_t *stats, uint32_t *count);
esp_err_t instrumentation_get_psram_bandwidth_stats(psram_bandwidth_stats_t *stats);
esp_err_t instrumentation_get_network_throughput_stats(network_throughput_stats_t *stats);
esp_err_t instrumentation_get_comprehensive_stats(system_stats_t *stats);
void instrumentation_log_comprehensive_stats(void);

// PSRAM bandwidth tracking
void instrumentation_psram_read_operation(uint32_t bytes);
void instrumentation_psram_write_operation(uint32_t bytes);
void instrumentation_psram_cache_hit(void);
void instrumentation_psram_cache_miss(void);

// Network throughput tracking
void instrumentation_network_sent_bytes(uint32_t bytes);
void instrumentation_network_received_bytes(uint32_t bytes);
void instrumentation_network_sent_packet(void);
void instrumentation_network_received_packet(void);

#endif // INSTRUMENTATION_H 