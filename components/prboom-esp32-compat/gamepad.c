
#include <stdlib.h>

#include "doomdef.h"
#include "doomtype.h"
#include "m_argv.h"
#include "d_event.h"
#include "g_game.h"
#include "d_main.h"
#include "gamepad.h"
#include "lprintf.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "input_handler.h"
#include "esp_log.h"

#define TAG "gamepad"

// Joystick axis mappings (stub values for ESP32) - these are missing from PrBoom
int joyleft = 0;   // Left direction
int joyright = 0;  // Right direction
int joyup = 0;     // Up direction
int joydown = 0;   // Down direction

// Joystick enable flag (disabled for ESP32) - this is missing from PrBoom
int usejoystick = 0;

// Joystick button state variables (missing from PrBoom)
int joybuttons[4] = {0, 0, 0, 0};
int joyxmove = 0;
int joyymove = 0;

void gamepadInit(void)
{
}

void gamepadPoll(void)
{
    static QueueHandle_t input_queue = NULL;
    input_event_t input_event;
    event_t ev;
    
    // Get input queue handle (lazy initialization)
    if (input_queue == NULL) {
        input_queue = input_handler_get_queue();
        if (input_queue == NULL) {
            return; // Input handler not initialized yet
        }
    }
    
    // Process all available input events
    while (xQueueReceive(input_queue, &input_event, 0) == pdTRUE) {
        switch (input_event.type) {
            case INPUT_KEYDOWN:
                ev.type = ev_keydown;
                ev.data1 = input_event.data1;
                ev.data2 = 0;
                ev.data3 = 0;
                D_PostEvent(&ev);
                break;
                
            case INPUT_KEYUP:
                ev.type = ev_keyup;
                ev.data1 = input_event.data1;
                ev.data2 = 0;
                ev.data3 = 0;
                D_PostEvent(&ev);
                break;
                
            case INPUT_MOUSE_MOVE:
                ev.type = ev_mouse;
                ev.data1 = 0; // button mask (handled separately)
                ev.data2 = input_event.data2; // x movement
                ev.data3 = input_event.data3; // y movement

                D_PostEvent(&ev);
                break;
                
            case INPUT_MOUSE_BUTTON:
                ev.type = ev_mouse;
                ev.data1 = input_event.data1; // button mask
                ev.data2 = 0;
                ev.data3 = 0;
                D_PostEvent(&ev);
                break;
                
            case INPUT_JOYSTICK:
                ev.type = ev_joystick;
                ev.data1 = input_event.data1; // button mask
                ev.data2 = input_event.data2; // x movement
                ev.data3 = input_event.data3; // y movement
                D_PostEvent(&ev);
                break;
        }
    }
}