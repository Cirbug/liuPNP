/*
 * state_machine.c — 贴片机状态机实现
 *
 * 核心流程:
 *   HOMING → 归零
 *   CALIBRATE → 拍照找 Mark → 记录像素→机械映射
 *   PICK → 移到飞达 → 吸元件
 *   BOTTOM_ALIGN → 下视拍照 → 算偏移 → 补偿
 *   PLACE → 移到目标坐标 → 释放 → 下一个
 */

#include "state_machine.h"
#include "main.h"
#include "calib.h"
#include "placements.h"
#include "vision.h"
#include "motion.h"
#include "camera.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sm";

static pnp_state_t     g_sm_state = STATE_IDLE;

const char* state_name(pnp_state_t s) {
    switch (s) {
    case STATE_IDLE:         return "IDLE";
    case STATE_HOMING:       return "HOMING";
    case STATE_CALIBRATE:    return "CALIBRATE";
    case STATE_PICK:         return "PICK";
    case STATE_BOTTOM_ALIGN: return "BOTTOM_ALIGN";
    case STATE_PLACE:        return "PLACE";
    case STATE_DONE:         return "DONE";
    case STATE_ERROR:        return "ERROR";
    default:                 return "?";
    }
}

void state_machine_init(void) {
    calib_load(&g_calib);
    placements_load(g_placements, MAX_PLACEMENTS, &g_placement_count);
    g_sm_state = STATE_IDLE;
    ESP_LOGI(TAG, "Ready — %d placements, calib=%.4f mm/px",
             g_placement_count, g_calib.pixel_to_mm_x);
}

void state_machine_trigger(pnp_state_t target) {
    ESP_LOGI(TAG, "Trigger: %s → %s", state_name(g_sm_state), state_name(target));
    g_sm_state = target;
}

pnp_state_t state_machine_get(void) { return g_sm_state; }

/* ================================================================
 * 状态执行 (每次调用跑一步, 适合在 FreeRTOS task 中轮询)
 * ================================================================ */
void state_machine_run(void) {
    float x_px, y_px, angle;

    switch (g_sm_state) {

    case STATE_HOMING:
        ESP_LOGI(TAG, "=== HOMING ===");
        motion_send("G28");                     /* 全轴归零 */
        vTaskDelay(pdMS_TO_TICKS(5000));
        motion_send("G90");                     /* 绝对坐标模式 */
        g_sm_state = STATE_IDLE;
        break;

    case STATE_CALIBRATE: {
        ESP_LOGI(TAG, "=== CALIBRATE ===");
        if (vision_find_mark(&x_px, &y_px) == ESP_OK) {
            /* 记录像素坐标, 用户需手动输入机械坐标后保存 */
            ESP_LOGI(TAG, "Mark pixel: (%.0f, %.0f) — input mark1_mm in calib", x_px, y_px);
            /* TODO: 自动标定流程需要移动轴后再次拍照 */
        } else {
            ESP_LOGW(TAG, "Mark not found — check lighting/focus");
        }
        g_sm_state = STATE_IDLE;
        break;
    }

    case STATE_PICK: {
        if (g_comp_idx >= g_placement_count) {
            g_sm_state = STATE_DONE;
            break;
        }
        ESP_LOGI(TAG, "=== PICK [%d] %s ===", g_comp_idx,
                 g_placements[g_comp_idx].designator);

        /* 查飞达坐标 */
        int feeder_id = g_placements[g_comp_idx].feeder;
        feeder_config_t *f = feeder_find(feeder_id);
        if (!f) {
            ESP_LOGW(TAG, "Feeder %d not found, using feeder 1", feeder_id);
            f = feeder_find(1);
        }
        if (!f) {
            ESP_LOGE(TAG, "No feeder config — abort");
            g_sm_state = STATE_ERROR;
            break;
        }

        motion_send("G00 Z%.2f", (double)Z_SAFE_MM);
        vTaskDelay(pdMS_TO_TICKS(300));
        motion_send("G00 X%.2f Y%.2f", (double)f->x_mm, (double)f->y_mm);
        vTaskDelay(pdMS_TO_TICKS(500));
        motion_send("G00 Z%.2f", (double)f->z_pickup_mm);
        vTaskDelay(pdMS_TO_TICKS(300));
        motion_send("PUMP ON");
        vTaskDelay(pdMS_TO_TICKS(200));
        motion_send("G00 Z%.2f", (double)Z_SAFE_MM);
        g_sm_state = STATE_BOTTOM_ALIGN;
        break;
    }

    case STATE_BOTTOM_ALIGN: {
        ESP_LOGI(TAG, "=== BOTTOM ALIGN ===");
        /* 移到下视相机上方 */
        motion_send("G00 X%.2f Y%.2f", (double)BOTTOM_CAM_X_MM, (double)BOTTOM_CAM_Y_MM);
        vTaskDelay(pdMS_TO_TICKS(500));

        /* 检查吸嘴是否有元件 */
        if (vision_check_nozzle(&angle) == ESP_OK) {
            ESP_LOGI(TAG, "Component OK, angle=%.1f°", angle);
        } else {
            ESP_LOGW(TAG, "Nozzle check failed — continue anyway");
        }

        /* 找 Mark 点算偏移 */
        if (vision_find_mark(&x_px, &y_px) == ESP_OK) {
            float cx = (float)g_calib.cam_width  / 2.0f;
            float cy = (float)g_calib.cam_height / 2.0f;
            float dx_mm = (x_px - cx) * g_calib.pixel_to_mm_x;
            float dy_mm = (y_px - cy) * g_calib.pixel_to_mm_y;
            ESP_LOGI(TAG, "Offset: (%.2f, %.2f) mm", dx_mm, dy_mm);

            motion_send("G91");
            motion_send("G00 X%.3f Y%.3f", -dx_mm, -dy_mm);
            motion_send("G90");
            vTaskDelay(pdMS_TO_TICKS(300));
        }
        g_sm_state = STATE_PLACE;
        break;
    }

    case STATE_PLACE: {
        placement_t *p = &g_placements[g_comp_idx];
        ESP_LOGI(TAG, "=== PLACE %s @ (%.2f,%.2f) R%.0f ===",
                 p->designator, p->x_mm, p->y_mm, p->rotation);

        motion_send("G00 X%.3f Y%.3f", p->x_mm, p->y_mm);
        vTaskDelay(pdMS_TO_TICKS(500));
        motion_send("G00 Z-3");
        vTaskDelay(pdMS_TO_TICKS(300));
        motion_send("PUMP OFF");
        vTaskDelay(pdMS_TO_TICKS(200));
        motion_send("G00 Z20");

        g_comp_idx++;
        g_sm_state = (g_comp_idx < g_placement_count) ? STATE_PICK : STATE_DONE;
        break;
    }

    case STATE_DONE:
        ESP_LOGI(TAG, "=== ALL %d DONE ===", g_placement_count);
        motion_send("G00 Z%.2f", (double)Z_SAFE_MM);
        g_comp_idx = 0;
        g_sm_state = STATE_IDLE;
        break;

    case STATE_ERROR:
        ESP_LOGE(TAG, "=== ERROR — ESTOP ===");
        motion_send("M112");
        motion_send("PUMP OFF");
        g_sm_state = STATE_IDLE;
        break;

    default:
        break;
    }
}
