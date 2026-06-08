/*
 * doubao.c — 豆包 Vision API 调用实现
 *
 * 流程:
 *   1. 收 JPEG 缓冲
 *   2. Base64 编码
 *   3. 拼装 OpenAI-compatible multipart vision 请求 JSON
 *   4. HTTP POST 到 https://ark.cn-beijing.volces.com/api/v3/chat/completions
 *   5. 解析 { "choices": [{ "message": { "content": "..." } }] }
 *   6. 从 content 字段提取 JSON → cJSON 对象返回
 */

#include "doubao.h"
#include "base64.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "doubao";

esp_err_t doubao_analyze_image(const uint8_t *jpeg_buf, size_t jpeg_len,
                               const char *prompt, cJSON **result_json) {
    *result_json = NULL;

    if (!jpeg_buf || jpeg_len == 0) {
        ESP_LOGE(TAG, "Invalid JPEG buffer");
        return ESP_ERR_INVALID_ARG;
    }

    /* ---- 1. Base64 编码 JPEG ---- */
    size_t b64_size = base64_encoded_len(jpeg_len) + 1;
    char *b64_buf = malloc(b64_size);
    if (!b64_buf) { ESP_LOGE(TAG, "OOM base64"); return ESP_ERR_NO_MEM; }
    base64_encode(jpeg_buf, jpeg_len, b64_buf, b64_size);

    /* 拼装 data:image/jpeg;base64,... */
    size_t data_url_size = strlen("data:image/jpeg;base64,") + strlen(b64_buf) + 1;
    char *data_url = malloc(data_url_size);
    if (!data_url) { free(b64_buf); return ESP_ERR_NO_MEM; }
    snprintf(data_url, data_url_size, "data:image/jpeg;base64,%s", b64_buf);
    free(b64_buf);

    /* ---- 2. 构造请求 JSON ---- */
    cJSON *root     = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", DOUBAO_MODEL);

    cJSON *msg      = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "user");

    cJSON *content  = cJSON_CreateArray();

    cJSON *text_p   = cJSON_CreateObject();
    cJSON_AddStringToObject(text_p, "type", "text");
    cJSON_AddStringToObject(text_p, "text", prompt);
    cJSON_AddItemToArray(content, text_p);

    cJSON *img_p    = cJSON_CreateObject();
    cJSON_AddStringToObject(img_p, "type", "image_url");
    cJSON *img_url  = cJSON_CreateObject();
    cJSON_AddStringToObject(img_url, "url", data_url);
    cJSON_AddItemToObject(img_p, "image_url", img_url);
    cJSON_AddItemToArray(content, img_p);

    cJSON_AddItemToObject(msg, "content", content);
    cJSON *msgs     = cJSON_CreateArray();
    cJSON_AddItemToArray(msgs, msg);
    cJSON_AddItemToObject(root, "messages", msgs);

    char *req_body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    free(data_url);

    ESP_LOGI(TAG, "Request body: %d chars", (int)strlen(req_body));

    /* ---- 3. HTTP POST ---- */
    char auth_header[128];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", DOUBAO_API_KEY);

    char url[256];
    snprintf(url, sizeof(url), "%s/chat/completions", DOUBAO_BASE_URL);

    esp_http_client_config_t http_cfg = {
        .url              = url,
        .method           = HTTP_METHOD_POST,
        .timeout_ms       = DOUBAO_TIMEOUT_MS,
        .buffer_size      = 8192,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_post_field(client, req_body, strlen(req_body));

    esp_err_t err = esp_http_client_perform(client);
    int http_code = 0;

    if (err == ESP_OK) {
        http_code = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "HTTP %d", http_code);

        if (http_code == 200) {
            int cl = esp_http_client_get_content_length(client);
            size_t buf_size = (cl > 0) ? (size_t)cl + 1 : 16384;
            char *resp_buf = malloc(buf_size);
            if (resp_buf) {
                int rlen = esp_http_client_read(client, resp_buf, buf_size - 1);
                if (rlen > 0) {
                    resp_buf[rlen] = '\0';
                    ESP_LOGI(TAG, "Response: %d bytes", rlen);

                    /* 解析: choices[0].message.content */
                    cJSON *resp = cJSON_Parse(resp_buf);
                    if (resp) {
                        cJSON *choices = cJSON_GetObjectItem(resp, "choices");
                        if (choices && cJSON_GetArraySize(choices) > 0) {
                            cJSON *msg_obj = cJSON_GetObjectItem(
                                cJSON_GetArrayItem(choices, 0), "message");
                            if (msg_obj) {
                                cJSON *content_obj = cJSON_GetObjectItem(msg_obj, "content");
                                if (content_obj && cJSON_IsString(content_obj)) {
                                    const char *text = content_obj->valuestring;
                                    ESP_LOGI(TAG, "LLM: %.100s...", text);

                                    /* 提取 content 里的 JSON 对象 */
                                    const char *brace = strchr(text, '{');
                                    if (brace) *result_json = cJSON_Parse(brace);
                                }
                            }
                        }
                        cJSON_Delete(resp);
                    }
                }
                free(resp_buf);
            }
        } else {
            char err_buf[512] = {0};
            esp_http_client_read(client, err_buf, sizeof(err_buf) - 1);
            ESP_LOGE(TAG, "API %d: %s", http_code, err_buf);
        }
    } else {
        ESP_LOGE(TAG, "HTTP error: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    free(req_body);
    return (*result_json) ? ESP_OK : ESP_FAIL;
}
