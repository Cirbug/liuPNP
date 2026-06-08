/*
 * placements.h — 嘉立创 EDA CSV 坐标文件加载
 *
 * CSV 格式 (UTF-8, 逗号分隔):
 *   Designator,Footprint,Mid X(mm),Mid Y(mm),Layer,Rotation,Feeder
 *
 * 示例:
 *   C1,C0603,12.5,23.4,TopLayer,90,1
 */

#ifndef PLACEMENTS_H
#define PLACEMENTS_H

#include "esp_err.h"

#define MAX_PLACEMENTS      200
#define CSV_PATH_SDCARD     "/sdcard/placements.csv"

typedef struct {
    char   designator[16];
    char   footprint[24];
    float  x_mm;
    float  y_mm;
    float  rotation;           /* 角度 (度) */
    int    feeder;             /* 飞达编号 (1-8 或其他) */
} placement_t;

/*
 * 从 TF 卡 /sdcard/placements.csv 加载坐标
 * out_count: 输出实际数量
 * 返回 ESP_OK 且 out_count > 0 表示成功
 */
esp_err_t placements_load(placement_t *out_list, int max_count, int *out_count);

#endif
