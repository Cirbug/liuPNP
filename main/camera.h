/*
 * camera.h — OV5647 MIPI CSI 摄像头驱动
 * ESP32-P4 板载 15pin FPC 接口 (MIPI 2-lane)
 */

#ifndef CAMERA_H
#define CAMERA_H

#include "esp_camera.h"
#include "esp_err.h"

/* SCCB 控制引脚 (微雪 ESP32-P4-WIFI6, MIPI CSI 15pin FPC) */
#define CAM_PIN_PWDN    -1
#define CAM_PIN_RESET   -1
#define CAM_PIN_XCLK    15
#define CAM_PIN_SIOD    10
#define CAM_PIN_SIOC    11

/* 图像尺寸 — UXGA, 可在 sdkconfig 覆写 */
#ifndef CAM_FRAME_WIDTH
#define CAM_FRAME_WIDTH  1600
#endif
#ifndef CAM_FRAME_HEIGHT
#define CAM_FRAME_HEIGHT 1200
#endif

esp_err_t camera_init(void);
camera_fb_t* camera_capture(void);
static inline void camera_release(camera_fb_t *fb) { esp_camera_fb_return(fb); }

#endif
