/*
 * calib.h — 标定参数 NVS 存取
 */

#ifndef CALIB_H
#define CALIB_H

#include "esp_err.h"

#define CALIB_NVS_NAMESPACE "pnp_calib"
#define CALIB_NVS_KEY       "params"

typedef struct {
    float   pixel_to_mm_x;       /* 像素→毫米 换算 */
    float   pixel_to_mm_y;
    int     cam_width;
    int     cam_height;
    float   cam_offset_x_mm;     /* 相机中心偏移 */
    float   cam_offset_y_mm;
    float   mark1_mm_x;          /* 参考 Mark 点机械坐标 */
    float   mark1_mm_y;
} calibration_t;

esp_err_t calib_init(void);
esp_err_t calib_load(calibration_t *out);
esp_err_t calib_save(const calibration_t *calib);
esp_err_t calib_reset(void);     /* 恢复出厂默认 */

/* 默认值 — 标定前使用 */
void calib_set_defaults(calibration_t *calib);

#endif
