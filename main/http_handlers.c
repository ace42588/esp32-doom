#include "http_handlers.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "HTTP Handlers";

// Web page buffer (moved to PSRAM to save internal RAM)
extern char *index_html;

esp_err_t index_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "HTTP request received for index.html");
    
    if (!index_html) {
        ESP_LOGE(TAG, "index.html buffer not initialized");
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, index_html, strlen(index_html));
    return ESP_OK;
}

esp_err_t doom_palette_js_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "HTTP request received for doom-palette.js");
    
    // Open and read the file directly from SPIFFS
    FILE *fp = fopen(DOOM_PALETTE_JS_PATH, "r");
    if (fp == NULL) {
        ESP_LOGE(TAG, "Failed to open %s", DOOM_PALETTE_JS_PATH);
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    
    // Get file size
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    if (file_size <= 0) {
        ESP_LOGE(TAG, "Invalid file size for %s", DOOM_PALETTE_JS_PATH);
        fclose(fp);
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    
    // Set content type
    httpd_resp_set_type(req, "application/javascript");
    
    // Send file in chunks to avoid memory issues
    char buffer[1024];
    size_t bytes_read;
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), fp)) > 0) {
        esp_err_t ret = httpd_resp_send_chunk(req, buffer, bytes_read);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send chunk: %s", esp_err_to_name(ret));
            fclose(fp);
            return ret;
        }
    }
    
    // Send final chunk (empty to indicate end)
    httpd_resp_send_chunk(req, NULL, 0);
    
    fclose(fp);
    return ESP_OK;
}

void ws_async_send(void *arg) {
    static const char *data = "Async data";
    struct async_resp_arg *resp_arg = (struct async_resp_arg *)arg;
    httpd_handle_t hd = resp_arg->hd;
    int fd = resp_arg->fd;
    
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.payload = (uint8_t*)data;
    ws_pkt.len = strlen(data);
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    httpd_ws_send_frame_async(hd, fd, &ws_pkt);
    free(resp_arg);
}

esp_err_t trigger_async_send(httpd_handle_t handle, httpd_req_t *req) {
    struct async_resp_arg *resp_arg = malloc(sizeof(struct async_resp_arg));
    if (resp_arg == NULL) {
        return ESP_ERR_NO_MEM;
    }
    
    resp_arg->hd = req->handle;
    resp_arg->fd = httpd_req_to_sockfd(req);
    
    esp_err_t ret = httpd_queue_work(handle, ws_async_send, resp_arg);
    if (ret != ESP_OK) {
        free(resp_arg);
    }
    
    return ret;
} 