/*
 * http_client.h — HTTP 图像拉取接口
 *
 * 从 PC mjpg-streamer 拉取 JPEG 帧
 */

#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

/* 默认超时 (ms) */
#define HTTP_IMG_DEFAULT_TIMEOUT_MS  5000

/*
 * 从指定 URL 拉取 JPEG 图像
 * url        : mjpg-streamer 快照地址, 如 "http://192.168.1.100:8080/?action=snapshot"
 * out_buf    : 输出: 堆上分配的 JPEG 缓冲区 (调用方须 http_img_free)
 * out_len    : 输出: 缓冲区大小
 * timeout_ms : 超时 (0 = 默认 5s)
 * 返回 ESP_OK 表示成功
 */
esp_err_t http_fetch_jpeg(const char *url, uint8_t **out_buf, size_t *out_len,
                          int timeout_ms);

/* 释放 http_fetch_jpeg 分配的缓冲区 */
void http_img_free(uint8_t *buf);

#endif