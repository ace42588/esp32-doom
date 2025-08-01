#pragma once

#include <stdint.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

// Input event types
typedef enum {
    INPUT_KEYDOWN,
    INPUT_KEYUP,
    INPUT_MOUSE_MOVE,
    INPUT_MOUSE_BUTTON,
    INPUT_JOYSTICK
} input_event_type_t;

// Input event structure
typedef struct {
    input_event_type_t type;
    int data1;    // key code / button mask
    int data2;    // x movement / button state
    int data3;    // y movement
} input_event_t;

// Input handler configuration
#define INPUT_QUEUE_SIZE 32
#define INPUT_QUEUE_ITEM_SIZE sizeof(input_event_t)

// Function declarations
esp_err_t input_handler_init(void);
void input_handler_deinit(void);
QueueHandle_t input_handler_get_queue(void);
int input_handler_process_websocket_message(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif 