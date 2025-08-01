#include "instrumentation.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_wifi_types.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"
#include <string.h>

static const char *TAG = "Instrumentation";

// Global state
static bool instrumentation_running = false;
static TaskHandle_t instrumentation_task_handle = NULL;
static TimerHandle_t instrumentation_timer = NULL;

// WiFi throughput counters (protected by mutex)
static wifi_throughput_stats_t wifi_stats = {0};
static SemaphoreHandle_t wifi_stats_mutex = NULL;
static bool wifi_instrumentation_initialized = false;

// CPU usage tracking (protected by mutex)
static cpu_task_stats_t cpu_stats[MAX_TASKS_TO_TRACK] = {0};
static uint32_t cpu_stats_count = 0;
static SemaphoreHandle_t cpu_stats_mutex = NULL;
static uint32_t last_cpu_stats_time = 0;

// PSRAM bandwidth tracking (protected by mutex)
static psram_bandwidth_stats_t psram_stats = {0};
static SemaphoreHandle_t psram_stats_mutex = NULL;

// Network throughput tracking (protected by mutex)
static network_throughput_stats_t network_stats = {0};
static SemaphoreHandle_t network_stats_mutex = NULL;

// Configuration cache
static struct {
    uint32_t cpu_freq_mhz;
    uint32_t flash_size_mb;
    bool psram_enabled;
    uint32_t wifi_mode;
    uint32_t doom_task_stack_size;
    uint32_t server_task_stack_size;
} config_cache = {0};

/**
 * @brief Get current memory statistics (with error checking)
 */
memory_stats_t instrumentation_get_memory_stats(void) {
    memory_stats_t stats = {0};
    
#if INSTRUMENTATION_SAFE_MODE
    // In safe mode, use only the most basic heap functions
    stats.free_internal_ram = esp_get_free_heap_size();
    stats.min_free_internal_ram = esp_get_minimum_free_heap_size();
    stats.largest_free_block = 0; // Always disabled in safe mode
    
    // PSRAM stats - only if available
    if (esp_psram_is_initialized()) {
        stats.free_psram = esp_psram_get_size(); // Total size
        stats.total_psram = esp_psram_get_size();
    }
    
    return stats;
#else
    // Internal RAM stats - with error checking
    size_t free_internal = 0;
    size_t min_free_internal = 0;
    
    // Try to get free internal RAM size
    free_internal = heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (free_internal == 0) {
        ESP_LOGW(TAG, "Failed to get free internal RAM size");
        free_internal = 0;
    }
    
    // Try to get minimum free internal RAM size
    min_free_internal = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (min_free_internal == 0) {
        ESP_LOGW(TAG, "Failed to get minimum free internal RAM size");
        min_free_internal = 0;
    }
    
    // CRITICAL: Skip largest_free_block as it causes LoadProhibited crashes
    // This function walks the heap and can crash if heap is corrupted
    // stats.largest_free_block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    stats.largest_free_block = 0; // Set to 0 to indicate not available
    
    stats.free_internal_ram = free_internal;
    stats.min_free_internal_ram = min_free_internal;
    
    // PSRAM stats - with error checking
    if (esp_psram_is_initialized()) {
        size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
        size_t total_psram = esp_psram_get_size();
        
        if (free_psram == 0) {
            ESP_LOGW(TAG, "Failed to get free PSRAM size");
            free_psram = 0;
        }
        if (total_psram == 0) {
            ESP_LOGW(TAG, "Failed to get total PSRAM size");
            total_psram = 0;
        }
        
        stats.free_psram = free_psram;
        stats.total_psram = total_psram;
    }
    
    return stats;
#endif
}

/**
 * @brief Get memory statistics using heap_caps functions
 * This function uses the correct heap functions to get proper memory information
 */
memory_stats_t instrumentation_get_heap_memory_stats(void) {
    memory_stats_t stats = {0};
    
    // Get internal RAM stats using heap_caps functions
    stats.free_internal_ram = heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    stats.min_free_internal_ram = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    stats.largest_free_block = 0; // Disabled for safety
    
    // Get PSRAM stats
    if (esp_psram_is_initialized()) {
        stats.free_psram = heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
        stats.total_psram = esp_psram_get_size();
    }
    
    return stats;
}

/**
 * @brief Get basic memory statistics (safer alternative)
 * This function uses only the most basic heap functions to avoid crashes
 */
memory_stats_t instrumentation_get_basic_memory_stats(void) {
    memory_stats_t stats = {0};
    
    // Use only the most basic heap functions that are less likely to crash
    stats.free_internal_ram = esp_get_free_heap_size();
    stats.min_free_internal_ram = esp_get_minimum_free_heap_size();
    
    // Don't try to get largest free block as it can cause crashes
    stats.largest_free_block = 0;
    
    // PSRAM stats - only if available
    if (esp_psram_is_initialized()) {
        stats.free_psram = esp_psram_get_size(); // This is total size, not free
        stats.total_psram = esp_psram_get_size();
    }
    
    return stats;
}

/**
 * @brief Get ultra-safe memory statistics (most conservative approach)
 * This function uses only the most basic functions and avoids all heap walking
 */
memory_stats_t instrumentation_get_ultra_safe_memory_stats(void) {
    memory_stats_t stats = {0};
    
    // Use only esp_get_free_heap_size() which is the most stable
    stats.free_internal_ram = esp_get_free_heap_size();
    stats.min_free_internal_ram = esp_get_free_heap_size(); // Use same value as fallback
    stats.largest_free_block = 0; // Always disabled
    
    // PSRAM stats - only if available
    if (esp_psram_is_initialized()) {
        stats.free_psram = esp_psram_get_size(); // Total size
        stats.total_psram = esp_psram_get_size();
    }
    
    return stats;
}

