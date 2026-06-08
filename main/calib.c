/*
 * calib.c — 标定参数 NVS 存取实现
 */

#include "calib.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>

static const char *TAG = "calib";

esp_err_t calib_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

void calib_set_defaults(calibration_t *calib) {
    memset(calib, 0, sizeof(*calib));
    calib->pixel_to_mm_x  = 0.005f;  /* 默认: 0.005mm/px, 标定后修正 */
    calib->pixel_to_mm_y  = 0.005f;
    calib->cam_width      = 1600;
    calib->cam_height     = 1200;
    calib->cam_offset_x_mm = 0.0f;
    calib->cam_offset_y_mm = 0.0f;
    calib->mark1_mm_x      = 0.0f;
    calib->mark1_mm_y      = 0.0f;
}

esp_err_t calib_load(calibration_t *out) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(CALIB_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No calib data, using defaults");
        calib_set_defaults(out);
        return err;
    }
    size_t size = sizeof(*out);
    err = nvs_get_blob(h, CALIB_NVS_KEY, out, &size);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Read failed, defaults");
        calib_set_defaults(out);
    } else {
        ESP_LOGI(TAG, "Loaded: %.4f mm/px", out->pixel_to_mm_x);
    }
    return ESP_OK;
}

esp_err_t calib_save(const calibration_t *calib) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(CALIB_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, CALIB_NVS_KEY, calib, sizeof(*calib));
    if (err == ESP_OK) nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Saved");
    return err;
}

esp_err_t calib_reset(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(CALIB_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        nvs_erase_key(h, CALIB_NVS_KEY);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "Reset to defaults");
    return ESP_OK;
}
