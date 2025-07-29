# Refactored ESP32 Doom WebSocket Server Architecture

## Overview

The ESP32 Doom WebSocket Server has been completely refactored to eliminate circular dependencies and provide clear separation of responsibilities. The new architecture follows the Single Responsibility Principle and provides clean interfaces between modules.

## Architecture Overview

```
┌─────────────────┐    ┌──────────────────┐    ┌─────────────────┐
│   Main App      │    │  WebSocket       │    │  Network        │
│                 │    │  Server          │    │  Transmission   │
└─────────────────┘    └──────────────────┘    └─────────────────┘
         │                       │                       │
         │                       │                       │
         ▼                       ▼                       ▼
┌─────────────────┐    ┌──────────────────┐    ┌─────────────────┐
│ Frame           │    │ HTTP Handlers    │    │ Buffer Pool     │
│ Broadcaster     │    │                  │    │ Management      │
└─────────────────┘    └──────────────────┘    └─────────────────┘
```

## Module Responsibilities

### 1. Main Application (`main.c`)
- **Responsibility**: Application entry point and coordination
- **Dependencies**: All other modules
- **Key Functions**:
  - Initialize ESP-IDF components
  - Connect to WiFi
  - Initialize all modules
  - Register network event handlers
  - Start Doom game task

### 2. WebSocket Server (`websocket_server.c`)
- **Responsibility**: WebSocket server management and client handling
- **Dependencies**: HTTP handlers (for static file serving)
- **Key Functions**:
  - WebSocket server initialization and lifecycle
  - Client connection management
  - WebSocket frame handling (binary and fragmented)
  - HTTP static file serving
  - Network event handling

### 3. Network Transmission (`network_transmission.c`)
- **Responsibility**: Asynchronous frame transmission and buffer management
- **Dependencies**: WebSocket server (for sending frames)
- **Key Functions**:
  - Buffer pool management (PSRAM-first allocation)
  - Asynchronous frame queuing and transmission
  - Frame fragmentation for large frames
  - Network transmission task management
  - Statistics and monitoring

### 4. HTTP Handlers (`http_handlers.c`)
- **Responsibility**: HTTP request handling and static file serving
- **Dependencies**: None (independent module)
- **Key Functions**:
  - Static file loading and caching
  - HTTP request handlers for HTML and JS files
  - Content type detection
  - File streaming utilities

### 5. Frame Broadcaster (`frame_broadcaster.c`)
- **Responsibility**: Simple interface for Doom game to send frames
- **Dependencies**: WebSocket server and network transmission
- **Key Functions**:
  - `broadcast_framebuffer()` - Main interface for Doom
  - Client connection status checking
  - Frame validation and error handling

## Key Improvements

### 1. Eliminated Circular Dependencies
- **Before**: WebSocket server ↔ Network transmission circular dependency
- **After**: Clear unidirectional dependencies with clean interfaces

### 2. Clear Separation of Concerns
- **WebSocket Server**: Only handles WebSocket protocol and client management
- **Network Transmission**: Only handles frame transmission and buffering
- **HTTP Handlers**: Only handles HTTP requests and static files
- **Frame Broadcaster**: Only provides simple interface for Doom game

### 3. Proper Initialization Order
1. ESP-IDF components (NVS, netif, event loop)
2. WiFi connection
3. WebSocket server initialization
4. Network transmission initialization
5. Event handler registration
6. Doom game task creation

### 4. Network Event Handling
- **Connect Event**: Automatically starts WebSocket server when IP is assigned
- **Disconnect Event**: Automatically stops WebSocket server when network is lost

### 5. Memory Management
- **PSRAM-First Allocation**: Uses external RAM when available
- **Buffer Pool**: Efficient buffer reuse for network transmission
- **Static File Caching**: HTML and JS files loaded to PSRAM

## Frame Transmission Flow

```
Doom Game → Frame Broadcaster → Network Transmission → WebSocket Server → Clients
```

1. **Doom Game** calls `broadcast_framebuffer()`
2. **Frame Broadcaster** validates and queues frame
3. **Network Transmission** processes frame asynchronously
4. **WebSocket Server** sends frame to connected clients
5. **Clients** receive frame via WebSocket

## Fragmentation Support

Large frames (>16KB) are automatically fragmented:
- **First chunk**: Contains palette index + data
- **Subsequent chunks**: Contain only data
- **Proper WebSocket fragmentation**: Uses HTTPD_WS_TYPE_CONTINUE frames
- **Reliable transmission**: Small delays between chunks

## Configuration

### WebSocket Server
- `MAX_WS_CLIENTS`: 4 (configurable)
- Server starts automatically when network connects
- Supports both binary and fragmented frames

### Network Transmission
- `NETWORK_QUEUE_SIZE`: 64 frames
- `BUFFER_POOL_SIZE`: 16 buffers
- `MAX_BUFFER_SIZE`: 32KB
- `FRAGMENT_SIZE`: 16KB

### Buffer Management
- PSRAM-first allocation when available
- Fallback to internal RAM
- Automatic buffer pool management
- Memory-efficient buffer reuse

## Usage Example

```c
// In Doom game code
#include "frame_broadcaster.h"

// Send a frame to all connected clients
esp_err_t ret = broadcast_framebuffer(framebuffer_data, framebuffer_size, palette_index);
if (ret != ESP_OK) {
    // Handle error
}

// Check if clients are connected
if (has_connected_clients()) {
    // Clients are connected
}
```

## Benefits of Refactored Architecture

1. **Maintainability**: Clear module boundaries and responsibilities
2. **Testability**: Each module can be tested independently
3. **Scalability**: Easy to add new features or modify existing ones
4. **Reliability**: Proper error handling and state management
5. **Performance**: Efficient memory usage and asynchronous processing
6. **Debugging**: Clear logging and error reporting per module

## Migration Notes

- All old function names have been updated with clear prefixes
- Global variables have been encapsulated in module structures
- Network event handlers are now properly scoped
- Frame broadcasting interface is simplified and clean
- No more circular dependencies or mixed responsibilities 