#pragma once
#include <stdint.h>
#include <stddef.h>

#define FRAME_WIDTH  320
#define FRAME_HEIGHT 240
#define FRAME_BPP    1     // 8bpp
#define FRAME_SIZE   (FRAME_WIDTH * FRAME_HEIGHT)
#define FRAME_QUEUE_DEPTH 2

typedef struct {
    uint8_t *frames[FRAME_QUEUE_DEPTH];
    volatile int write_index;
    volatile int read_index;
    volatile int count;
} frame_queue_t;

void frame_queue_init(frame_queue_t *q);
uint8_t *frame_queue_get_write_buffer(frame_queue_t *q);
void frame_queue_submit_frame(frame_queue_t *q);
uint8_t *frame_queue_get_next_frame(frame_queue_t *q);
void frame_queue_release_frame(frame_queue_t *q);
