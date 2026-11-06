#include "web_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "screen_driver.h"

static const char *TAG = "web";

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");
extern const uint8_t vue_js_start[]     asm("_binary_vue_global_prod_js_start");
extern const uint8_t vue_js_end[]       asm("_binary_vue_global_prod_js_end");

static esp_err_t send_embedded(httpd_req_t *req, const uint8_t *start, const uint8_t *end,
                               const char *type)
{
    const size_t len = (size_t)(end - start);
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    return httpd_resp_send(req, (const char *)start, len);
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    return send_embedded(req, index_html_start, index_html_end, "text/html; charset=utf-8");
}

static esp_err_t vue_get_handler(httpd_req_t *req)
{
    return send_embedded(req, vue_js_start, vue_js_end, "application/javascript; charset=utf-8");
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    char json[80];
    snprintf(json, sizeof(json), "{\"ok\":true,\"w\":%d,\"h\":%d,\"frame\":%u}",
             SCREEN_WIDTH, SCREEN_HEIGHT, (unsigned)SCREEN_FRAME_BYTES);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t frame_post_handler(httpd_req_t *req)
{
    if (req->content_len != SCREEN_FRAME_BYTES) {
        ESP_LOGW(TAG, "bad frame size %d, expect %u", req->content_len, (unsigned)SCREEN_FRAME_BYTES);
        /* 文案里的字节数由屏参宏动态生成，避免换屏后写死旧尺寸 */
        char err_msg[96];
        snprintf(err_msg, sizeof(err_msg),
                 "stale page: hard-refresh, expect %u bytes", (unsigned)SCREEN_FRAME_BYTES);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err_msg);
        return ESP_FAIL;
    }

    uint8_t *frame = heap_caps_malloc(SCREEN_FRAME_BYTES, MALLOC_CAP_8BIT);
    if (frame == NULL) {
        frame = malloc(SCREEN_FRAME_BYTES);
    }
    if (frame == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }

    int received = 0;
    while (received < SCREEN_FRAME_BYTES) {
        int ret = httpd_req_recv(req, (char *)frame + received, SCREEN_FRAME_BYTES - received);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            free(frame);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
            return ESP_FAIL;
        }
        received += ret;
    }

    esp_err_t err = screen_driver_show_frame(frame, SCREEN_FRAME_BYTES);
    free(frame);

    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "flush failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

esp_err_t web_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 8;
    config.stack_size = 10240;
    config.recv_wait_timeout = 20;
    config.send_wait_timeout = 20;
    config.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &config), TAG, "httpd_start");

    const httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
    };
    const httpd_uri_t vue = {
        .uri = "/vue.global.prod.js",
        .method = HTTP_GET,
        .handler = vue_get_handler,
    };
    const httpd_uri_t status = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = status_get_handler,
    };
    const httpd_uri_t frame = {
        .uri = "/api/frame",
        .method = HTTP_POST,
        .handler = frame_post_handler,
    };

    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &vue);
    httpd_register_uri_handler(server, &status);
    httpd_register_uri_handler(server, &frame);

    ESP_LOGI(TAG, "HTTP ready on :80");
    return ESP_OK;
}
