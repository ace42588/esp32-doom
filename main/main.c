/*
 * ESP32 Doom WebSocket Server
 *
 * This file implements the main application entry point and coordinates
 * the various modules (WebSocket server, network transmission, delta encoding).
 */

#include <stdio.h>

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "esp_task_wdt.h"
#include "esp_psram.h"

// FreeRTOS includes
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "protocol_examples_common.h"
#include "i_system.h"
#include "server_integration.h"
#include "esp_heap_caps.h"

#define DOOM_TASK_CORE 1         // Core 0 = WiFi, Core 1 = Doom
#define DOOM_TASK_STACK_SIZE 32768  // 32KB is the absolute minimum
#define SERVER_TASK_STACK_SIZE 8192
#define SERVER_TASK_PRIORITY 2

static const char *TAG = "Main Application";

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
    ESP_LOGI(TAG, "Starting ESP32 Doom WebSocket Server");

    // Initialize ESP-IDF components first
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Connect to WiFi before starting DOOM
    ESP_ERROR_CHECK(example_connect());

    // Set moderate logging levels for debugging
    esp_log_level_set("DOOM", ESP_LOG_INFO);

    // Disable WiFi power management to prevent crashes
    esp_wifi_set_ps(WIFI_PS_NONE);

    // Start Doom game task after all initialization is complete
    ESP_LOGI(TAG, "Creating DOOM task...");
    
    // Log memory before DOOM task creation
    size_t free_psram = 0;
    if (esp_psram_is_initialized()) {
        free_psram = esp_psram_get_size();
    }
    // Get internal DRAM only (excluding PSRAM)
    size_t free_internal_ram = heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    size_t min_free_internal_ram = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    
    ESP_LOGI(TAG, "Memory before DOOM task - Free Internal RAM: %zu bytes, Min Free Internal RAM: %zu bytes, Free PSRAM: %zu bytes", 
             free_internal_ram, min_free_internal_ram, free_psram);
    
    BaseType_t task_created = xTaskCreatePinnedToCore(&doom_task, "doom", DOOM_TASK_STACK_SIZE, NULL, 3, NULL, DOOM_TASK_CORE);
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
    
    // Start server integration task (handles both HTTP and WebSocket)
    ESP_LOGI(TAG, "Creating server integration task...");
    BaseType_t server_task_created = xTaskCreate(&server_integration_task, "server_integration", SERVER_TASK_STACK_SIZE, NULL, SERVER_TASK_PRIORITY, NULL);
    if (server_task_created == pdPASS) {
        ESP_LOGI(TAG, "Server integration task created successfully");
    } else {
        ESP_LOGE(TAG, "Failed to create server integration task");
        return;
    }
}
