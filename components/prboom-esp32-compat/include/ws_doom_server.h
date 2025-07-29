#ifndef WS_DOOM_SERVER_H
#define WS_DOOM_SERVER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void ws_broadcast_framebuffer(const void *data, size_t len, uint8_t palette_index);

#ifdef __cplusplus
}
#endif

#endif // WS_DOOM_SERVER_H 