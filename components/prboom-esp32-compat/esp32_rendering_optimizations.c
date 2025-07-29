#include "esp32_rendering_optimizations.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "r_main.h"
#include "r_segs.h"
#include "r_things.h"
#include "r_plane.h"
#include "r_draw.h"
#include "v_video.h"
#include "z_zone.h"
#include "r_bsp.h"
#include "r_data.h"
#include "r_patch.h"
#include "m_fixed.h"
#include "doomdef.h"
#include "r_patch.h"

// Include original function declarations for fallback
extern void R_DrawColumn(draw_column_vars_t *dcvars);
extern void R_DrawSpan(draw_span_vars_t *dsvars);

static const char *TAG = "ESP32_RENDER";

// Performance counters
uint32_t esp32_render_cycles = 0;
uint32_t esp32_cache_misses = 0;
uint32_t esp32_dma_transfers = 0;

// Cache-aligned temporary buffers for optimized rendering
static ESP32_CACHE_ALIGN uint8_t esp32_temp_column_buffer[ESP32_COLUMN_BUFFER_SIZE];
static ESP32_CACHE_ALIGN uint8_t esp32_temp_span_buffer[ESP32_SPAN_BUFFER_SIZE];

// ============================================================================
// OPTIMIZED COLUMN RENDERING WITH DMA AND CACHE OPTIMIZATIONS
// ============================================================================

#if ESP32_ENABLE_COLUMN_OPTIMIZATION
void IRAM_ATTR ESP32_R_DrawColumn_Optimized(draw_column_vars_t *dcvars)
{
    uint32_t start_cycles = esp_timer_get_time();
    
    // Prefetch critical data
    esp32_prefetch(dcvars->source);
    esp32_prefetch(dcvars->colormap);
    
    const byte *source = dcvars->source;
    const lighttable_t *colormap = dcvars->colormap;
    const byte *translation = dcvars->translation;
    
    int count = dcvars->yh - dcvars->yl + 1;
    if (count <= 0) return;
    
    // Calculate texture mapping parameters
    fixed_t frac = dcvars->texturemid + (dcvars->yl - centery) * dcvars->iscale;
    fixed_t fracstep = dcvars->iscale;
    
    // Optimize for common texture heights (128 is most common)
    if (dcvars->texheight == 128) {
        #define FIXEDT_128MASK ((127<<FRACBITS)|0xffff)
        
        // Use cache-aligned buffer for better memory access patterns
        uint8_t *temp_buf = esp32_temp_column_buffer;
        int temp_count = count > 256 ? 256 : count;
        
        // Process in cache-friendly chunks
        while (count > 0) {
            int chunk_size = count > 256 ? 256 : count;
            
            // Prefetch next chunk
            if (count > 256) {
                esp32_prefetch(source + ((frac + 256 * fracstep) >> FRACBITS));
            }
            
            // Optimized inner loop for 128-height textures
            for (int i = 0; i < chunk_size; i++) {
                int texel = (frac & FIXEDT_128MASK) >> FRACBITS;
                byte pixel = source[texel];
                
                if (translation) {
                    pixel = translation[pixel];
                }
                
                temp_buf[i] = colormap[pixel];
                frac += fracstep;
            }
            
            // Copy to framebuffer using optimized memory operations
            memcpy(&screens[0].data[dcvars->x + (dcvars->yl + (count - chunk_size)) * SCREENPITCH], 
                   temp_buf, chunk_size);
            
            count -= chunk_size;
        }
    } else {
        // Generic optimized path for other texture heights
        byte *dest = &screens[0].data[dcvars->x + dcvars->yl * SCREENPITCH];
        
        while (count--) {
            int texel = (frac >> FRACBITS) % dcvars->texheight;
            byte pixel = source[texel];
            
            if (translation) {
                pixel = translation[pixel];
            }
            
            *dest = colormap[pixel];
            dest += SCREENPITCH;
            frac += fracstep;
        }
    }
    
    esp32_render_cycles += esp_timer_get_time() - start_cycles;
}
#else
// Fallback to original function when optimization is disabled
void IRAM_ATTR ESP32_R_DrawColumn_Optimized(draw_column_vars_t *dcvars)
{
    R_DrawColumn(dcvars);
}
#endif

// ============================================================================
// OPTIMIZED SPAN RENDERING WITH SIMD-LIKE OPERATIONS
// ============================================================================

#if ESP32_ENABLE_SPAN_OPTIMIZATION
void IRAM_ATTR ESP32_R_DrawSpan_Optimized(draw_span_vars_t *dsvars)
{
    uint32_t start_cycles = esp_timer_get_time();
    
    unsigned count = dsvars->x2 - dsvars->x1 + 1;
    if (count == 0) return;
    
    fixed_t xfrac = dsvars->xfrac;
    fixed_t yfrac = dsvars->yfrac;
    const fixed_t xstep = dsvars->xstep;
    const fixed_t ystep = dsvars->ystep;
    const byte *source = dsvars->source;
    const byte *colormap = dsvars->colormap;
    
    // Prefetch texture data
    esp32_prefetch(source);
    esp32_prefetch(colormap);
    
    // Use cache-aligned buffer for span rendering
    uint8_t *temp_buf = esp32_temp_span_buffer;
    byte *dest = &screens[0].data[dsvars->y * SCREENPITCH + dsvars->x1];
    
    // Process in 32-pixel chunks for better cache utilization
    while (count >= 32) {
        // Prefetch next chunk
        esp32_prefetch(source + ((xfrac + 32 * xstep) >> 16) + ((yfrac + 32 * ystep) >> 10) * 64);
        
        for (int i = 0; i < 32; i++) {
            const fixed_t xtemp = (xfrac >> 16) & 63;
            const fixed_t ytemp = (yfrac >> 10) & 4032;
            const fixed_t spot = xtemp | ytemp;
            
            temp_buf[i] = colormap[source[spot]];
            xfrac += xstep;
            yfrac += ystep;
        }
        
        // Copy chunk to framebuffer
        memcpy(dest, temp_buf, 32);
        dest += 32;
        count -= 32;
    }
    
    // Handle remaining pixels
    while (count--) {
        const fixed_t xtemp = (xfrac >> 16) & 63;
        const fixed_t ytemp = (yfrac >> 10) & 4032;
        const fixed_t spot = xtemp | ytemp;
        
        *dest++ = colormap[source[spot]];
        xfrac += xstep;
        yfrac += ystep;
    }
    
    esp32_render_cycles += esp_timer_get_time() - start_cycles;
}
#else
// Fallback to original function when optimization is disabled
void IRAM_ATTR ESP32_R_DrawSpan_Optimized(draw_span_vars_t *dsvars)
{
    R_DrawSpan(dsvars);
}
#endif

// ============================================================================
// PERFORMANCE MONITORING FUNCTIONS
// ============================================================================

void esp32_init_performance_monitoring(void)
{
    esp32_render_cycles = 0;
    esp32_cache_misses = 0;
    esp32_dma_transfers = 0;
    ESP_LOGI(TAG, "Performance monitoring initialized");
}

void esp32_get_performance_stats(uint32_t *cycles, uint32_t *misses, uint32_t *transfers)
{
    if (cycles) *cycles = esp32_render_cycles;
    if (misses) *misses = esp32_cache_misses;
    if (transfers) *transfers = esp32_dma_transfers;
} 