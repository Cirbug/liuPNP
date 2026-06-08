/*
 * AI-native Pick-and-Place Controller — ESP32-P4
 *
 * 架构:
 *   OV5647(MIPI) → P4 CSI → JPEG
 *   P4 → WiFi6 → 豆包 API (国内直连) → 坐标/角度
 *   P4 → UART → S3 → MKS → 电机
 *
 * 验证阶段: 先跑通 拍照→豆包→坐标 核心链路
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_camera.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "driver/uart.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"

static const char *TAG = "PNP_CORE";

/* ============================================================
 * 豆包 API 配置 (火山引擎)
 * ============================================================ */
// TODO: 替换为你的 API Key
// 获取: https://console.volcengine.com/ark
#define DOUBAO_API_KEY  "ark-93b5357a-4684-4187-8589-52a337dc1cf3-53c50"
#define DOUBAO_BASE_URL "https://ark.cn-beijing.volces.com/api/v3"
#define DOUBAO_MODEL    "doubao-seed-1-6-vision-250815"
// 备选视觉模型: doubao-seed-1-8-251228, doubao-seed-2-0-pro-260215

// API 调用超时(ms) — 豆包视觉推理通常 2-5 秒
#define VISION_TIMEOUT_MS 15000

/* ============================================================
 * UART 到 ESP32-S3 (执行层)
 * ============================================================ */
#define UART_S3_PORT     UART_NUM_1
#define UART_S3_TX       17
#define UART_S3_RX       18
#define UART_S3_BAUD     921600

/* ============================================================
 * 摄像头引脚 (微雪 ESP32-P4-WIFI6, MIPI CSI 15pin FPC)
 * 接 OV5647 或 OV5640 MIPI 版
 * ============================================================ */
#define CAM_PIN_PWDN     -1
#define CAM_PIN_RESET    -1
#define CAM_PIN_XCLK     15
#define CAM_PIN_SIOD     10
#define CAM_PIN_SIOC     11

/* ============================================================
 * 状态机
 * ============================================================ */
typedef enum {
    STATE_IDLE,
    STATE_HOMING,
    STATE_CALIBRATE,
    STATE_PICK,
    STATE_BOTTOM_ALIGN,
    STATE_PLACE,
    STATE_DONE,
    STATE_ERROR
} pnp_state_t;

static pnp_state_t g_state = STATE_IDLE;

/* ============================================================
 * 标定参数 (NVS)
 * ============================================================ */
typedef struct {
    // 像素 → 毫米 换算 (标定后填入)
    float pixel_to_mm_x;
    float pixel_to_mm_y;
    // 图像中心像素坐标
    int   cam_width;
    int   cam_height;
    // 相机偏移
    float cam_offset_x_mm;
    float cam_offset_y_mm;
    // Mark 点理论机械坐标 (从 PCB 坐标文件读)
    float mark1_x_mm;
    float mark1_y_mm;
} calibration_t;

static calibration_t g_calib;

/* ============================================================
 * 坐标文件 (从 TF 卡 CSV 加载)
 * ============================================================ */
#define MAX_PLACEMENTS 200

typedef struct {
    char designator[16];
    char footprint[24];
    float x_mm, y_mm;
    float rotation;
    int   feeder;
} placement_t;

static placement_t g_placements[MAX_PLACEMENTS];
static int g_placement_count = 0;

/* ============================================================
 * Base64 编码 (用于图片转 API 请求)
 * ============================================================ */
static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int base64_encode(const uint8_t *src, size_t src_len, char *dst, size_t dst_len) {
    size_t i = 0, j = 0;
    while (i < src_len && j < dst_len - 1) {
        uint32_t b = src[i++] << 16;
        b |= (i < src_len) ? src[i++] << 8 : 0;
        b |= (i < src_len) ? src[i++] : 0;

        dst[j++] = b64_table[(b >> 18) & 0x3F];
        if (j < dst_len - 1) dst[j++] = b64_table[(b >> 12) & 0x3F];
        if (j < dst_len - 1) dst[j++] = b64_table[(b >> 6) & 0x3F];
        if (j < dst_len - 1) dst[j++] = b64_table[b & 0x3F];
    }
    /* padding */
    size_t pad = (3 - (src_len % 3)) % 3;
    for (size_t p = 0; p < pad && j >= 2; p++) {
        dst[j - pad + p] = '=';
    }
    dst[j] = '\0';
    return j;
}

