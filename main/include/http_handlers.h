#ifndef HTTP_HANDLERS_H
#define HTTP_HANDLERS_H

#include "esp_http_server.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// HTTP response structure for static files
typedef struct {
    const char *content_type;
    const char *data;
    size_t length;
} http_static_file_t;

// Function declarations for HTTP request handling
esp_err_t http_index_handler(httpd_req_t *req);
esp_err_t http_palette_handler(httpd_req_t *req);
esp_err_t http_ws_handler(httpd_req_t *req);

// Static file management
esp_err_t http_load_static_files(void);
void http_cleanup_static_files(void);

// Utility functions
const char* http_get_content_type(const char *filename);
esp_err_t http_send_file_response(httpd_req_t *req, const char *filepath, const char *content_type);

#ifdef __cplusplus
}
#endif

#endif // HTTP_HANDLERS_H 