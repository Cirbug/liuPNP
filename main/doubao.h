/*
 * doubao.h — 豆包 (火山引擎 Ark) Vision API 调用
 * 把 JPEG → 豆包分析 → 返回解析后的 JSON
 */

#ifndef DOUBAO_H
#define DOUBAO_H

#include "esp_err.h"
#include "cJSON.h"
#include <stddef.h>
#include <stdint.h>

/* ===== 用户必须修改 ===== */
#ifndef DOUBAO_API_KEY
#define DOUBAO_API_KEY  "YOUR_ARK_API_KEY"   /* 替换你的 key */
#endif

/* ===== 固定配置 ===== */
#define DOUBAO_BASE_URL     "https://ark.cn-beijing.volces.com/api/v3"
#define DOUBAO_MODEL        "doubao-seed-1-6-vision-250815"
#define DOUBAO_TIMEOUT_MS   15000           /* 视觉推理 2-8 秒, 给 15s 余量 */

/*
 * 发送 JPEG + prompt 给豆包, 解析返回 JSON
 * 返回 ESP_OK 时 *result_json 是 cJSON 对象, 调用方负责 cJSON_Delete
 */
esp_err_t doubao_analyze_image(const uint8_t *jpeg_buf, size_t jpeg_len,
                               const char *prompt, cJSON **result_json);

#endif
