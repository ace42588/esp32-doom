#ifndef ESP32_RENDERING_OPTIMIZATIONS_H
#define ESP32_RENDERING_OPTIMIZATIONS_H

#include "esp_attr.h"
#include "r_draw.h"
#include "r_defs.h"

// ============================================================================
// ESP32-SPECIFIC RENDERING OPTIMIZATIONS
// ============================================================================

#define ESP32_COLUMN_BUFFER_SIZE 320
#define ESP32_SPAN_BUFFER_SIZE 256

// DMA-optimized column rendering with cache-friendly memory access
void IRAM_ATTR ESP32_R_DrawColumn_Optimized(draw_column_vars_t *dcvars);

// SIMD-optimized span rendering for floor/ceiling
void IRAM_ATTR ESP32_R_DrawSpan_Optimized(draw_span_vars_t *dsvars);

// ============================================================================
// WRAPPER FUNCTIONS FOR EASY INTEGRATION
// ============================================================================

// Wrapper functions that automatically choose optimized or original versions
#if ESP32_ENABLE_COLUMN_OPTIMIZATION
#define R_DrawColumn_ESP32 ESP32_R_DrawColumn_Optimized
#else
#define R_DrawColumn_ESP32 R_DrawColumn
#endif

#if ESP32_ENABLE_SPAN_OPTIMIZATION
#define R_DrawSpan_ESP32 ESP32_R_DrawSpan_Optimized
#else
#define R_DrawSpan_ESP32 R_DrawSpan
#endif

// ============================================================================
// MEMORY OPTIMIZATION HELPERS
// ============================================================================

// Align data structures for optimal cache performance
#if ESP32_ENABLE_CACHE_ALIGNMENT
#define ESP32_CACHE_ALIGN __attribute__((aligned(32)))
#else
#define ESP32_CACHE_ALIGN
#endif

// Prefetch data for rendering operations
#if ESP32_ENABLE_PREFETCHING
static inline void esp32_prefetch(const void *ptr) {
    __builtin_prefetch(ptr, 0, 3); // Read, high locality
}
#else
static inline void esp32_prefetch(const void *ptr) {
    (void)ptr; // No-op when disabled
}
#endif

// Branch prediction hints for ESP32
#if ESP32_ENABLE_BRANCH_PREDICTION
#define ESP32_LIKELY(x) __builtin_expect(!!(x), 1)
#define ESP32_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define ESP32_LIKELY(x) (x)
#define ESP32_UNLIKELY(x) (x)
#endif

// ============================================================================
// PERFORMANCE MONITORING
// ============================================================================

// Performance counters for optimization analysis
extern uint32_t esp32_render_cycles;
extern uint32_t esp32_cache_misses;
extern uint32_t esp32_dma_transfers;

// Initialize performance monitoring
void esp32_init_performance_monitoring(void);

// Get performance statistics
void esp32_get_performance_stats(uint32_t *cycles, uint32_t *misses, uint32_t *transfers);

#endif // ESP32_RENDERING_OPTIMIZATIONS_H 