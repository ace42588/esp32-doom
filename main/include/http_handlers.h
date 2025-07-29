#ifndef HTTP_HANDLERS_H
#define HTTP_HANDLERS_H

#include "esp_http_server.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Web page paths
#define INDEX_HTML_PATH "/spiffs/index.html"
#define DOOM_PALETTE_JS_PATH "/spiffs/doom-palette.js"

/**
 * @brief Handle HTTP GET requests for the root path
 * @param req HTTP request structure
 * @return ESP_OK on success
 */
esp_err_t index_handler(httpd_req_t *req);

/**
 * @brief Handle HTTP GET requests for doom-palette.js
 * @param req HTTP request structure
 * @return ESP_OK on success
 */
esp_err_t doom_palette_js_handler(httpd_req_t *req);

/**
 * @brief Structure for async response arguments
 */
struct async_resp_arg {
    httpd_handle_t hd;
    int fd;
};

/**
 * @brief Async send function for WebSocket frames
 * @param arg Pointer to async_resp_arg structure
 */
void ws_async_send(void *arg);

/**
 * @brief Trigger async send for WebSocket frames
 * @param handle HTTP server handle
 * @param req HTTP request structure
 * @return ESP_OK on success
 */
esp_err_t trigger_async_send(httpd_handle_t handle, httpd_req_t *req);

#ifdef __cplusplus
}
#endif

#endif // HTTP_HANDLERS_H 