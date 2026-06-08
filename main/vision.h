/*
 * vision.h — 视觉任务封装
 * 基于豆包 API 封装两个核心任务:
 *   1. 找 PCB Mark 点 → 像素坐标
 *   2. 检查吸嘴上的元件 → 角度
 */

#ifndef VISION_H
#define VISION_H

#include "esp_err.h"

/* 找 PCB 上第一个 Mark 点, 输出像素坐标 */
esp_err_t vision_find_mark(float *out_x_px, float *out_y_px);

/* 从底部看吸嘴 + 元件, 输出旋转角度 (度) */
esp_err_t vision_check_nozzle(float *out_angle);

#endif
