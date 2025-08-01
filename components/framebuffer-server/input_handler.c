#include <string.h>
#include <stdio.h>
#include <esp_log.h>
#include <esp_err.h>
#include "input_handler.h"

#define TAG "input_handler"

// Global input queue
static QueueHandle_t g_input_queue = NULL;

// WebSocket message types for input
#define WS_MSG_INPUT_KEYDOWN    0x01
#define WS_MSG_INPUT_KEYUP      0x02
#define WS_MSG_INPUT_MOUSE_MOVE 0x03
#define WS_MSG_INPUT_MOUSE_BTN  0x04
#define WS_MSG_INPUT_JOYSTICK   0x05

// DOOM key codes (from doomdef.h)
#define KEYD_RIGHTARROW 0xae
#define KEYD_LEFTARROW  0xac
#define KEYD_UPARROW    0xad
#define KEYD_DOWNARROW  0xaf
#define KEYD_ESCAPE     27
#define KEYD_ENTER      13
#define KEYD_SPACEBAR   0x20
#define KEYD_RCTRL      0x9d
#define KEYD_RSHIFT     0xb6
#define KEYD_RALT       0xb8

// Convert browser key codes to DOOM key codes
static int map_browser_key_to_doom_key(int browser_key_code) {
    switch (browser_key_code) {
        // Arrow keys
        case 37: return KEYD_LEFTARROW;   // Left
        case 38: return KEYD_UPARROW;     // Up
        case 39: return KEYD_RIGHTARROW;  // Right
        case 40: return KEYD_DOWNARROW;   // Down
        
        // WASD keys
        case 65: return 'a';  // A
        case 68: return 'd';  // D
        case 87: return 'w';  // W
        case 83: return 's';  // S
        
        // Control keys
        case 27: return KEYD_ESCAPE;      // Escape
        case 13: return KEYD_ENTER;       // Enter
        case 32: return KEYD_SPACEBAR;    // Space
        case 17: return KEYD_RCTRL;       // Ctrl
        case 16: return KEYD_RSHIFT;      // Shift
        case 18: return KEYD_RALT;        // Alt
        
        // Default: return the browser key code as-is
        default: return browser_key_code;
    }
}

esp_err_t input_handler_init(void) {
    ESP_LOGI(TAG, "Initializing input handler");
    
    g_input_queue = xQueueCreate(INPUT_QUEUE_SIZE, INPUT_QUEUE_ITEM_SIZE);
    if (g_input_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create input queue");
        return ESP_ERR_NO_MEM;
    }
    
    ESP_LOGI(TAG, "Input handler initialized successfully");
    return ESP_OK;
}

void input_handler_deinit(void) {
    if (g_input_queue != NULL) {
        vQueueDelete(g_input_queue);
        g_input_queue = NULL;
    }
    ESP_LOGI(TAG, "Input handler deinitialized");
}

QueueHandle_t input_handler_get_queue(void) {
    return g_input_queue;
}

int input_handler_process_websocket_message(const uint8_t *data, size_t len) {
    if (data == NULL || len < 1) {
        ESP_LOGE(TAG, "Invalid input message: data=%p, len=%zu", data, len);
        return -1;
    }
    
    uint8_t msg_type = data[0];
   
    input_event_t event = {0};
    
    switch (msg_type) {
        case WS_MSG_INPUT_KEYDOWN:
            if (len >= 2) {
                event.type = INPUT_KEYDOWN;
                event.data1 = map_browser_key_to_doom_key(data[1]);
                event.data2 = 0;
                event.data3 = 0;
                ESP_LOGD(TAG, "Key down: browser=%d, doom=%d", data[1], event.data1);
            } else {
                ESP_LOGE(TAG, "Invalid keydown message length: %zu", len);
                return -1;
            }
            break;
            
        case WS_MSG_INPUT_KEYUP:
            if (len >= 2) {
                event.type = INPUT_KEYUP;
                event.data1 = map_browser_key_to_doom_key(data[1]);
                event.data2 = 0;
                event.data3 = 0;
                ESP_LOGD(TAG, "Key up: browser=%d, doom=%d", data[1], event.data1);
            } else {
                ESP_LOGE(TAG, "Invalid keyup message length: %zu", len);
                return -1;
            }
            break;
            
        case WS_MSG_INPUT_MOUSE_MOVE:
            if (len >= 4) {
                event.type = INPUT_MOUSE_MOVE;
                event.data1 = 0; // button mask
                event.data2 = (int8_t)data[1]; // x movement
                event.data3 = (int8_t)data[2]; // y movement
                ESP_LOGD(TAG, "Mouse move: x=%d, y=%d", event.data2, event.data3);
            } else {
                ESP_LOGE(TAG, "Invalid mouse move message length: %zu", len);
                return -1;
            }
            break;
            
        case WS_MSG_INPUT_MOUSE_BTN:
            if (len >= 2) {
                event.type = INPUT_MOUSE_BUTTON;
                event.data1 = data[1]; // button mask
                event.data2 = 0;
                event.data3 = 0;
                ESP_LOGD(TAG, "Mouse button: mask=0x%02x", event.data1);
            } else {
                ESP_LOGE(TAG, "Invalid mouse button message length: %zu", len);
                return -1;
            }
            break;
            
        case WS_MSG_INPUT_JOYSTICK:
            if (len >= 4) {
                event.type = INPUT_JOYSTICK;
                event.data1 = data[1]; // button mask
                event.data2 = (int8_t)data[2]; // x movement
                event.data3 = (int8_t)data[3]; // y movement
                ESP_LOGD(TAG, "Joystick: buttons=0x%02x, x=%d, y=%d", 
                         event.data1, event.data2, event.data3);
            } else {
                ESP_LOGE(TAG, "Invalid joystick message length: %zu", len);
                return -1;
            }
            break;
            
        default:
            ESP_LOGE(TAG, "Unknown input message type: 0x%02x", msg_type);
            return -1;
    }
    
    // Add event to queue
    if (g_input_queue != NULL) {
        if (xQueueSend(g_input_queue, &event, pdMS_TO_TICKS(10)) != pdTRUE) {
            ESP_LOGW(TAG, "Input queue full, dropping event");
            return -1;
        }
    } else {
        ESP_LOGE(TAG, "Input queue not initialized");
        return -1;
    }
    
    return 0;
} 