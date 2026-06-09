/*
 * doubao.h — 豆包 (火山引擎 Ark) Vision API 调用
 * 把 JPEG → 豆包分析 → 返回解析后的 JSON
 *
 * 配置通过 menuconfig (Kconfig.projbuild) 管理:
 *   - CONFIG_DOUBAO_API_KEY
 *   - CONFIG_DOUBAO_API_URL
 *   - CONFIG_DOUBAO_MODEL
 *   - CONFIG_DOUBAO_TIMEOUT_MS
 *   - CONFIG_DOUBAO_MAX_RETRIES
 */

#ifndef DOUBAO_H
#define DOUBAO_H

#include "esp_err.h"
#include "cJSON.h"
#include <stddef.h>
#include <stdint.h>

/*
 * 发送 JPEG + prompt 给豆包, 解析返回 JSON
 *
 * jpeg_buf    : JPEG 图像缓冲区
 * jpeg_len    : 缓冲区长度
 * prompt      : 文本指令 (提示词)
 * result_json : 输出 cJSON 对象, 调用方负责 cJSON_Delete
 *
 * 返回 ESP_OK 时 *result_json 非空
 * 内置自动重试 (次数由 CONFIG_DOUBAO_MAX_RETRIES 控制)
 */
esp_err_t doubao_analyze_image(const uint8_t *jpeg_buf, size_t jpeg_len,
                               const char *prompt, cJSON **result_json);

#endif