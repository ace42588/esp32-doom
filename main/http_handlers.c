#include "http_handlers.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "HTTP Handlers";

// Static file buffers
static char *g_index_html = NULL;
static char *g_palette_js = NULL;

/* ============================================================================
 * STATIC FILE MANAGEMENT
 * ============================================================================ */

esp_err_t http_load_static_files(void) {
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = true
    };

    ESP_ERROR_CHECK(esp_vfs_spiffs_register(&conf));

    // Load index.html
    struct stat st;
    if (stat("/spiffs/index.html", &st) == 0) {
        g_index_html = heap_caps_malloc(st.st_size + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (g_index_html) {
            FILE *fp = fopen("/spiffs/index.html", "r");
            if (fp) {
                if (fread(g_index_html, st.st_size, 1, fp) == 1) {
                    g_index_html[st.st_size] = '\0';
                    ESP_LOGI(TAG, "Loaded index.html (%ld bytes)", st.st_size);
                } else {
                    heap_caps_free(g_index_html);
                    g_index_html = NULL;
                }
                fclose(fp);
            } else {
                heap_caps_free(g_index_html);
                g_index_html = NULL;
            }
        }
    }

    // Load doom-palette.js
    if (stat("/spiffs/doom-palette.js", &st) == 0) {
        g_palette_js = heap_caps_malloc(st.st_size + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (g_palette_js) {
            FILE *fp = fopen("/spiffs/doom-palette.js", "r");
            if (fp) {
                if (fread(g_palette_js, st.st_size, 1, fp) == 1) {
                    g_palette_js[st.st_size] = '\0';
                    ESP_LOGI(TAG, "Loaded doom-palette.js (%ld bytes)", st.st_size);
                } else {
                    heap_caps_free(g_palette_js);
                    g_palette_js = NULL;
                }
                fclose(fp);
            } else {
                heap_caps_free(g_palette_js);
                g_palette_js = NULL;
            }
        }
    }

    return ESP_OK;
}

void http_cleanup_static_files(void) {
    if (g_index_html) {
        heap_caps_free(g_index_html);
        g_index_html = NULL;
    }
    
    if (g_palette_js) {
        heap_caps_free(g_palette_js);
        g_palette_js = NULL;
    }
    
    ESP_LOGI(TAG, "Static files cleaned up");
}

/* ============================================================================
 * HTTP REQUEST HANDLERS
 * ============================================================================ */

esp_err_t http_index_handler(httpd_req_t *req) {
    if (g_index_html) {
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, g_index_html, strlen(g_index_html));
    } else {
        httpd_resp_send_404(req);
    }
    return ESP_OK;
}

esp_err_t http_palette_handler(httpd_req_t *req) {
    if (g_palette_js) {
        httpd_resp_set_type(req, "application/javascript");
        httpd_resp_send(req, g_palette_js, strlen(g_palette_js));
    } else {
        httpd_resp_send_404(req);
    }
    return ESP_OK;
}

esp_err_t http_ws_handler(httpd_req_t *req) {
    // This is a placeholder - actual WebSocket handling is done in websocket_server.c
    httpd_resp_send_404(req);
    return ESP_OK;
}

/* ============================================================================
 * UTILITY FUNCTIONS
 * ============================================================================ */

// Allocate buffer in PSRAM with fallback to internal memory
void* http_alloc_psram_buffer(size_t size) {
    void *buffer = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer == NULL) {
        ESP_LOGW(TAG, "Failed to allocate %zu bytes in PSRAM, falling back to internal memory", size);
        buffer = heap_caps_malloc(size, MALLOC_CAP_8BIT);
        if (buffer == NULL) {
            ESP_LOGE(TAG, "Failed to allocate %zu bytes in any memory", size);
        }
    }
    return buffer;
}

const char* http_get_content_type(const char *filename) {
    if (strstr(filename, ".html")) {
        return "text/html";
    } else if (strstr(filename, ".js")) {
        return "application/javascript";
    } else if (strstr(filename, ".css")) {
        return "text/css";
    } else if (strstr(filename, ".png")) {
        return "image/png";
    } else if (strstr(filename, ".jpg") || strstr(filename, ".jpeg")) {
        return "image/jpeg";
    } else {
        return "application/octet-stream";
    }
}

esp_err_t http_send_file_response(httpd_req_t *req, const char *filepath, const char *content_type) {
    FILE *fp = fopen(filepath, "r");
    if (!fp) {
        httpd_resp_send_404(req);
        return ESP_ERR_NOT_FOUND;
    }
    
    if (content_type) {
        httpd_resp_set_type(req, content_type);
    }
    
    char buffer[1024];
    size_t bytes_read;
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), fp)) > 0) {
        httpd_resp_send_chunk(req, buffer, bytes_read);
    }
    
    httpd_resp_send_chunk(req, NULL, 0);
    fclose(fp);
    return ESP_OK;
} 