/**
 * @brief Get CPU usage per task with detailed statistics
 */
esp_err_t instrumentation_get_cpu_usage_per_task(cpu_task_stats_t *stats, uint32_t *count) {
    if (!stats || !count) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (xSemaphoreTake(cpu_stats_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire CPU stats mutex");
        return ESP_ERR_TIMEOUT;
    }
    
    // Copy current CPU stats
    *count = cpu_stats_count;
    for (uint32_t i = 0; i < cpu_stats_count && i < MAX_TASKS_TO_TRACK; i++) {
        stats[i] = cpu_stats[i];
    }
    
    xSemaphoreGive(cpu_stats_mutex);
    return ESP_OK;
}

/**
 * @brief Update CPU usage statistics for all tasks
 */
static void update_cpu_usage_stats(void) {
    if (xSemaphoreTake(cpu_stats_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire CPU stats mutex for update");
        return;
    }
    
    uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
    uint32_t time_diff = current_time - last_cpu_stats_time;
    
    if (time_diff < 100) { // Minimum 100ms between updates
        xSemaphoreGive(cpu_stats_mutex);
        return;
    }
    
    // Get all tasks
    UBaseType_t task_count = uxTaskGetNumberOfTasks();
    TaskStatus_t *task_status_array = pvPortMalloc(task_count * sizeof(TaskStatus_t));
    
    if (!task_status_array) {
        ESP_LOGE(TAG, "Failed to allocate task status array for CPU stats");
        xSemaphoreGive(cpu_stats_mutex);
        return;
    }
    
    UBaseType_t actual_task_count = uxTaskGetSystemState(task_status_array, task_count, NULL);
    
    if (actual_task_count > 0) {
        cpu_stats_count = 0;
        uint32_t total_runtime = 0;
        
        // Calculate total runtime for all tasks
        for (UBaseType_t i = 0; i < actual_task_count; i++) {
            total_runtime += task_status_array[i].ulRunTimeCounter;
        }
        
        // Update CPU stats for each task
        for (UBaseType_t i = 0; i < actual_task_count && cpu_stats_count < MAX_TASKS_TO_TRACK; i++) {
            cpu_task_stats_t *task_stat = &cpu_stats[cpu_stats_count];
            
            // Copy task name
            strncpy(task_stat->task_name, task_status_array[i].pcTaskName, configMAX_TASK_NAME_LEN - 1);
            task_stat->task_name[configMAX_TASK_NAME_LEN - 1] = '\0';
            
            // Calculate CPU usage percentage
            if (total_runtime > 0) {
                task_stat->cpu_usage_percent = (task_status_array[i].ulRunTimeCounter * 100) / total_runtime;
            } else {
                task_stat->cpu_usage_percent = 0;
            }
            
            // Update runtime statistics
            uint32_t current_runtime = task_status_array[i].ulRunTimeCounter;
            uint32_t runtime_diff = current_runtime - task_stat->last_runtime_ticks;
            
            task_stat->last_runtime_ticks = current_runtime;
            task_stat->total_runtime_ticks = current_runtime;
            task_stat->run_count = task_status_array[i].ulRunTimeCounter;
            
            // Calculate frequency (runs per second)
            if (time_diff > 0) {
                task_stat->frequency_hz = (runtime_diff * 1000) / time_diff;
            }
            
            // Update min/max/avg runtime
            uint32_t runtime_ms = (runtime_diff * portTICK_PERIOD_MS);
            if (runtime_ms < task_stat->min_runtime_ms || task_stat->min_runtime_ms == 0) {
                task_stat->min_runtime_ms = runtime_ms;
            }
            if (runtime_ms > task_stat->max_runtime_ms) {
                task_stat->max_runtime_ms = runtime_ms;
            }
            
            // Calculate average runtime (simple moving average)
            task_stat->avg_runtime_ms = (task_stat->avg_runtime_ms + runtime_ms) / 2;
            
            // Stack information
            task_stat->stack_high_water_mark = task_status_array[i].usStackHighWaterMark;
            task_stat->stack_size = task_status_array[i].usStackHighWaterMark * 2; // Estimate
            task_stat->priority = task_status_array[i].uxCurrentPriority;
            task_stat->runtime_percentage = task_stat->cpu_usage_percent;
            
            cpu_stats_count++;
        }
    }
    
    last_cpu_stats_time = current_time;
    vPortFree(task_status_array);
    xSemaphoreGive(cpu_stats_mutex);
}

/**
 * @brief Log CPU usage per task statistics
 */
static void log_cpu_usage_stats(void) {
    cpu_task_stats_t stats[MAX_TASKS_TO_TRACK];
    uint32_t count = 0;
    
    esp_err_t ret = instrumentation_get_cpu_usage_per_task(stats, &count);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get CPU usage stats");
        return;
    }
    
#if INSTRUMENTATION_LIGHTWEIGHT_MODE
    ESP_LOGI(TAG, "=== CPU USAGE ===");
    for (uint32_t i = 0; i < count; i++) {
        ESP_LOGI(TAG, "%s: %u%%", stats[i].task_name, stats[i].cpu_usage_percent);
    }
#else
    ESP_LOGI(TAG, "=== CPU USAGE PER TASK ===");
    ESP_LOGI(TAG, "Task Name           | CPU%% | Stack | Priority | Freq(Hz) | Avg(ms)");
    ESP_LOGI(TAG, "-------------------|------|-------|----------|----------|--------");
    
    for (uint32_t i = 0; i < count; i++) {
        ESP_LOGI(TAG, "%-18s | %3u%% | %5u | %8u | %8u | %7u",
                 stats[i].task_name,
                 stats[i].cpu_usage_percent,
                 stats[i].stack_high_water_mark,
                 stats[i].priority,
                 stats[i].frequency_hz,
                 stats[i].avg_runtime_ms);
    }
#endif
}

/**
 * @brief Log memory statistics (with error handling)
 */
static void log_memory_stats(void) {
    memory_stats_t stats = {0};
    
    // Use accurate memory stats instead of safe mode which gives wrong values
    stats = instrumentation_get_heap_memory_stats();
    
#if INSTRUMENTATION_LIGHTWEIGHT_MODE
    // Ultra-compact logging for lightweight mode
    ESP_LOGI(TAG, "=== MEMORY ===");
    ESP_LOGI(TAG, "RAM: %zu bytes", stats.free_internal_ram);
    ESP_LOGI(TAG, "Min: %zu bytes", stats.min_free_internal_ram);
    
    if (esp_psram_is_initialized() && stats.total_psram > 0) {
        ESP_LOGI(TAG, "PSRAM: %zu bytes", stats.total_psram);
    }
#else
    // Use more compact logging to reduce stack usage
    ESP_LOGI(TAG, "=== MEMORY STATS ===");
    
    if (stats.free_internal_ram > 0) {
        ESP_LOGI(TAG, "Free RAM: %zu bytes", stats.free_internal_ram);
    } else {
        ESP_LOGW(TAG, "Free RAM: Unable to determine");
    }
    
    if (stats.min_free_internal_ram > 0) {
        ESP_LOGI(TAG, "Min Free RAM: %zu bytes", stats.min_free_internal_ram);
    } else {
        ESP_LOGW(TAG, "Min Free RAM: Unable to determine");
    }
    
    if (stats.largest_free_block > 0) {
        ESP_LOGI(TAG, "Largest Block: %zu bytes", stats.largest_free_block);
    } else {
        ESP_LOGW(TAG, "Largest Block: Disabled (causes crashes)");
    }
    
    if (esp_psram_is_initialized()) {
        if (stats.free_psram > 0 && stats.total_psram > 0) {
            ESP_LOGI(TAG, "Free PSRAM: %zu bytes", stats.free_psram);
        } else {
            ESP_LOGW(TAG, "Free PSRAM: Unable to determine");
        }
        
        if (stats.total_psram > 0) {
            ESP_LOGI(TAG, "Total PSRAM: %zu bytes", stats.total_psram);
        } else {
            ESP_LOGW(TAG, "Total PSRAM: Unable to determine");
        }
    } else {
        ESP_LOGI(TAG, "PSRAM: Not available");
    }
#endif
}

/**
 * @brief Log task runtime statistics (with error handling)
 */
esp_err_t instrumentation_log_task_stats(void) {
#if INSTRUMENTATION_LIGHTWEIGHT_MODE
    // Ultra-compact task logging for lightweight mode
    ESP_LOGI(TAG, "=== TASKS ===");
    
    // Get all tasks and show them properly
    UBaseType_t task_count = uxTaskGetNumberOfTasks();
    ESP_LOGI(TAG, "Total tasks: %u", task_count);
    
    // Get basic task info for all tasks
    TaskStatus_t *task_status_array = pvPortMalloc(task_count * sizeof(TaskStatus_t));
    if (task_status_array) {
        UBaseType_t actual_task_count = uxTaskGetSystemState(task_status_array, task_count, NULL);
        if (actual_task_count > 0) {
            // Show all tasks, not just first 3
            for (UBaseType_t i = 0; i < actual_task_count; i++) {
                ESP_LOGI(TAG, "%s: %u bytes", 
                         task_status_array[i].pcTaskName,
                         task_status_array[i].usStackHighWaterMark);
            }
        }
        vPortFree(task_status_array);
    }
    
    return ESP_OK;
#else
    // Use smaller buffer to reduce stack usage
    char *runtime_stats_buffer = pvPortMalloc(1024); // Reduced from 2048
    if (!runtime_stats_buffer) {
        ESP_LOGE(TAG, "Failed to allocate runtime stats buffer");
        return ESP_ERR_NO_MEM;
    }
    
    // Generate runtime statistics
    vTaskGetRunTimeStats(runtime_stats_buffer);
    
    ESP_LOGI(TAG, "=== TASK RUNTIME STATS ===");
    ESP_LOGI(TAG, "%s", runtime_stats_buffer);
    
    // Log stack high water marks for all tasks with reduced stack usage
    ESP_LOGI(TAG, "=== TASK STACK USAGE ===");
    UBaseType_t task_count = uxTaskGetNumberOfTasks();
    
    // Use smaller allocation to reduce stack usage
    TaskStatus_t *task_status_array = pvPortMalloc(task_count * sizeof(TaskStatus_t));
    
    if (task_status_array) {
        UBaseType_t actual_task_count = uxTaskGetSystemState(task_status_array, task_count, NULL);
        
        if (actual_task_count > 0) {
            // Show all tasks, not just first 10
            for (UBaseType_t i = 0; i < actual_task_count; i++) {
                uint32_t stack_high_water = task_status_array[i].usStackHighWaterMark;
                
                // Use a safer approach for stack size calculation
                uint32_t stack_size = 0;
                if (task_status_array[i].pxStackBase && stack_high_water > 0) {
                    // Estimate stack size based on high water mark and typical usage
                    stack_size = stack_high_water * 2; // Conservative estimate
                }
                
                float usage_percentage = stack_size > 0 ? 
                    (float)stack_high_water / stack_size * 100.0f : 0.0f;
                
                ESP_LOGI(TAG, "Task: %-16s | Stack: %u/%u bytes (%.1f%%) | Priority: %u", 
                         task_status_array[i].pcTaskName,
                         stack_high_water,
                         stack_size,
                         usage_percentage,
                         task_status_array[i].uxCurrentPriority);
            }
        } else {
            ESP_LOGW(TAG, "Failed to get task system state");
        }
        
        vPortFree(task_status_array);
    } else {
        ESP_LOGE(TAG, "Failed to allocate task status array");
    }
    
    vPortFree(runtime_stats_buffer);
    return ESP_OK;
#endif
}

/**
 * @brief Log WiFi throughput statistics (with error handling)
 */
static void log_wifi_stats(void) {
    if (xSemaphoreTake(wifi_stats_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
        uint32_t time_diff = current_time - wifi_stats.last_reset_time;
        
#if INSTRUMENTATION_LIGHTWEIGHT_MODE
        // Ultra-compact WiFi logging
        ESP_LOGI(TAG, "=== WIFI ===");
        ESP_LOGI(TAG, "RSSI: %d dBm", wifi_stats.wifi_rssi);
        ESP_LOGI(TAG, "Channel: %u", wifi_stats.wifi_channel);
        ESP_LOGI(TAG, "PHY: %u", wifi_stats.wifi_phy_mode);
#else
        ESP_LOGI(TAG, "=== WIFI STATS ===");
        ESP_LOGI(TAG, "Period: %u ms", time_diff);
        
        // WiFi connection info - compact logging
        ESP_LOGI(TAG, "WiFi RSSI: %d dBm", wifi_stats.wifi_rssi);
        ESP_LOGI(TAG, "WiFi Channel: %u", wifi_stats.wifi_channel);
        ESP_LOGI(TAG, "WiFi PHY: %u", wifi_stats.wifi_phy_mode);
        
        // Note: Manual tracking deprecated, using esp_wifi_statis_dump() for driver stats
        ESP_LOGI(TAG, "Note: Using esp_wifi_statis_dump() for driver-level statistics");
#endif
        
        // Reset time for next period
        wifi_stats.last_reset_time = current_time;
        
        xSemaphoreGive(wifi_stats_mutex);
    } else {
        ESP_LOGW(TAG, "Failed to acquire WiFi stats mutex");
    }
}

/**
 * @brief Get PSRAM bandwidth statistics
 */
esp_err_t instrumentation_get_psram_bandwidth_stats(psram_bandwidth_stats_t *stats) {
    if (!stats) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (xSemaphoreTake(psram_stats_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire PSRAM stats mutex");
        return ESP_ERR_TIMEOUT;
    }
    
    *stats = psram_stats;
    xSemaphoreGive(psram_stats_mutex);
    return ESP_OK;
}

/**
 * @brief Track PSRAM read operation
 */
void instrumentation_psram_read_operation(uint32_t bytes) {
    if (xSemaphoreTake(psram_stats_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        psram_stats.read_operations++;
        psram_stats.bytes_read += bytes;
        xSemaphoreGive(psram_stats_mutex);
    }
}

/**
 * @brief Track PSRAM write operation
 */
void instrumentation_psram_write_operation(uint32_t bytes) {
    if (xSemaphoreTake(psram_stats_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        psram_stats.write_operations++;
        psram_stats.bytes_written += bytes;
        xSemaphoreGive(psram_stats_mutex);
    }
}

/**
 * @brief Track PSRAM cache hit
 */
void instrumentation_psram_cache_hit(void) {
    if (xSemaphoreTake(psram_stats_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        psram_stats.cache_hits++;
        xSemaphoreGive(psram_stats_mutex);
    }
}

/**
 * @brief Track PSRAM cache miss
 */
void instrumentation_psram_cache_miss(void) {
    if (xSemaphoreTake(psram_stats_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        psram_stats.cache_misses++;
        xSemaphoreGive(psram_stats_mutex);
    }
}

/**
 * @brief Update PSRAM bandwidth utilization
 */
static void update_psram_bandwidth_stats(void) {
    if (xSemaphoreTake(psram_stats_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire PSRAM stats mutex for update");
        return;
    }
    
    uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
    uint32_t time_diff = current_time - psram_stats.last_reset_time;
    
    if (time_diff > 0) {
        // Calculate bandwidth utilization based on total operations
        uint32_t total_operations = psram_stats.read_operations + psram_stats.write_operations;
        uint32_t total_bytes = psram_stats.bytes_read + psram_stats.bytes_written;
        
        // Calculate actual bandwidth in bytes per second
        uint32_t bytes_per_second = (total_bytes * 1000) / time_diff;
        
        // Estimate bandwidth utilization based on operations and access patterns
        // PSRAM theoretical bandwidth is ~40MB/s, but actual usable bandwidth is lower
        // Consider both operation frequency and data volume
        uint32_t theoretical_bandwidth_bps = 20 * 1024 * 1024; // 20 MB/s conservative estimate
        
        if (theoretical_bandwidth_bps > 0) {
            // Calculate utilization based on both data volume and operation frequency
            uint32_t data_utilization = (bytes_per_second * 100) / theoretical_bandwidth_bps;
            uint32_t operation_utilization = 0;
            
            // Factor in operation frequency (high frequency = higher utilization)
            if (time_diff > 0) {
                uint32_t ops_per_second = (total_operations * 1000) / time_diff;
                // Assume max 1000 ops/sec is 100% utilization
                operation_utilization = (ops_per_second * 100) / 1000;
                if (operation_utilization > 100) operation_utilization = 100;
            }
            
            // Combine data and operation utilization
            psram_stats.bandwidth_utilization_percent = (data_utilization + operation_utilization) / 2;
            
            if (psram_stats.bandwidth_utilization_percent > 100) {
                psram_stats.bandwidth_utilization_percent = 100;
            }
        }
        
        // Reset counters for next period
        psram_stats.read_operations = 0;
        psram_stats.write_operations = 0;
        psram_stats.bytes_read = 0;
        psram_stats.bytes_written = 0;
        psram_stats.cache_hits = 0;
        psram_stats.cache_misses = 0;
        psram_stats.last_reset_time = current_time;
    }
    
    xSemaphoreGive(psram_stats_mutex);
}

/**
 * @brief Log PSRAM bandwidth statistics
 */
static void log_psram_bandwidth_stats(void) {
    psram_bandwidth_stats_t stats;
    
    esp_err_t ret = instrumentation_get_psram_bandwidth_stats(&stats);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get PSRAM bandwidth stats");
        return;
    }
    
#if INSTRUMENTATION_LIGHTWEIGHT_MODE
    ESP_LOGI(TAG, "=== PSRAM ===");
    ESP_LOGI(TAG, "Read: %u ops, %u bytes", stats.read_operations, stats.bytes_read);
    ESP_LOGI(TAG, "Write: %u ops, %u bytes", stats.write_operations, stats.bytes_written);
    ESP_LOGI(TAG, "Cache: %u hits, %u misses", stats.cache_hits, stats.cache_misses);
    ESP_LOGI(TAG, "Bandwidth: %u%%", stats.bandwidth_utilization_percent);
#else
    ESP_LOGI(TAG, "=== PSRAM BANDWIDTH STATS ===");
    ESP_LOGI(TAG, "Read Operations: %u", stats.read_operations);
    ESP_LOGI(TAG, "Write Operations: %u", stats.write_operations);
    ESP_LOGI(TAG, "Bytes Read: %u", stats.bytes_read);
    ESP_LOGI(TAG, "Bytes Written: %u", stats.bytes_written);
    ESP_LOGI(TAG, "Cache Hits: %u", stats.cache_hits);
    ESP_LOGI(TAG, "Cache Misses: %u", stats.cache_misses);
    
    if (stats.cache_hits + stats.cache_misses > 0) {
        uint32_t cache_hit_rate = (stats.cache_hits * 100) / (stats.cache_hits + stats.cache_misses);
        ESP_LOGI(TAG, "Cache Hit Rate: %u%%", cache_hit_rate);
    }
    
    ESP_LOGI(TAG, "Bandwidth Utilization: %u%%", stats.bandwidth_utilization_percent);
#endif
}

/**
 * @brief Get network throughput statistics
 */
esp_err_t instrumentation_get_network_throughput_stats(network_throughput_stats_t *stats) {
    if (!stats) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (xSemaphoreTake(network_stats_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire network stats mutex");
        return ESP_ERR_TIMEOUT;
    }
    
    *stats = network_stats;
    xSemaphoreGive(network_stats_mutex);
    return ESP_OK;
}

/**
 * @brief Track network bytes sent
 */
void instrumentation_network_sent_bytes(uint32_t bytes) {
    if (xSemaphoreTake(network_stats_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        network_stats.bytes_sent += bytes;
        network_stats.packets_sent++;
        xSemaphoreGive(network_stats_mutex);
    }
}

/**
 * @brief Track network bytes received
 */
void instrumentation_network_received_bytes(uint32_t bytes) {
    if (xSemaphoreTake(network_stats_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        network_stats.bytes_received += bytes;
        network_stats.packets_received++;
        xSemaphoreGive(network_stats_mutex);
    }
}

/**
 * @brief Track network packet sent
 */
void instrumentation_network_sent_packet(void) {
    if (xSemaphoreTake(network_stats_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        network_stats.packets_sent++;
        xSemaphoreGive(network_stats_mutex);
    }
}

/**
 * @brief Track network packet received
 */
void instrumentation_network_received_packet(void) {
    if (xSemaphoreTake(network_stats_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        network_stats.packets_received++;
        xSemaphoreGive(network_stats_mutex);
    }
}

/**
 * @brief Update network throughput statistics
 */
static void update_network_throughput_stats(void) {
    if (xSemaphoreTake(network_stats_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire network stats mutex for update");
        return;
    }
    
    uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
    uint32_t time_diff = current_time - network_stats.last_reset_time;
    
    if (time_diff > 0) {
        // Calculate bytes per second
        network_stats.bytes_per_sec_sent = (network_stats.bytes_sent * 1000) / time_diff;
        network_stats.bytes_per_sec_received = (network_stats.bytes_received * 1000) / time_diff;
        
        // Calculate packets per second
        network_stats.packets_per_sec_sent = (network_stats.packets_sent * 1000) / time_diff;
        network_stats.packets_per_sec_received = (network_stats.packets_received * 1000) / time_diff;
        
        // Calculate connection quality based on WiFi RSSI
        if (wifi_stats.wifi_rssi != 0) {
            // RSSI ranges from -100 (poor) to -30 (excellent)
            int8_t rssi = wifi_stats.wifi_rssi;
            if (rssi >= -50) {
                network_stats.connection_quality_percent = 100;
            } else if (rssi >= -60) {
                network_stats.connection_quality_percent = 90;
            } else if (rssi >= -70) {
                network_stats.connection_quality_percent = 75;
            } else if (rssi >= -80) {
                network_stats.connection_quality_percent = 50;
            } else if (rssi >= -90) {
                network_stats.connection_quality_percent = 25;
            } else {
                network_stats.connection_quality_percent = 10;
            }
        }
        
        // Calculate retransmission rate (simplified)
        uint32_t total_packets = network_stats.packets_sent + network_stats.packets_received;
        if (total_packets > 0) {
            // Estimate retransmission rate based on WiFi errors
            uint32_t total_errors = wifi_stats.wifi_tx_errors + wifi_stats.wifi_rx_errors;
            network_stats.retransmission_rate_percent = (total_errors * 100) / total_packets;
            if (network_stats.retransmission_rate_percent > 100) {
                network_stats.retransmission_rate_percent = 100;
            }
        }
    }
    
    xSemaphoreGive(network_stats_mutex);
}

/**
 * @brief Log network throughput statistics
 */
static void log_network_throughput_stats(void) {
    network_throughput_stats_t stats;
    
    esp_err_t ret = instrumentation_get_network_throughput_stats(&stats);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get network throughput stats");
        return;
    }
    
#if INSTRUMENTATION_LIGHTWEIGHT_MODE
    ESP_LOGI(TAG, "=== NETWORK ===");
    ESP_LOGI(TAG, "Sent: %u bytes/s", stats.bytes_per_sec_sent);
    ESP_LOGI(TAG, "Received: %u bytes/s", stats.bytes_per_sec_received);
    ESP_LOGI(TAG, "Quality: %u%%", stats.connection_quality_percent);
    ESP_LOGI(TAG, "Retransmit: %u%%", stats.retransmission_rate_percent);
#else
    ESP_LOGI(TAG, "=== NETWORK THROUGHPUT STATS ===");
    ESP_LOGI(TAG, "Bytes Sent: %u (%u bytes/sec)", stats.bytes_sent, stats.bytes_per_sec_sent);
    ESP_LOGI(TAG, "Bytes Received: %u (%u bytes/sec)", stats.bytes_received, stats.bytes_per_sec_received);
    ESP_LOGI(TAG, "Packets Sent: %u (%u packets/sec)", stats.packets_sent, stats.packets_per_sec_sent);
    ESP_LOGI(TAG, "Packets Received: %u (%u packets/sec)", stats.packets_received, stats.packets_per_sec_received);
    ESP_LOGI(TAG, "Connection Quality: %u%%", stats.connection_quality_percent);
    ESP_LOGI(TAG, "Retransmission Rate: %u%%", stats.retransmission_rate_percent);
#endif
}

/**
 * @brief Log system configuration
 */
void instrumentation_log_configuration(void) {
    ESP_LOGI(TAG, "=== SYSTEM CONFIG ===");
    ESP_LOGI(TAG, "CPU: %u MHz", config_cache.cpu_freq_mhz);
    ESP_LOGI(TAG, "Flash: %u MB", config_cache.flash_size_mb);
    ESP_LOGI(TAG, "PSRAM: %s", config_cache.psram_enabled ? "Yes" : "No");
    ESP_LOGI(TAG, "WiFi Mode: %u", config_cache.wifi_mode);
    ESP_LOGI(TAG, "Doom Stack: %u bytes", config_cache.doom_task_stack_size);
    ESP_LOGI(TAG, "Server Stack: %u bytes", config_cache.server_task_stack_size);
    
    // Log some key sdkconfig values - compact
    ESP_LOGI(TAG, "FreeRTOS Config:");
    ESP_LOGI(TAG, "  Max Task Name: %d", configMAX_TASK_NAME_LEN);
    ESP_LOGI(TAG, "  Max Priorities: %d", configMAX_PRIORITIES);
    ESP_LOGI(TAG, "  Tick Rate: %d Hz", configTICK_RATE_HZ);
    ESP_LOGI(TAG, "  Idle Stack: %d", configMINIMAL_STACK_SIZE);
    
    // Log heap configuration with error checking
    ESP_LOGI(TAG, "Heap Config:");
    size_t free_heap = esp_get_free_heap_size();
    size_t min_free_heap = esp_get_minimum_free_heap_size();
    
    if (free_heap > 0) {
        ESP_LOGI(TAG, "  Free Heap: %zu bytes", free_heap);
    } else {
        ESP_LOGW(TAG, "  Free Heap: Unable to determine");
    }
    
    if (min_free_heap > 0) {
        ESP_LOGI(TAG, "  Min Free Heap: %zu bytes", min_free_heap);
    } else {
        ESP_LOGW(TAG, "  Min Free Heap: Unable to determine");
    }
    
    // Log instrumentation configuration
    ESP_LOGI(TAG, "Instrumentation Config:");
    ESP_LOGI(TAG, "  Interval: %d ms", INSTRUMENTATION_INTERVAL_MS);
    ESP_LOGI(TAG, "  Stack Size: %d bytes", INSTRUMENTATION_TASK_STACK_SIZE);
    ESP_LOGI(TAG, "  Priority: %d", INSTRUMENTATION_TASK_PRIORITY);
}

/**
 * @brief Periodic instrumentation timer callback (with error handling)
 */
static void instrumentation_timer_callback(TimerHandle_t xTimer) {
    if (!instrumentation_running) {
        return;
    }
    
    // Wrap the entire callback in error handling to prevent crashes
    ESP_LOGI(TAG, "=== INSTRUMENTATION REPORT ===");
    
    // Log memory stats (with error handling)
    log_memory_stats();
    
    // Log task stats (with error handling)
    esp_err_t task_stats_ret = instrumentation_log_task_stats();
    if (task_stats_ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to log task statistics");
    }
    
    // Update and log WiFi stats (with error handling)
    instrumentation_wifi_update_stats();
    log_wifi_stats();
    
    // Update and log CPU usage stats (with error handling)
    update_cpu_usage_stats();
    log_cpu_usage_stats();

    // Update and log PSRAM bandwidth stats (with error handling)
    update_psram_bandwidth_stats();
    log_psram_bandwidth_stats();

    // Update and log network throughput stats (with error handling)
    update_network_throughput_stats();
    log_network_throughput_stats();
    
    // Log comprehensive system statistics
    instrumentation_log_comprehensive_stats();
    
    // Log WebSocket profiling stats (if available)
    extern void log_all_websocket_profiles(void);
    log_all_websocket_profiles();
    
    ESP_LOGI(TAG, "=== END REPORT ===");
}

/**
 * @brief Initialize WiFi driver statistics tracking
 */
esp_err_t instrumentation_wifi_init(void) {
    if (wifi_instrumentation_initialized) {
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Initializing WiFi driver statistics tracking");
    
    // Enable WiFi statistics collection
    esp_err_t ret = esp_wifi_set_promiscuous_rx_cb(NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set WiFi promiscuous callback: %s", esp_err_to_name(ret));
    }
    
    wifi_instrumentation_initialized = true;
    ESP_LOGI(TAG, "WiFi driver statistics tracking initialized");
    return ESP_OK;
}

/**
 * @brief Update WiFi driver statistics using esp_wifi_statis_dump()
 */
void instrumentation_wifi_update_stats(void) {
    if (!wifi_instrumentation_initialized) {
        return;
    }
    
    if (xSemaphoreTake(wifi_stats_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        // Get current WiFi connection info
        wifi_ap_record_t ap_info;
        esp_err_t ret = esp_wifi_sta_get_ap_info(&ap_info);
        if (ret == ESP_OK) {
            wifi_stats.wifi_rssi = ap_info.rssi;
            wifi_stats.wifi_channel = ap_info.primary;
            wifi_stats.wifi_phy_mode = ap_info.phy_11b ? 1 : 
                                     ap_info.phy_11g ? 2 : 
                                     ap_info.phy_11n ? 3 : 0;
        } else {
            ESP_LOGW(TAG, "Failed to get WiFi AP info: %s", esp_err_to_name(ret));
        }
        
        // Get WiFi driver statistics using esp_wifi_statis_dump()
        // This provides actual driver-level statistics instead of manual tracking
        esp_wifi_statis_dump(0); // Dump to console for debugging
        
        xSemaphoreGive(wifi_stats_mutex);
    } else {
        ESP_LOGW(TAG, "Failed to acquire WiFi stats mutex for update");
    }
}

/**
 * @brief Initialize instrumentation system
 */
esp_err_t instrumentation_init(void) {
    ESP_LOGI(TAG, "Initializing instrumentation system");
    
    // Create mutex for WiFi stats
    wifi_stats_mutex = xSemaphoreCreateMutex();
    if (!wifi_stats_mutex) {
        ESP_LOGE(TAG, "Failed to create WiFi stats mutex");
        return ESP_ERR_NO_MEM;
    }

    // Create mutex for CPU stats
    cpu_stats_mutex = xSemaphoreCreateMutex();
    if (!cpu_stats_mutex) {
        ESP_LOGE(TAG, "Failed to create CPU stats mutex");
        return ESP_ERR_NO_MEM;
    }

    // Create mutex for PSRAM stats
    psram_stats_mutex = xSemaphoreCreateMutex();
    if (!psram_stats_mutex) {
        ESP_LOGE(TAG, "Failed to create PSRAM stats mutex");
        return ESP_ERR_NO_MEM;
    }

    // Create mutex for network stats
    network_stats_mutex = xSemaphoreCreateMutex();
    if (!network_stats_mutex) {
        ESP_LOGE(TAG, "Failed to create network stats mutex");
        return ESP_ERR_NO_MEM;
    }
    
    // Cache configuration values
    config_cache.cpu_freq_mhz = 240;  // ESP32 default CPU frequency in MHz
    config_cache.flash_size_mb = 4;    // ESP32 default flash size in MB
    config_cache.psram_enabled = esp_psram_is_initialized();
    
    // Get WiFi mode with error checking
    wifi_mode_t wifi_mode;
    esp_err_t ret = esp_wifi_get_mode(&wifi_mode);
    if (ret == ESP_OK) {
        config_cache.wifi_mode = wifi_mode;
    } else {
        ESP_LOGW(TAG, "Failed to get WiFi mode: %s", esp_err_to_name(ret));
        config_cache.wifi_mode = 0;
    }
    
    // Cache task stack sizes
    config_cache.doom_task_stack_size = 32768;  // From main.c
    config_cache.server_task_stack_size = 8192;  // From main.c
    
    // Initialize WiFi stats
    wifi_stats.last_reset_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    // Initialize PSRAM stats
    psram_stats.last_reset_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    // Initialize network stats
    network_stats.last_reset_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    // Initialize CPU stats
    last_cpu_stats_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
    cpu_stats_count = 0;
    
    // Initialize WiFi driver statistics tracking
    ret = instrumentation_wifi_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to initialize WiFi instrumentation: %s", esp_err_to_name(ret));
    }
    
    ESP_LOGI(TAG, "Instrumentation system initialized with comprehensive monitoring");
    return ESP_OK;
}

/**
 * @brief Start periodic instrumentation
 */
void instrumentation_start(void) {
    if (instrumentation_running) {
        ESP_LOGW(TAG, "Instrumentation already running");
        return;
    }
    
    // Create periodic timer
    instrumentation_timer = xTimerCreate("instrumentation_timer",
                                       pdMS_TO_TICKS(INSTRUMENTATION_INTERVAL_MS),
                                       pdTRUE,  // Auto-reload
                                       NULL,
                                       instrumentation_timer_callback);
    
    if (!instrumentation_timer) {
        ESP_LOGE(TAG, "Failed to create instrumentation timer");
        return;
    }
    
    // Start timer
    if (xTimerStart(instrumentation_timer, 0) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start instrumentation timer");
        xTimerDelete(instrumentation_timer, 0);
        instrumentation_timer = NULL;
        return;
    }
    
    instrumentation_running = true;
    ESP_LOGI(TAG, "Instrumentation started (logging every %d ms)", INSTRUMENTATION_INTERVAL_MS);
}

/**
 * @brief Stop periodic instrumentation
 */
void instrumentation_stop(void) {
    if (!instrumentation_running) {
        return;
    }
    
    if (instrumentation_timer) {
        xTimerStop(instrumentation_timer, 0);
        xTimerDelete(instrumentation_timer, 0);
        instrumentation_timer = NULL;
    }
    
    instrumentation_running = false;
    ESP_LOGI(TAG, "Instrumentation stopped");
}

/**
 * @brief Track WiFi bytes sent
 * @deprecated Use esp_wifi_statis_dump() for proper WiFi driver statistics
 */
void instrumentation_wifi_sent_bytes(uint32_t bytes) {
    // Deprecated - use esp_wifi_statis_dump() instead
    ESP_LOGW(TAG, "instrumentation_wifi_sent_bytes() is deprecated - use esp_wifi_statis_dump()");
}

/**
 * @brief Track WiFi bytes received
 * @deprecated Use esp_wifi_statis_dump() for proper WiFi driver statistics
 */
void instrumentation_wifi_received_bytes(uint32_t bytes) {
    // Deprecated - use esp_wifi_statis_dump() instead
    ESP_LOGW(TAG, "instrumentation_wifi_received_bytes() is deprecated - use esp_wifi_statis_dump()");
}

/**
 * @brief Track WiFi packet sent
 * @deprecated Use esp_wifi_statis_dump() for proper WiFi driver statistics
 */
void instrumentation_wifi_sent_packet(void) {
    // Deprecated - use esp_wifi_statis_dump() instead
    ESP_LOGW(TAG, "instrumentation_wifi_sent_packet() is deprecated - use esp_wifi_statis_dump()");
}

/**
 * @brief Track WiFi packet received
 * @deprecated Use esp_wifi_statis_dump() for proper WiFi driver statistics
 */
void instrumentation_wifi_received_packet(void) {
    // Deprecated - use esp_wifi_statis_dump() instead
    ESP_LOGW(TAG, "instrumentation_wifi_received_packet() is deprecated - use esp_wifi_statis_dump()");
} 

/**
 * @brief Get comprehensive system statistics
 */
esp_err_t instrumentation_get_comprehensive_stats(system_stats_t *stats) {
    if (!stats) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Get CPU stats
    esp_err_t ret = instrumentation_get_cpu_usage_per_task(stats->cpu_stats, &stats->cpu_stats_count);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get CPU stats");
    }
    
    // Get PSRAM stats
    ret = instrumentation_get_psram_bandwidth_stats(&stats->psram_stats);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get PSRAM stats");
    }
    
    // Get network stats
    ret = instrumentation_get_network_throughput_stats(&stats->network_stats);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get network stats");
    }
    
    // Get memory stats
    stats->memory_stats = instrumentation_get_heap_memory_stats();
    
    // Get WiFi stats
    stats->wifi_stats = wifi_stats;
    
    // Calculate total CPU usage
    stats->total_cpu_usage_percent = 0;
    for (uint32_t i = 0; i < stats->cpu_stats_count; i++) {
        stats->total_cpu_usage_percent += stats->cpu_stats[i].cpu_usage_percent;
    }
    
    // Get system uptime
    stats->system_uptime_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    return ESP_OK;
}

/**
 * @brief Log comprehensive system statistics
 */
void instrumentation_log_comprehensive_stats(void) {
    system_stats_t stats = {0};
    
    esp_err_t ret = instrumentation_get_comprehensive_stats(&stats);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get comprehensive stats");
        return;
    }
    
    ESP_LOGI(TAG, "=== COMPREHENSIVE SYSTEM STATS ===");
    ESP_LOGI(TAG, "System Uptime: %u ms", stats.system_uptime_ms);
    ESP_LOGI(TAG, "Total CPU Usage: %u%%", stats.total_cpu_usage_percent);
    
    // Log memory summary
    ESP_LOGI(TAG, "Memory - Free RAM: %zu bytes, PSRAM: %zu bytes", 
             stats.memory_stats.free_internal_ram, stats.memory_stats.free_psram);
    
    // Log PSRAM bandwidth summary
    ESP_LOGI(TAG, "PSRAM - Bandwidth: %u%%, Read: %u ops, Write: %u ops",
             stats.psram_stats.bandwidth_utilization_percent,
             stats.psram_stats.read_operations, stats.psram_stats.write_operations);
    
    // Log network summary
    ESP_LOGI(TAG, "Network - Sent: %u bytes/s, Received: %u bytes/s, Quality: %u%%",
             stats.network_stats.bytes_per_sec_sent, stats.network_stats.bytes_per_sec_received,
             stats.network_stats.connection_quality_percent);
    
    // Log WiFi summary
    ESP_LOGI(TAG, "WiFi - RSSI: %d dBm, Channel: %u, PHY: %u",
             stats.wifi_stats.wifi_rssi, stats.wifi_stats.wifi_channel, stats.wifi_stats.wifi_phy_mode);
    
    // Log top CPU consumers
    ESP_LOGI(TAG, "Top CPU Consumers:");
    for (uint32_t i = 0; i < stats.cpu_stats_count && i < 5; i++) {
        if (stats.cpu_stats[i].cpu_usage_percent > 0) {
            ESP_LOGI(TAG, "  %s: %u%% (Freq: %u Hz, Avg: %u ms)",
                     stats.cpu_stats[i].task_name,
                     stats.cpu_stats[i].cpu_usage_percent,
                     stats.cpu_stats[i].frequency_hz,
                     stats.cpu_stats[i].avg_runtime_ms);
        }
    }
} 