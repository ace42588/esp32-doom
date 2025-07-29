/*
 * ESP32 Doom WebSocket Server
 *
 * This file implements the main application entry point and coordinates
 * the various modules (WebSocket server, network transmission, delta encoding).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h> 
#include <stdbool.h>
#include <sys/stat.h>

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_spiffs.h"
#include "esp_task_wdt.h"
#include "esp_psram.h"

// FreeRTOS includes
#include "freertos/semphr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <sys/socket.h>
#include <sys/time.h>

#include "protocol_examples_common.h"
#include "i_system.h"
#include "ws_doom_server.h"
#include "esp_heap_caps.h"

// Include refactored modules
#include "websocket_server.h"
#include "network_transmission.h"
#include "network_transmission_optimizations.h"
#include "delta_encoding.h"
#include "http_handlers.h"

#define DOOM_TASK_CORE 1         // Core 0 = WiFi, Core 1 = Doom
#define DOOM_TASK_STACK_SIZE 32768  // 32KB is the absolute minimum

static const char *TAG = "Main Application";

/**
 * @brief Performance monitoring task
 * @param pvParameters Task parameters (unused)
 */
void performance_monitor_task(void *pvParameters) {
    ESP_LOGI(TAG, "Performance monitor task started");
    
    while (1) {
        // Wait 10 seconds between monitoring cycles
        vTaskDelay(pdMS_TO_TICKS(10000));
        
        // Get compression statistics
        compression_stats_t comp_stats;
        get_compression_stats(&comp_stats);
        
        // Log compression performance
        if (comp_stats.original_size > 0) {
            ESP_LOGI(TAG, "Compression: %zu → %zu bytes (%.1f%%) in %lu us", 
                     comp_stats.original_size, comp_stats.compressed_size, 
                     comp_stats.compression_ratio * 100, comp_stats.compression_time_us);
        }
        
        // Log memory usage
        size_t free_heap = esp_get_free_heap_size();
        ESP_LOGI(TAG, "Free RAM: %zu bytes", free_heap);
    }
}

/**
 * @brief Doom game task that runs the PrBoom engine
 * @param pvParameters Task parameters (unused)
 */
void doom_task(void *pvParameters) {
    char const *argv[] = {
        "doom", "-cout", "ICWEFDA"
    };
    
    ESP_LOGI(TAG, "Starting Doom game task");
    
    // Register this task with the watchdog
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    ESP_ERROR_CHECK(esp_task_wdt_status(NULL));
    
    ESP_LOGI(TAG, "Calling doom_main...");
    doom_main(sizeof(argv)/sizeof(argv[0]), argv);
    ESP_LOGI(TAG, "doom_main returned (should not happen)");
}

/* ============================================================================
 * MAIN APPLICATION
 * ============================================================================ */

/**
 * @brief Main application entry point
 */
