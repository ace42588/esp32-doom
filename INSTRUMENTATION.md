# ESP32 Doom Instrumentation System

This document describes the lightweight instrumentation system added to the ESP32 Doom project for monitoring system performance.

## Overview

The instrumentation system provides periodic logging of:
- **CPU Contention**: Task runtime statistics and CPU usage percentages
- **Memory Bus Contention**: Free heap, PSRAM usage, and largest free blocks
- **Task Scheduling**: Stack usage, priorities, and task switching patterns
- **WiFi Throughput**: Bytes sent/received, packets, and throughput rates

## Features

### Automatic Configuration Logging
At startup, the system logs:
- CPU frequency and flash size
- PSRAM availability and WiFi mode
- Task stack sizes and FreeRTOS configuration
- Heap configuration and available memory

### Periodic Performance Reports
Every 5 seconds (configurable), the system logs:
- Memory usage statistics with percentages
- Task runtime statistics showing CPU usage per task
- Task stack usage with high water marks
- WiFi throughput with KB/s rates
- Packet counts for network analysis

### WiFi Throughput Tracking
The system tracks WiFi performance at the driver level:
- **Driver-level statistics**: Actual bytes/packets sent/received by the WiFi driver
- **Application-level statistics**: Bytes/packets sent/received by your application
- **Utilization analysis**: Shows if your app is using the full WiFi capacity
- **Error tracking**: TX/RX errors, retries, and dropped packets
- **Connection quality**: RSSI, channel, and PHY mode information

## Configuration

### Logging Interval
Change `INSTRUMENTATION_INTERVAL_MS` in `main/include/instrumentation.h`:
```c
#define INSTRUMENTATION_INTERVAL_MS 5000  // Log every 5 seconds
```

### Task Stack Size
Adjust the instrumentation task stack size:
```c
#define INSTRUMENTATION_TASK_STACK_SIZE 4096
```

### Task Priority
Modify the instrumentation task priority:
```c
#define INSTRUMENTATION_TASK_PRIORITY 1
```

## Sample Output

```
I (1234) Instrumentation: === SYSTEM CONFIGURATION ===
I (1234) Instrumentation: CPU Frequency: 240 MHz
I (1234) Instrumentation: Flash Size: 4 MB
I (1234) Instrumentation: PSRAM Enabled: Yes
I (1234) Instrumentation: WiFi Mode: 1
I (1234) Instrumentation: Doom Task Stack: 32768 bytes
I (1234) Instrumentation: Server Task Stack: 8192 bytes

I (5678) Instrumentation: ==================================================
I (5678) Instrumentation: PERIODIC INSTRUMENTATION REPORT
I (5678) Instrumentation: ==================================================
I (5678) Instrumentation: === MEMORY STATS ===
I (5678) Instrumentation: Free Internal RAM: 123456 bytes (85.2% of min)
I (5678) Instrumentation: Min Free Internal RAM: 145000 bytes
I (5678) Instrumentation: Largest Free Block: 98765 bytes
I (5678) Instrumentation: Free PSRAM: 2097152 bytes (50.0% of total)
I (5678) Instrumentation: Total PSRAM: 4194304 bytes

I (5678) Instrumentation: === TASK RUNTIME STATS ===
I (5678) Instrumentation: Task Name          Runtime  Count
I (5678) Instrumentation: doom               45.2%    1234
I (5678) Instrumentation: server_integration 12.1%    567
I (5678) Instrumentation: Idle               42.7%    2345

I (5678) Instrumentation: === TASK STACK USAGE ===
I (5678) Instrumentation: Task: doom          | Stack: 24567/32768 bytes (75.0%) | Priority: 3
I (5678) Instrumentation: Task: server_integration | Stack: 6789/8192 bytes (82.9%) | Priority: 2

I (5678) Instrumentation: === WIFI THROUGHPUT STATS ===
I (5678) Instrumentation: Time period: 5000 ms
I (5678) Instrumentation: App Bytes sent: 1048576 (209.72 KB/s)
I (5678) Instrumentation: App Bytes received: 51200 (10.24 KB/s)
I (5678) Instrumentation: App Packets sent: 1024
I (5678) Instrumentation: App Packets received: 128
I (5678) Instrumentation: WiFi Driver Stats:
I (5678) Instrumentation:   TX Bytes: 1153024 (230.60 KB/s)
I (5678) Instrumentation:   RX Bytes: 61440 (12.29 KB/s)
I (5678) Instrumentation:   TX Packets: 1120
I (5678) Instrumentation:   RX Packets: 140
I (5678) Instrumentation:   TX Errors: 0
I (5678) Instrumentation:   RX Errors: 0
I (5678) Instrumentation:   TX Retries: 5
I (5678) Instrumentation:   RX Dropped: 2
I (5678) Instrumentation:   RSSI: -45 dBm
I (5678) Instrumentation:   Channel: 6
I (5678) Instrumentation:   PHY Mode: 3
I (5678) Instrumentation: WiFi Utilization Analysis:
I (5678) Instrumentation:   App TX Utilization: 91.0%
I (5678) Instrumentation:   App RX Utilization: 83.3%
I (5678) Instrumentation:   -> More WiFi capacity available for TX
I (5678) Instrumentation:   -> More WiFi capacity available for RX
```

## Integration Points

### WiFi Driver Integration
The system hooks directly into the ESP32 WiFi driver to track:
- Actual WiFi driver TX/RX statistics
- Network errors, retries, and dropped packets
- Connection quality metrics (RSSI, channel, PHY mode)
- Utilization analysis to identify available capacity

### Memory Monitoring
The system monitors:
- Internal RAM usage and fragmentation
- PSRAM availability and usage
- Heap allocation patterns

### Task Monitoring
FreeRTOS integration provides:
- Runtime statistics for all tasks
- Stack usage with high water marks
- Task priority and scheduling information

## Building and Running

1. Build the project with instrumentation:
   ```bash
   idf.py build
   ```

2. Flash to ESP32:
   ```bash
   idf.py flash
   ```

3. Monitor serial output:
   ```bash
   idf.py monitor
   ```

The instrumentation system starts automatically and logs to the serial console every 5 seconds.

## Troubleshooting

### No Instrumentation Output
- Check that `instrumentation.c` is included in the build
- Verify the instrumentation task is created successfully
- Ensure serial logging is enabled

### High Memory Usage
- Reduce `INSTRUMENTATION_TASK_STACK_SIZE` if needed
- Increase logging interval to reduce overhead
- Monitor the instrumentation task's own memory usage

### WiFi Tracking Issues
- Verify WiFi driver statistics are being collected
- Check that `esp_wifi_get_stats()` returns valid data
- Ensure WiFi connection is established before tracking starts
- Monitor RSSI and error rates for connection quality issues 