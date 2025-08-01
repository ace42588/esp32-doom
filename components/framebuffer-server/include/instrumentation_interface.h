#ifndef INSTRUMENTATION_INTERFACE_H
#define INSTRUMENTATION_INTERFACE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// PSRAM bandwidth tracking functions
void instrumentation_psram_read_operation(uint32_t bytes);
void instrumentation_psram_write_operation(uint32_t bytes);
void instrumentation_psram_cache_hit(void);
void instrumentation_psram_cache_miss(void);

// Network throughput tracking functions
void instrumentation_network_sent_bytes(uint32_t bytes);
void instrumentation_network_received_bytes(uint32_t bytes);
void instrumentation_network_sent_packet(void);
void instrumentation_network_received_packet(void);

// WebSocket profiling functions
void log_all_websocket_profiles(void);

#ifdef __cplusplus
}
#endif

#endif // INSTRUMENTATION_INTERFACE_H 