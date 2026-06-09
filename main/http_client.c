/*
 * http_client.c — HTTP 图像拉取实现
 *
 * 从 PC mjpg-streamer 拉取 JPEG 帧，提供给视觉分析模块使用。
 * 支持两种模式:
 *   - 快照模式: GET /?action=snapshot → 单帧 JPEG
 *   - 流模式:   GET /?action=stream  → MJPEG 流 (暂不启用)
 */
#include "http_client.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "http_img";

/* ---- 拉取单帧 JPEG ---- */
esp_err_t http_fetch_jpeg(const char *url, uint8_t **out_buf, size_t *out_len,
                          int timeout_ms)
{
    *out_buf = NULL;
    *out_len = 0;

    if (!url) return ESP_ERR_INVALID_ARG;

    esp_http_client_config_t cfg = {
        .url        = url,
        .method     = HTTP_METHOD_GET,
        .timeout_ms = timeout_ms > 0 ? timeout_ms : HTTP_IMG_DEFAULT_TIMEOUT_MS,
        .buffer_size = 4096,
        .keep_alive_enable = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "esp_http_client_init failed");
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP GET %s: %s", url, esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    int http_code = esp_http_client_get_status_code(client);
    if (http_code != 200) {
        ESP_LOGE(TAG, "HTTP %d", http_code);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    /* 读取响应体 */
    int content_len = esp_http_client_get_content_length(client);
    size_t buf_size = (content_len > 0) ? (size_t)content_len : 65536; /* 默认 64KB */
    uint8_t *buf = malloc(buf_size);
    if (!buf) {
        ESP_LOGE(TAG, "OOM: %zu bytes", buf_size);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    int total = 0;
    int read_len;
    while (total < (int)buf_size - 1) {
        read_len = esp_http_client_read(client, (char *)(buf + total),
                                        (int)(buf_size - total - 1));
        if (read_len <= 0) break;
        total += read_len;
    }

    esp_http_client_cleanup(client);

    if (total > 0) {
        *out_buf = buf;
        *out_len = (size_t)total;
        ESP_LOGI(TAG, "Fetched %d bytes from %s", total, url);
        return ESP_OK;
    } else {
        free(buf);
        ESP_LOGE(TAG, "Empty response");
        return ESP_FAIL;
    }
}

/* ---- 释放缓冲区 ---- */
void http_img_free(uint8_t *buf)
{
    if (buf) free(buf);
}