/* ============================================================
 * 摄像头拍照
 * ============================================================ */
static camera_fb_t *camera_capture(void) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        ESP_LOGE(TAG, "Camera capture failed");
    } else {
        ESP_LOGI(TAG, "Captured: %zu bytes, %dx%d",
                 fb->len, fb->width, fb->height);
    }
    return fb;
}

/* ============================================================
 * 豆包 Vision API 调用
 *
 * 发送: base64 JPEG + prompt
 * 返回: cJSON 对象 (由调用方释放)
 * ============================================================ */
static esp_err_t doubao_analyze_image(const uint8_t *jpeg_buf, size_t jpeg_len,
                                       const char *prompt, cJSON **result_json) {
    *result_json = NULL;

    if (!jpeg_buf || jpeg_len == 0) {
        ESP_LOGE(TAG, "Invalid JPEG buffer");
        return ESP_ERR_INVALID_ARG;
    }

    /* ---- 1. Base64 编码 JPEG ---- */
    size_t b64_len = ((jpeg_len + 2) / 3) * 4 + 1;
    char *b64_data = malloc(b64_len);
    if (!b64_data) {
        ESP_LOGE(TAG, "Malloc failed for base64 (%zu bytes)", b64_len);
        return ESP_ERR_NO_MEM;
    }
    base64_encode(jpeg_buf, jpeg_len, b64_data, b64_len);
    ESP_LOGI(TAG, "JPEG %zu bytes → base64 %d chars", jpeg_len, (int)strlen(b64_data));

    /* ---- 2. 构造 data:image/jpeg;base64,... URL ---- */
    const char *prefix = "data:image/jpeg;base64,";
    size_t data_url_len = strlen(prefix) + strlen(b64_data) + 1;
    char *data_url = malloc(data_url_len);
    if (!data_url) {
        free(b64_data);
        return ESP_ERR_NO_MEM;
    }
    snprintf(data_url, data_url_len, "%s%s", prefix, b64_data);
    free(b64_data);

    /* ---- 3. 构造 JSON 请求体 ---- */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", DOUBAO_MODEL);
    cJSON *msg_array = cJSON_CreateArray();
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "user");
    cJSON *content_array = cJSON_CreateArray();

    // text part
    cJSON *text_part = cJSON_CreateObject();
    cJSON_AddStringToObject(text_part, "type", "text");
    cJSON_AddStringToObject(text_part, "text", prompt);
    cJSON_AddItemToArray(content_array, text_part);

    // image part
    cJSON *img_part = cJSON_CreateObject();
    cJSON_AddStringToObject(img_part, "type", "image_url");
    cJSON *img_url_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(img_url_obj, "url", data_url);
    cJSON_AddItemToObject(img_part, "image_url", img_url_obj);
    cJSON_AddItemToArray(content_array, img_part);

    cJSON_AddItemToObject(msg, "content", content_array);
    cJSON_AddItemToArray(msg_array, msg);
    cJSON_AddItemToObject(root, "messages", msg_array);

    char *req_body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    free(data_url);

    if (!req_body) {
        ESP_LOGE(TAG, "Failed to serialize JSON request");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Request body: %d bytes", (int)strlen(req_body));

    /* ---- 4. HTTP POST ---- */
    char auth_header[128];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", DOUBAO_API_KEY);

    char url[256];
    snprintf(url, sizeof(url), "%s/chat/completions", DOUBAO_BASE_URL);

    esp_http_client_config_t http_cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = VISION_TIMEOUT_MS,
        .buffer_size = 8192,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_post_field(client, req_body, strlen(req_body));

    esp_err_t err = esp_http_client_perform(client);
    int status_code = 0;
    if (err == ESP_OK) {
        status_code = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "HTTP %d", status_code);

        if (status_code == 200) {
            int content_len = esp_http_client_get_content_length(client);
            char *resp_buf = malloc(content_len > 0 ? content_len + 1 : 16384);
            if (resp_buf) {
                int read_len = esp_http_client_read(client, resp_buf,
                    content_len > 0 ? content_len : 16383);
                if (read_len > 0) {
                    resp_buf[read_len] = '\0';
                    ESP_LOGI(TAG, "Response: %d bytes", read_len);

                    /* ---- 5. 解析响应 JSON ---- */
                    // 豆包返回格式: {"choices":[{"message":{"content":"...JSON..."}}]}
                    cJSON *resp_json = cJSON_Parse(resp_buf);
                    if (resp_json) {
                        cJSON *choices = cJSON_GetObjectItem(resp_json, "choices");
                        if (choices && cJSON_GetArraySize(choices) > 0) {
                            cJSON *choice0 = cJSON_GetArrayItem(choices, 0);
                            cJSON *message = cJSON_GetObjectItem(choice0, "message");
                            if (message) {
                                cJSON *content = cJSON_GetObjectItem(message, "content");
                                if (content && cJSON_IsString(content)) {
                                    const char *text = content->valuestring;
                                    ESP_LOGI(TAG, "LLM output: %s", text);

                                    // 尝试从回复里提取 JSON
                                    const char *brace = strchr(text, '{');
                                    if (brace) {
                                        *result_json = cJSON_Parse(brace);
                                    }
                                }
                            }
                        }
                        cJSON_Delete(resp_json);
                    }
                    if (!*result_json) {
                        ESP_LOGW(TAG, "No valid JSON in LLM response");
                    }
                }
                free(resp_buf);
            }
        } else {
            ESP_LOGE(TAG, "API error: HTTP %d", status_code);
            char err_buf[512] = {0};
            esp_http_client_read(client, err_buf, sizeof(err_buf) - 1);
            ESP_LOGE(TAG, "Error body: %s", err_buf);
        }
    } else {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    free(req_body);
    return (*result_json != NULL) ? ESP_OK : ESP_FAIL;
}