void app_main(void) {
    static httpd_handle_t server = NULL;

    ESP_LOGI(TAG, "Starting ESP32 Doom WebSocket Server");

    // Initialize ESP-IDF components first
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Connect to network (WiFi or Ethernet) before starting DOOM
    ESP_ERROR_CHECK(example_connect());

    // Give WiFi a moment to stabilize
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    // Set moderate logging levels for debugging
    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_log_level_set("esp_netif_handlers", ESP_LOG_WARN);
    esp_log_level_set("example_connect", ESP_LOG_WARN);
    esp_log_level_set("example_common", ESP_LOG_WARN);
    esp_log_level_set("httpd", ESP_LOG_WARN);
    esp_log_level_set("httpd_uri", ESP_LOG_WARN);
    esp_log_level_set("DOOM", ESP_LOG_INFO);

    // Disable WiFi power management to prevent crashes
    esp_wifi_set_ps(WIFI_PS_NONE);

    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, 
                                             &connect_handler, &server));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, 
                                             &disconnect_handler, &server));
    // Initialize web page buffer
    init_web_page_buffer();
    
    // Start the web server for the first time
    server = setup_websocket_server();
    g_server_handle = server; // Set global reference

    // Initialize asynchronous network transmission
    ESP_LOGI(TAG, "=== BEFORE NETWORK TRANSMISSION INIT ===");
    ESP_LOGI(TAG, "Internal RAM: %zu bytes, PSRAM: %zu bytes",
             esp_get_free_heap_size(), esp_psram_is_initialized() ? esp_psram_get_size() : 0);
    
    ESP_ERROR_CHECK(init_async_network_transmission());
    
    ESP_LOGI(TAG, "=== AFTER NETWORK TRANSMISSION INIT ===");
    ESP_LOGI(TAG, "Internal RAM: %zu bytes, PSRAM: %zu bytes",
             esp_get_free_heap_size(), esp_psram_is_initialized() ? esp_psram_get_size() : 0);

    // Initialize optimized network transmission with aggressive settings
    ESP_LOGI(TAG, "=== BEFORE OPTIMIZATIONS INIT ===");
    ESP_LOGI(TAG, "Internal RAM: %zu bytes, PSRAM: %zu bytes",
             esp_get_free_heap_size(), esp_psram_is_initialized() ? esp_psram_get_size() : 0);
    
    /*
    transmission_optimization_flags_t opt_flags = {
        .enable_zero_copy = true,
        .enable_adaptive_buffering = true,
        .enable_connection_pooling = true,
        .enable_memory_detection = true
    };
    
    esp_err_t opt_result = init_optimized_network_transmission(&opt_flags);
    if (opt_result == ESP_OK) {
        ESP_LOGI(TAG, "Optimized network transmission initialized successfully");
    } else {
        ESP_LOGW(TAG, "Network optimizations disabled due to insufficient memory - using basic transmission");
    }
    */
    ESP_LOGI(TAG, "Optimized network transmission disabled for stability testing");
    
    ESP_LOGI(TAG, "=== AFTER OPTIMIZATIONS INIT ===");
    ESP_LOGI(TAG, "Internal RAM: %zu bytes, PSRAM: %zu bytes",
             esp_get_free_heap_size(), esp_psram_is_initialized() ? esp_psram_get_size() : 0);

    // Temporarily disable performance monitor task for stability testing
    /*
    // Create performance monitoring task
    BaseType_t monitor_ret = xTaskCreatePinnedToCore(
        performance_monitor_task,
        "perf_monitor",
        2048,  // stack size
        NULL,
        1,     // priority (lower than network tasks)
        NULL,
        0      // Pin to Core 0 (WiFi core)
    );
    
    if (monitor_ret != pdPASS) {
        ESP_LOGW(TAG, "Failed to create performance monitor task");
    } else {
        ESP_LOGI(TAG, "Performance monitor task created successfully");
    }
    
    // Temporarily disable performance monitor task for stability testing
    if (monitor_ret == pdPASS) {
        vTaskSuspend(NULL);  // Suspend the performance monitor task
        ESP_LOGI(TAG, "Performance monitor task suspended for stability testing");
    }
    */
    ESP_LOGI(TAG, "Performance monitor task disabled for stability testing");

    // Start Doom game task after HTTP server is initialized
    ESP_LOGI(TAG, "Creating DOOM task...");
    
    // Log memory before DOOM task creation with proper internal RAM detection
    size_t free_heap_before = esp_get_free_heap_size();
    size_t min_free_heap = esp_get_minimum_free_heap_size();
    size_t free_psram = 0;
    if (esp_psram_is_initialized()) {
        free_psram = esp_psram_get_size();
    }
    
    // Get internal DRAM only (excluding PSRAM)
    size_t free_internal_ram = heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    size_t min_free_internal_ram = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    
    ESP_LOGI(TAG, "Memory before DOOM task - Free Internal RAM: %zu bytes, Min Free Internal RAM: %zu bytes, Free PSRAM: %zu bytes", 
             free_internal_ram, min_free_internal_ram, free_psram);
    
    BaseType_t task_created = xTaskCreatePinnedToCore(&doom_task, "doom", DOOM_TASK_STACK_SIZE, NULL, 2, NULL, DOOM_TASK_CORE);
    if (task_created == pdPASS) {
        ESP_LOGI(TAG, "DOOM task created successfully on Core %d", DOOM_TASK_CORE);
    } else {
        ESP_LOGE(TAG, "Failed to create DOOM task");
        ESP_LOGE(TAG, "Available Internal RAM: %zu bytes, Required: %zu bytes", free_internal_ram, DOOM_TASK_STACK_SIZE);
        
        // Check specific error conditions
        if (task_created == errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY) {
            ESP_LOGE(TAG, "Error: Could not allocate required memory for task stack");
        } else if (task_created == pdFAIL) {
            ESP_LOGE(TAG, "Error: Task creation failed");
        } else {
            ESP_LOGE(TAG, "Error: Unknown error code %d", task_created);
        }
        return;
    }
}
