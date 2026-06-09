/*
 * main.h — liuPNP 全局定义和外部声明
 *
 * 被 main.c / state_machine.c / vision.c 等共享
 */

#ifndef MAIN_H
#define MAIN_H

#include <stdbool.h>
#include "calib.h"
#include "placements.h"
#include "state_machine.h"

/* ---- 飞达坐标配置表 ---- */
/* TODO: 实际安装后通过 calib 命令修改，或从 TF 卡 feeder.json 加载 */
typedef struct {
    int   id;              /* 飞达编号 */
    float x_mm;            /* 飞达取料 X 坐标 (mm) */
    float y_mm;            /* 飞达取料 Y 坐标 (mm) */
    float z_pickup_mm;     /* 吸料下探深度 */
} feeder_config_t;

#define MAX_FEEDERS             8
#define FEEDER_DEFAULT_X        50.0f
#define FEEDER_DEFAULT_Y        50.0f
#define FEEDER_DEFAULT_Z_PICKUP -5.0f

/* ---- 下视相机位置 ---- */
#define BOTTOM_CAM_X_MM         80.0f
#define BOTTOM_CAM_Y_MM         80.0f

/* ---- 安全高度 ---- */
#define Z_SAFE_MM               20.0f
#define Z_PLACE_MM              -3.0f

/* ---- 全局状态（跨模块共享） ---- */
extern placement_t    g_placements[MAX_PLACEMENTS];
extern int            g_placement_count;
extern int            g_comp_idx;
extern calibration_t  g_calib;

/* ---- 飞达配置表 ---- */
extern feeder_config_t g_feeders[MAX_FEEDERS];
extern int             g_feeder_count;

/* 初始化默认飞达配置 */
void feeders_init_defaults(void);

/* 根据飞达编号查找坐标 */
feeder_config_t *feeder_find(int id);

/* WiFi 连接状态 (供 web_server 查询) */
bool wifi_is_connected(void);

#endif