/* ============================================================
 * 视觉识别 — Mark 点
 * ============================================================ */
static esp_err_t vision_find_mark(float *out_x_px, float *out_y_px) {
    camera_fb_t *fb = camera_capture();
    if (!fb) return ESP_FAIL;

    cJSON *json = NULL;
    esp_err_t ret = doubao_analyze_image(
        fb->buf, fb->len,
        "Find the fiducial mark (small round copper pad, usually 1-2mm diameter) "
        "in this PCB image. Return ONLY a JSON object, no other text:\n"
        "{\"x\": <center_pixel_x>, \"y\": <center_pixel_y>, \"width\": <image_width>, \"height\": <image_height>}\n"
        "x and y must be integers. width and height are the image dimensions you see.",
        &json
    );

    esp_camera_fb_return(fb);

    if (ret == ESP_OK && json) {
        *out_x_px = (float)cJSON_GetObjectItem(json, "x")->valuedouble;
        *out_y_px = (float)cJSON_GetObjectItem(json, "y")->valuedouble;
        cJSON_Delete(json);
        ESP_LOGI(TAG, "Mark found @ pixel (%.0f, %.0f)", *out_x_px, *out_y_px);
        return ESP_OK;
    }
    return ESP_FAIL;
}

/* ============================================================
 * 视觉识别 — 吸嘴/元件检查
 * ============================================================ */
static esp_err_t vision_check_nozzle(float *out_angle) {
    camera_fb_t *fb = camera_capture();
    if (!fb) return ESP_FAIL;

    cJSON *json = NULL;
    esp_err_t ret = doubao_analyze_image(
        fb->buf, fb->len,
        "A component held by a vacuum nozzle is visible in this image (viewed from below). "
        "Return ONLY a JSON object:\n"
        "{\"detected\": true/false, \"angle\": <rotation_degrees>, \"width\": <img_w>, \"height\": <img_h>}\n"
        "If no component is visible, set detected to false.",
        &json
    );

    esp_camera_fb_return(fb);

    if (ret == ESP_OK && json) {
        cJSON *detected = cJSON_GetObjectItem(json, "detected");
        if (cJSON_IsTrue(detected)) {
            *out_angle = (float)cJSON_GetObjectItem(json, "angle")->valuedouble;
            cJSON_Delete(json);
            return ESP_OK;
        }
        cJSON_Delete(json);
    }
    return ESP_FAIL;
}

