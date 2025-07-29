#ifndef SIMPLE_PROFILER_H
#define SIMPLE_PROFILER_H

#include <stdint.h>
#include <stddef.h>
#include "esp_log.h"
#include "esp_timer.h"

// Simple profiling macros that can be used anywhere
// These use esp_timer for microsecond precision timing
#define SIMPLE_PROFILE_SECTION(name) \
    uint64_t _profile_start_##name = esp_timer_get_time(); \
    for (int _profile_done = 0; !_profile_done; _profile_done = 1) { \
        uint64_t _profile_duration = esp_timer_get_time() - _profile_start_##name; \
        if (_profile_duration > 0) { \
            ESP_LOGI("PROFILE", "%s: %llu us", #name, _profile_duration); \
        } \
        break; \
    }

// Frame timing macros
#define FRAME_START() \
    uint64_t _frame_start = esp_timer_get_time()

#define FRAME_END() \
    { \
        uint64_t _frame_duration = esp_timer_get_time() - _frame_start; \
        static uint32_t _frame_count = 0; \
        _frame_count++; \
        ESP_LOGI("FRAME", "Frame %lu: %llu us (%.1f FPS)", _frame_count, _frame_duration, 1000000.0f / _frame_duration); \
    }

// Hierarchical profiling - track cumulative time for functions called many times
#define HIERARCHICAL_PROFILE_START(name) \
    static uint64_t _hierarchical_total_##name = 0; \
    static uint32_t _hierarchical_count_##name = 0; \
    uint64_t _hierarchical_start_##name = esp_timer_get_time()

#define HIERARCHICAL_PROFILE_END(name) \
    { \
        uint64_t _hierarchical_duration = esp_timer_get_time() - _hierarchical_start_##name; \
        _hierarchical_total_##name += _hierarchical_duration; \
        _hierarchical_count_##name++; \
        if (_hierarchical_count_##name % 100 == 0) { \
            ESP_LOGI("HIERARCHICAL", "%s: %llu us total, %lu calls, %llu us avg", \
                     #name, _hierarchical_total_##name, _hierarchical_count_##name, \
                     _hierarchical_total_##name / _hierarchical_count_##name); \
        } \
    }

// Sampling profiler - periodically log which function we're in
#define SAMPLING_PROFILE(name) \
    static uint32_t _sampling_counter_##name = 0; \
    if (++_sampling_counter_##name % 1000 == 0) { \
        ESP_LOGI("SAMPLING", "Currently in: %s", #name); \
    }

#endif // SIMPLE_PROFILER_H 