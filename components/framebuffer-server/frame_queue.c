#include "frame_queue.h"
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "instrumentation_interface.h"

void frame_queue_init(frame_queue_t *q) {
    memset(q, 0, sizeof(*q));
    for (int i = 0; i < FRAME_QUEUE_DEPTH; i++) {
        // Use PSRAM for frame buffers to save internal memory
        q->frames[i] = heap_caps_malloc(FRAME_SIZE + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
        if (q->frames[i] == NULL) {
            ESP_LOGE("frame_queue", "Failed to allocate frame buffer %d in PSRAM, falling back to internal memory", i);
            q->frames[i] = heap_caps_malloc(FRAME_SIZE + 1, MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
        } else {
            instrumentation_psram_write_operation(FRAME_SIZE + 1);
        }
    }
}

uint8_t *frame_queue_get_write_buffer(frame_queue_t *q) {
    if (q->count >= FRAME_QUEUE_DEPTH) return NULL;
    return q->frames[q->write_index];
}

void frame_queue_submit_frame(frame_queue_t *q) {
    q->write_index = (q->write_index + 1) % FRAME_QUEUE_DEPTH;
    q->count++;
    
    // Track PSRAM write operation for frame submission
    instrumentation_psram_write_operation(FRAME_SIZE);
}

uint8_t *frame_queue_get_next_frame(frame_queue_t *q) {
    if (q->count == 0) return NULL;
    
    // Track PSRAM read operation for frame retrieval
    instrumentation_psram_read_operation(FRAME_SIZE);
    return q->frames[q->read_index];
}

void frame_queue_release_frame(frame_queue_t *q) {
    q->read_index = (q->read_index + 1) % FRAME_QUEUE_DEPTH;
    q->count--;
}
