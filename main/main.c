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
#include "instrumentation.h"
#include "websocket_server.h"

#define DOOM_TASK_CORE 1         // Core 0 = WiFi, Core 1 = Doom
#define DOOM_TASK_STACK_SIZE 32768  // 32KB is the absolute minimum
#define DOOM_TASK_PRIORITY 8

#define SERVER_TASK_CORE 0
#define SERVER_TASK_STACK_SIZE 8192
#define SERVER_TASK_PRIORITY 2

#define WEBSOCKET_TASK_CORE 0
#define WEBSOCKET_TASK_STACK_SIZE 8192
#define WEBSOCKET_TASK_PRIORITY 4

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

    // Initialize and start instrumentation system early to capture startup effects
    ESP_LOGI(TAG, "Initializing instrumentation system...");
    ESP_ERROR_CHECK(instrumentation_init());
    
    // Log initial configuration
    instrumentation_log_configuration();
    
    // Start periodic instrumentation
    instrumentation_start();

    // Start server integration task (handles both HTTP and WebSocket)
    ESP_LOGI(TAG, "Creating server integration task...");
    BaseType_t server_task_created = xTaskCreatePinnedToCore(
        &server_integration_task, 
        "server_integration", 
        SERVER_TASK_STACK_SIZE, 
        NULL, 
        SERVER_TASK_PRIORITY, 
        NULL,
        SERVER_TASK_CORE
    );
    if (server_task_created == pdPASS) {
        ESP_LOGI(TAG, "Server integration task created successfully");
    } else {
        ESP_LOGE(TAG, "Failed to create server integration task");
        return;
    }
    
    // Start WebSocket server task
    ESP_LOGI(TAG, "Creating WebSocket server task...");
    BaseType_t ws_task_created = xTaskCreatePinnedToCore(
        websocket_server_task, 
        "websocket_server", 
        WEBSOCKET_TASK_STACK_SIZE, 
        NULL, 
        WEBSOCKET_TASK_PRIORITY, 
        NULL,
        WEBSOCKET_TASK_CORE
    );
    if (ws_task_created == pdPASS) {
        ESP_LOGI(TAG, "WebSocket server task created successfully");
    } else {
        ESP_LOGE(TAG, "Failed to create WebSocket server task");
        return;
    }
    
    // Start Doom game task after instrumentation is ready
    ESP_LOGI(TAG, "Creating DOOM task...");
    BaseType_t task_created = xTaskCreatePinnedToCore(
        &doom_task, 
        "doom", 
        DOOM_TASK_STACK_SIZE, 
        NULL, 
        DOOM_TASK_PRIORITY, 
        NULL, 
        DOOM_TASK_CORE
    );
    if (task_created == pdPASS) {
        ESP_LOGI(TAG, "DOOM task created successfully on Core %d", DOOM_TASK_CORE);
    } else {
        ESP_LOGE(TAG, "Failed to create DOOM task");
        
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
