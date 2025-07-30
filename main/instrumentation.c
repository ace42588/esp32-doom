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
#include "sdkconfig.h"

static const char *TAG = "Instrumentation";

// Global state
static bool instrumentation_running = false;
static TaskHandle_t instrumentation_task_handle = NULL;
static TimerHandle_t instrumentation_timer = NULL;

// WiFi throughput counters (protected by mutex)
static wifi_throughput_stats_t wifi_stats = {0};
static SemaphoreHandle_t wifi_stats_mutex = NULL;
static bool wifi_instrumentation_initialized = false;

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
    
    // Initialize WiFi driver statistics tracking
    ret = instrumentation_wifi_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to initialize WiFi instrumentation: %s", esp_err_to_name(ret));
    }
    
    ESP_LOGI(TAG, "Instrumentation system initialized");
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