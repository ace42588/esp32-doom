#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

// Server integration configuration
#define HTTP_SERVER_PORT 80
#define HTTP_SERVER_MAX_URI_HANDLERS 8

// Function declarations
esp_err_t server_integration_init(void);
esp_err_t server_integration_start(void);
esp_err_t server_integration_start_websocket(void);
void server_integration_stop(void);
void server_integration_task(void *pv);

// WebSocket upgrade handler
esp_err_t server_websocket_upgrade_handler(httpd_req_t *req);

#ifdef __cplusplus
}
#endif 