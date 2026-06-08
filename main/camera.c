/*
 * camera.c — OV5647 MIPI CSI 摄像头驱动实现
 */

#include "camera.h"
#include "esp_log.h"

static const char *TAG = "camera";

esp_err_t camera_init(void) {
    camera_config_t cfg = {
        .pin_pwdn       = CAM_PIN_PWDN,
        .pin_reset      = CAM_PIN_RESET,
        .pin_xclk       = CAM_PIN_XCLK,
        .pin_sccb_sda   = CAM_PIN_SIOD,
        .pin_sccb_scl   = CAM_PIN_SIOC,

        .xclk_freq_hz   = 20000000,    /* OV5647 典型 20MHz */
        .ledc_timer     = LEDC_TIMER_0,
        .ledc_channel   = LEDC_CHANNEL_0,

        .pixel_format   = PIXFORMAT_JPEG,  /* 直接出 JPEG */
        .frame_size     = FRAMESIZE_UXGA,   /* 1600×1200 */

        .jpeg_quality   = 72,              /* 压缩率: API 图片 100KB~200KB */
        .fb_count       = 2,               /* 双缓冲 */
        .fb_location    = CAMERA_FB_IN_PSRAM,
        .grab_mode      = CAMERA_GRAB_WHEN_EMPTY,
    };

    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 调整传感器参数 */
    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        s->set_brightness(s, 1);         // -2..2
        s->set_contrast(s, 0);           // -2..2
        s->set_saturation(s, 0);         // -2..2
        s->set_sharpness(s, 1);          // 增强边缘 → 利于 Mark 点识别
        s->set_hmirror(s, 0);
        s->set_vflip(s, 0);
    }

    ESP_LOGI(TAG, "OV5647 MIPI CSI init OK, %dx%d JPEG",
             CAM_FRAME_WIDTH, CAM_FRAME_HEIGHT);
    return ESP_OK;
}

camera_fb_t* camera_capture(void) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        ESP_LOGE(TAG, "Capture failed");
    } else {
        ESP_LOGI(TAG, "Captured %zu bytes @ %dx%d",
                 fb->len, fb->width, fb->height);
    }
    return fb;
}
