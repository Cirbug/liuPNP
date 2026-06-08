/*
 * vision.c — 视觉任务实现
 */

#include "vision.h"
#include "camera.h"
#include "doubao.h"
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "vision";

/* ---- 找 Mark 点 ---- */
esp_err_t vision_find_mark(float *out_x, float *out_y) {
    camera_fb_t *fb = camera_capture();
    if (!fb) return ESP_FAIL;

    cJSON *json = NULL;
    esp_err_t ret = doubao_analyze_image(fb->buf, fb->len,
        "Find the fiducial mark (small round copper pad, 1-2mm diameter) "
        "in this PCB image. Return ONLY a JSON object, no other text:\n"
        "{\"x\": <center_pixel_x>, \"y\": <center_pixel_y>, "
        "\"width\": <image_width>, \"height\": <image_height>}\n"
        "x and y must be integers.",
        &json);

    camera_release(fb);

    if (ret == ESP_OK && json) {
        cJSON *jx = cJSON_GetObjectItem(json, "x");
        cJSON *jy = cJSON_GetObjectItem(json, "y");
        if (jx && jy) {
            *out_x = (float)jx->valuedouble;
            *out_y = (float)jy->valuedouble;
            ESP_LOGI(TAG, "Mark @ (%.0f, %.0f) px", *out_x, *out_y);
            cJSON_Delete(json);
            return ESP_OK;
        }
        cJSON_Delete(json);
    }
    return ESP_FAIL;
}

/* ---- 检查吸嘴元件 ---- */
esp_err_t vision_check_nozzle(float *out_angle) {
    camera_fb_t *fb = camera_capture();
    if (!fb) return ESP_FAIL;

    cJSON *json = NULL;
    esp_err_t ret = doubao_analyze_image(fb->buf, fb->len,
        "A component held by a vacuum nozzle is visible (viewed from below). "
        "Return ONLY JSON: "
        "{\"detected\": true/false, \"angle\": <rotation_degrees>, "
        "\"width\": <img_w>, \"height\": <img_h>}",
        &json);

    camera_release(fb);

    if (ret == ESP_OK && json) {
        cJSON *det = cJSON_GetObjectItem(json, "detected");
        if (cJSON_IsTrue(det)) {
            cJSON *ang = cJSON_GetObjectItem(json, "angle");
            if (ang) {
                *out_angle = (float)ang->valuedouble;
                ESP_LOGI(TAG, "Nozzle OK, angle=%.1f°", *out_angle);
                cJSON_Delete(json);
                return ESP_OK;
            }
        }
        cJSON_Delete(json);
    }
    return ESP_FAIL;
}
