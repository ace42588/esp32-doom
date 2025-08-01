#include "instrumentation_interface.h"

// Stub implementations for instrumentation functions
// These will be overridden by the main instrumentation system when linked

void instrumentation_psram_read_operation(uint32_t bytes) {
    // Stub implementation - will be overridden by main instrumentation
    (void)bytes;
}

void instrumentation_psram_write_operation(uint32_t bytes) {
    // Stub implementation - will be overridden by main instrumentation
    (void)bytes;
}

void instrumentation_psram_cache_hit(void) {
    // Stub implementation - will be overridden by main instrumentation
}

void instrumentation_psram_cache_miss(void) {
    // Stub implementation - will be overridden by main instrumentation
}

void instrumentation_network_sent_bytes(uint32_t bytes) {
    // Stub implementation - will be overridden by main instrumentation
    (void)bytes;
}

void instrumentation_network_received_bytes(uint32_t bytes) {
    // Stub implementation - will be overridden by main instrumentation
    (void)bytes;
}

void instrumentation_network_sent_packet(void) {
    // Stub implementation - will be overridden by main instrumentation
}

void instrumentation_network_received_packet(void) {
    // Stub implementation - will be overridden by main instrumentation
}

// WebSocket profiling stub
void log_all_websocket_profiles(void) {
    // Stub implementation - will be overridden by websocket server
} 