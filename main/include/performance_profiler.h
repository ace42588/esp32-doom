#ifndef PERFORMANCE_PROFILER_H
#define PERFORMANCE_PROFILER_H

#include <stdint.h>
#include <stddef.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Performance profiling macros and structures
#define MAX_PROFILE_SECTIONS 32
#define PROFILE_HISTORY_SIZE 60  // Keep 60 frames of history

typedef struct {
    const char* name;
    uint32_t start_time;
    uint32_t total_time;
    uint32_t call_count;
    uint32_t min_time;
    uint32_t max_time;
    uint32_t history[PROFILE_HISTORY_SIZE];
    uint32_t history_index;
} profile_section_t;

typedef struct {
    profile_section_t sections[MAX_PROFILE_SECTIONS];
    uint32_t section_count;
    uint32_t frame_start_time;
    uint32_t frame_count;
    uint32_t frame_times[PROFILE_HISTORY_SIZE];
    uint32_t frame_time_index;
} performance_profiler_t;

extern performance_profiler_t g_profiler;

// Initialize the profiler
void profiler_init(void);

// Start profiling a section
void profiler_start_section(const char* name);

// End profiling a section
void profiler_end_section(const char* name);

// Start frame profiling
void profiler_start_frame(void);

// End frame profiling and print stats
void profiler_end_frame(void);

// Print detailed profiling statistics
void profiler_print_stats(void);

// Reset all profiling data
void profiler_reset(void);

// Macro for automatic section profiling
#define PROFILE_SECTION(name) \
    profiler_start_section(name); \
    for (int _profile_done = 0; !_profile_done; _profile_done = 1, profiler_end_section(name))

// Macro for function profiling
#define PROFILE_FUNCTION() PROFILE_SECTION(__FUNCTION__)

#endif // PERFORMANCE_PROFILER_H 