/* ============================================================
 * MKS 指令发送 (通过 S3 转发)
 * ============================================================ */
static void mks_send(const char *fmt, ...) {
    char buf[128];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (len > 0 && len < sizeof(buf) - 1) {
        buf[len] = '\n';
        uart_write_bytes(UART_S3_PORT, buf, len + 1);
        ESP_LOGI(TAG, "→ S3: %s", buf);
    }
}

/* ============================================================
 * NVS 标定存取
 * ============================================================ */
static esp_err_t calib_load(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open("calib", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No calibration data — using defaults");
        memset(&g_calib, 0, sizeof(g_calib));
        g_calib.pixel_to_mm_x = 0.01f; // 需要实际标定
        g_calib.pixel_to_mm_y = 0.01f;
        return err;
    }
    size_t size = sizeof(g_calib);
    nvs_get_blob(handle, "params", &g_calib, &size);
    nvs_close(handle);
    return ESP_OK;
}

static esp_err_t calib_save(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open("calib", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(handle, "params", &g_calib, sizeof(g_calib));
    nvs_commit(handle);
    nvs_close(handle);
    return err;
}

/* ============================================================
 * TF 卡 CSV 加载 (嘉立创 EDA 导出格式)
 *
 * 格式: Designator,Footprint,Mid X(mm),Mid Y(mm),Layer,Rotation,Feeder
 * 示例: C1,C0603,12.5,23.4,TopLayer,90,1
 * ============================================================ */
static esp_err_t placements_load(void) {
    FILE *f = fopen("/sdcard/placements.csv", "r");
    if (!f) {
        ESP_LOGW(TAG, "No /sdcard/placements.csv — run calibration first");
        return ESP_FAIL;
    }

    char line[256];
    int count = 0;

    // 跳过标题行
    fgets(line, sizeof(line), f);

    while (fgets(line, sizeof(line), f) && count < MAX_PLACEMENTS) {
        placement_t *p = &g_placements[count];

        // Designator,Footprint,X,Y,Layer,Rotation,Feeder
        char layer[16];
        int n = sscanf(line, "%15[^,],%23[^,],%f,%f,%15[^,],%f,%d",
                       p->designator, p->footprint,
                       &p->x_mm, &p->y_mm,
                       layer, &p->rotation, &p->feeder);

        if (n >= 6) {
            if (n < 7) p->feeder = 1; // 默认飞达1
            ESP_LOGI(TAG, "  [%d] %s %s @ (%.2f,%.2f) R%.0f° feeder=%d",
                     count, p->designator, p->footprint,
                     p->x_mm, p->y_mm, p->rotation, p->feeder);
            count++;
        }
    }
    fclose(f);

    g_placement_count = count;
    ESP_LOGI(TAG, "Loaded %d placements", count);
    return count > 0 ? ESP_OK : ESP_FAIL;
}

/* ============================================================
 * 主状态机
 * ============================================================ */
static void state_machine_run(void) {
    static int comp_idx = 0;
    float mark_x_px, mark_y_px;

    switch (g_state) {

    /* ---- 归零 ---- */
    case STATE_HOMING:
        ESP_LOGI(TAG, "=== HOMING ===");
        mks_send("G28");
        vTaskDelay(pdMS_TO_TICKS(5000));
        mks_send("G90");  // 绝对坐标
        g_state = STATE_IDLE;
        break;

    /* ---- 标定: LLM识别Mark点 → 记录像素→机械映射 ---- */
    case STATE_CALIBRATE:
        ESP_LOGI(TAG, "=== CALIBRATE ===");
        if (vision_find_mark(&mark_x_px, &mark_y_px) == ESP_OK) {
            // TODO: 移动轴一个已知距离, 再拍一次, 算 pixel_to_mm
            // 当前先存像素坐标
            g_calib.cam_width = 1600;  // UXGA
            g_calib.cam_height = 1200;
            g_calib.mark1_x_mm = 0;    // 需填入实际机械坐标
            g_calib.mark1_y_mm = 0;
            calib_save();
            ESP_LOGI(TAG, "Mark recorded @ pixel(%.0f,%.0f)", mark_x_px, mark_y_px);
        }
        g_state = STATE_IDLE;
        break;

    /* ---- 取料 ---- */
    case STATE_PICK:
        ESP_LOGI(TAG, "=== PICK [%d] %s ===", comp_idx,
                 g_placements[comp_idx].designator);
        mks_send("G00 Z20");
        vTaskDelay(pdMS_TO_TICKS(300));
        // TODO: 移到对应飞达坐标
        mks_send("G00 X100 Y50");
        vTaskDelay(pdMS_TO_TICKS(500));
        mks_send("G00 Z-5");
        vTaskDelay(pdMS_TO_TICKS(300));
        mks_send("PUMP ON");
        vTaskDelay(pdMS_TO_TICKS(200));
        mks_send("G00 Z20");
        g_state = STATE_BOTTOM_ALIGN;
        break;

    /* ---- 底部对位: LLM识别Mark → 算偏移 ---- */
    case STATE_BOTTOM_ALIGN: {
        ESP_LOGI(TAG, "=== BOTTOM ALIGN ===");
        // 移到对位相机上方
        mks_send("G00 X50 Y50");
        vTaskDelay(pdMS_TO_TICKS(500));

        float angle = 0;
        if (vision_check_nozzle(&angle) == ESP_OK) {
            ESP_LOGI(TAG, "Component OK, angle=%.1f°", angle);
        }

        if (vision_find_mark(&mark_x_px, &mark_y_px) == ESP_OK) {
            // 偏移 = 实际像素 - 理论中心
            float dx_px = mark_x_px - (float)g_calib.cam_width / 2;
            float dy_px = mark_y_px - (float)g_calib.cam_height / 2;
            float dx_mm = dx_px * g_calib.pixel_to_mm_x;
            float dy_mm = dy_px * g_calib.pixel_to_mm_y;

            ESP_LOGI(TAG, "Offset: px(%.0f,%.0f) → mm(%.2f,%.2f)",
                     dx_px, dy_px, dx_mm, dy_mm);

            // 相对移动补偿
            mks_send("G91");
            mks_send("G00 X%.2f Y%.2f", -dx_mm, -dy_mm);
            mks_send("G90");
            vTaskDelay(pdMS_TO_TICKS(300));

            g_state = STATE_PLACE;
        } else {
            ESP_LOGW(TAG, "Mark not found — skip align");
            g_state = STATE_PLACE;
        }
        break;
    }

    /* ---- 贴装 ---- */
    case STATE_PLACE:
        ESP_LOGI(TAG, "=== PLACE ===");
        // 移到目标坐标 (从 CSV 读)
        mks_send("G00 X%.2f Y%.2f",
                 g_placements[comp_idx].x_mm,
                 g_placements[comp_idx].y_mm);
        vTaskDelay(pdMS_TO_TICKS(500));
        mks_send("G00 Z-3");
        vTaskDelay(pdMS_TO_TICKS(300));
        mks_send("PUMP OFF");
        vTaskDelay(pdMS_TO_TICKS(200));
        mks_send("G00 Z20");

        comp_idx++;
        if (comp_idx < g_placement_count) {
            g_state = STATE_PICK;
        } else {
            g_state = STATE_DONE;
        }
        break;

    case STATE_DONE:
        ESP_LOGI(TAG, "=== ALL DONE ===");
        mks_send("G00 Z50");
        comp_idx = 0;
        g_state = STATE_IDLE;
        break;

    case STATE_ERROR:
        ESP_LOGE(TAG, "=== EMERGENCY STOP ===");
        mks_send("M112");
        mks_send("PUMP OFF");
        g_state = STATE_IDLE;
        break;

    default:
        break;
    }
}

/* ============================================================
 * 测试模式: 拍照 → 发豆包 → 打印坐标 (不接电机)
 * ============================================================ */
static void test_vision(void) {
    ESP_LOGI(TAG, "=== VISION TEST ===");

    camera_fb_t *fb = camera_capture();
    if (!fb) return;

    cJSON *json = NULL;
    esp_err_t ret = doubao_analyze_image(
        fb->buf, fb->len,
        "Describe what you see in this image briefly. "
        "If you see a PCB, describe the components visible. "
        "If you see any round fiducial marks, note their approximate position. "
        "Keep response under 200 characters.",
        &json
    );

    if (ret == ESP_OK && json) {
        char *str = cJSON_Print(json);
        ESP_LOGI(TAG, "Vision result: %s", str);
        free(str);
        cJSON_Delete(json);
    } else {
        ESP_LOGE(TAG, "Vision test FAILED — check API key / network");
    }

    esp_camera_fb_return(fb);
}

/* ============================================================
 * CLI 调试控制台
 * ============================================================ */
static void cli_task(void *arg) {
    char cmd[64];
    while (1) {
        if (scanf("%63s", cmd) > 0) {
            if (strcmp(cmd, "test") == 0) {
                test_vision();
            } else if (strcmp(cmd, "home") == 0) {
                g_state = STATE_HOMING;
            } else if (strcmp(cmd, "cal") == 0) {
                g_state = STATE_CALIBRATE;
            } else if (strcmp(cmd, "start") == 0) {
                g_state = STATE_PICK;
            } else if (strcmp(cmd, "stop") == 0) {
                g_state = STATE_ERROR;
            } else if (strcmp(cmd, "load") == 0) {
                placements_load();
            } else {
                printf("Commands: test home cal start stop load\n");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* ============================================================
 * 摄像头初始化
 * ============================================================ */
static void camera_init(void) {
    camera_config_t config = {
        .pin_pwdn  = CAM_PIN_PWDN,
        .pin_reset = CAM_PIN_RESET,
        .pin_xclk  = CAM_PIN_XCLK,
        .pin_siod  = CAM_PIN_SIOD,
        .pin_sioc  = CAM_PIN_SIOC,

        // MIPI CSI 模式: D0-D7 无关, 填 -1
        .pin_d7 = -1, .pin_d6 = -1, .pin_d5 = -1, .pin_d4 = -1,
        .pin_d3 = -1, .pin_d2 = -1, .pin_d1 = -1, .pin_d0 = -1,
        .pin_vsync = -1, .pin_href = -1, .pin_pclk = -1,

        .xclk_freq_hz  = 20000000,
        .ledc_timer    = LEDC_TIMER_0,
        .ledc_channel  = LEDC_CHANNEL_0,
        .pixel_format  = PIXFORMAT_JPEG,
        .frame_size    = FRAMESIZE_UXGA,    // 1600×1200
        .jpeg_quality  = 8,                 // 0-63, 越小越好但越大
        .fb_count      = 2,
        .grab_mode     = CAMERA_GRAB_WHEN_EMPTY,
    };

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init FAILED: 0x%x", err);
    } else {
        sensor_t *s = esp_camera_sensor_get();
        if (s) {
            s->set_vflip(s, 0);    // 根据安装方向调整
            s->set_hmirror(s, 0);
        }
        ESP_LOGI(TAG, "Camera OK (MIPI CSI, %dx%d JPEG)",
                 config.frame_size == FRAMESIZE_UXGA ? 1600 : 800,
                 config.frame_size == FRAMESIZE_UXGA ? 1200 : 600);
    }
}

/* ============================================================
 * UART 初始化 (到 S3)
 * ============================================================ */
static void uart_init(void) {
    uart_config_t cfg = {
        .baud_rate = UART_S3_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };
    uart_param_config(UART_S3_PORT, &cfg);
    uart_set_pin(UART_S3_PORT, UART_S3_TX, UART_S3_RX,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(UART_S3_PORT, 1024, 1024, 0, NULL, 0);
    ESP_LOGI(TAG, "UART→S3: TX=%d RX=%d @ %d baud",
             UART_S3_TX, UART_S3_RX, UART_S3_BAUD);
}

/* ============================================================
 * 入口
 * ============================================================ */
void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    ESP_LOGI(TAG, "=== AI-PNP (P4 + 豆包) ===");

    calib_load();
    camera_init();
    uart_init();

    // 尝试加载坐标文件
    placements_load();

    ESP_LOGI(TAG, "Ready. Type 'test' to validate vision pipeline.");
    ESP_LOGI(TAG, "Commands: test home cal start stop load");

    xTaskCreate(cli_task, "cli", 4096, NULL, 5, NULL);

    while (1) {
        if (g_state != STATE_IDLE) {
            state_machine_run();
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
