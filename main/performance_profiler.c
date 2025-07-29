#include "performance_profiler.h"
#include <string.h>
#include <stdio.h>
#include "esp_timer.h"

static const char* TAG = "PROFILER";

// Global profiler instance
performance_profiler_t g_profiler = {0};

void profiler_init(void) {
    memset(&g_profiler, 0, sizeof(g_profiler));
    ESP_LOGI(TAG, "Performance profiler initialized");
}

static uint32_t get_time_us(void) {
    return esp_timer_get_time();
}

static profile_section_t* find_or_create_section(const char* name) {
    // Find existing section
    for (uint32_t i = 0; i < g_profiler.section_count; i++) {
        if (strcmp(g_profiler.sections[i].name, name) == 0) {
            return &g_profiler.sections[i];
        }
    }
    
    // Create new section
    if (g_profiler.section_count < MAX_PROFILE_SECTIONS) {
        profile_section_t* section = &g_profiler.sections[g_profiler.section_count++];
        section->name = name;
        section->min_time = UINT32_MAX;
        return section;
    }
    
    ESP_LOGW(TAG, "Too many profile sections, cannot add: %s", name);
    return NULL;
}

void profiler_start_section(const char* name) {
    profile_section_t* section = find_or_create_section(name);
    if (section) {
        section->start_time = get_time_us();
    }
}

void profiler_end_section(const char* name) {
    profile_section_t* section = find_or_create_section(name);
    if (section && section->start_time > 0) {
        uint32_t duration = get_time_us() - section->start_time;
        
        section->total_time += duration;
        section->call_count++;
        
        if (duration < section->min_time) section->min_time = duration;
        if (duration > section->max_time) section->max_time = duration;
        
        // Add to history
        section->history[section->history_index] = duration;
        section->history_index = (section->history_index + 1) % PROFILE_HISTORY_SIZE;
        
        section->start_time = 0; // Reset start time
    }
}

void profiler_start_frame(void) {
    g_profiler.frame_start_time = get_time_us();
}

void profiler_end_frame(void) {
    uint32_t frame_time = get_time_us() - g_profiler.frame_start_time;
    g_profiler.frame_count++;
    
    // Add frame time to history
    g_profiler.frame_times[g_profiler.frame_time_index] = frame_time;
    g_profiler.frame_time_index = (g_profiler.frame_time_index + 1) % PROFILE_HISTORY_SIZE;
    
    // Print stats every 60 frames (about once per second at 60fps)
    if (g_profiler.frame_count % 60 == 0) {
        profiler_print_stats();
    }
}

void profiler_print_stats(void) {
    ESP_LOGI(TAG, "=== PERFORMANCE STATS (Frame %lu) ===", g_profiler.frame_count);
    
    // Calculate average frame time
    uint32_t total_frame_time = 0;
    for (int i = 0; i < PROFILE_HISTORY_SIZE; i++) {
        total_frame_time += g_profiler.frame_times[i];
    }
    uint32_t avg_frame_time = total_frame_time / PROFILE_HISTORY_SIZE;
    float fps = 1000000.0f / avg_frame_time;
    
    ESP_LOGI(TAG, "Frame Time: %lu us avg, %.1f FPS", avg_frame_time, fps);
    
    // Sort sections by total time (descending)
    profile_section_t* sorted_sections[MAX_PROFILE_SECTIONS];
    for (uint32_t i = 0; i < g_profiler.section_count; i++) {
        sorted_sections[i] = &g_profiler.sections[i];
    }
    
    // Simple bubble sort
    for (uint32_t i = 0; i < g_profiler.section_count - 1; i++) {
        for (uint32_t j = 0; j < g_profiler.section_count - i - 1; j++) {
            if (sorted_sections[j]->total_time < sorted_sections[j + 1]->total_time) {
                profile_section_t* temp = sorted_sections[j];
                sorted_sections[j] = sorted_sections[j + 1];
                sorted_sections[j + 1] = temp;
            }
        }
    }
    
    // Print top sections
    ESP_LOGI(TAG, "Top performance sections:");
    for (uint32_t i = 0; i < g_profiler.section_count && i < 10; i++) {
        profile_section_t* section = sorted_sections[i];
        uint32_t avg_time = section->call_count > 0 ? section->total_time / section->call_count : 0;
        float percentage = (float)section->total_time / avg_frame_time * 100.0f;
        
        ESP_LOGI(TAG, "  %s: %lu us total, %lu calls, %lu us avg, %.1f%% of frame", 
                 section->name, section->total_time, section->call_count, avg_time, percentage);
    }
    
    ESP_LOGI(TAG, "=== END STATS ===");
}

void profiler_reset(void) {
    memset(&g_profiler, 0, sizeof(g_profiler));
    ESP_LOGI(TAG, "Performance profiler reset");
} 