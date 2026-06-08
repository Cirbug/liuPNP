/*
 * main.c — AI-PNP Pick-and-Place Controller (ESP32-P4)
 *
 * 启动流程:
 *   1. NVS 初始化
 *   2. 摄像头 (OV5647 MIPI CSI) 初始化 + WiFi
 *   3. UART (→ S3) 初始化
 *   4. 状态机初始化 (加载标定 + 坐标)
 *   5. CLI 控制台 (串口指令)
 *
 * 命令行:
 *   test      拍照 → 豆包 → 打印结果 (不接电机)
 *   home      归零
 *   calibrate 标定 Mark 点
 *   run       自动运行全部贴装
 *   status    查看状态
 *   calib     查看/修改标定参数
 *   help      帮助
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "camera.h"
#include "doubao.h"
#include "vision.h"
#include "motion.h"
#include "calib.h"
#include "state_machine.h"
#include "cJSON.h"

static const char *TAG = "main";
static calibration_t g_calib;

/* ================================================================
 * WiFi 初始化
 * ================================================================ */
static void wifi_init_sta(void) {
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    wifi_config_t wifi_cfg = {0};
    /* 从 sdkconfig 读 WiFi 配置, 或在此处硬编码 */
    strcpy((char *)wifi_cfg.sta.ssid,     CONFIG_WIFI_SSID);
    strcpy((char *)wifi_cfg.sta.password, CONFIG_WIFI_PASSWORD);

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    esp_wifi_start();

    ESP_LOGI(TAG, "WiFi connecting to %s...", CONFIG_WIFI_SSID);
    esp_wifi_connect();
    /* 简化版: 不等连接完成, main loop 会在任务中处理 */
}

/* ================================================================
 * test 命令: 拍照 → 豆包 → 看描述
 * ================================================================ */
static void cmd_test(void) {
    ESP_LOGI(TAG, "=== VISION TEST ===");
    camera_fb_t *fb = camera_capture();
    if (!fb) {
        ESP_LOGE(TAG, "Camera capture failed — check OV5647 MIPI connection");
        return;
    }

    cJSON *json = NULL;
    esp_err_t ret = doubao_analyze_image(fb->buf, fb->len,
        "Describe this image briefly (under 200 chars). "
        "If you see a PCB, note key components. "
        "If you see round fiducial marks, note approximate positions.",
        &json);

    if (ret == ESP_OK && json) {
        char *s = cJSON_Print(json);
        ESP_LOGI(TAG, "Vision result:\n%s", s);
        free(s);
        cJSON_Delete(json);
        ESP_LOGI(TAG, "✓ Vision test PASSED");
    } else {
        ESP_LOGE(TAG, "✗ Vision test FAILED — check DOUBAO_API_KEY and WiFi");
    }
    camera_release(fb);
}

/* ================================================================
 * calib 命令: 查看/修改标定参数
 * ================================================================ */
static void cmd_calib_show(void) {
    ESP_LOGI(TAG, "pixel_to_mm:  (%.6f, %.6f)", g_calib.pixel_to_mm_x, g_calib.pixel_to_mm_y);
    ESP_LOGI(TAG, "cam_size:     %d x %d", g_calib.cam_width, g_calib.cam_height);
    ESP_LOGI(TAG, "cam_offset:   (%.3f, %.3f) mm", g_calib.cam_offset_x_mm, g_calib.cam_offset_y_mm);
    ESP_LOGI(TAG, "mark1_mm:     (%.3f, %.3f)", g_calib.mark1_mm_x, g_calib.mark1_mm_y);
}

/* ================================================================
 * CLI 任务
 * ================================================================ */
static void cli_task(void *arg) {
    char line[128];
    ESP_LOGI(TAG, "CLI ready. Commands: test | home | calibrate | run | status | calib | help");

    while (1) {
        printf("PNP> ");
        fflush(stdout);

        if (fgets(line, sizeof(line), stdin) == NULL) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* 去掉尾部换行 */
        line[strcspn(line, "\r\n")] = '\0';

        if (strcmp(line, "test") == 0) {
            cmd_test();

        } else if (strcmp(line, "home") == 0) {
            state_machine_trigger(STATE_HOMING);

        } else if (strcmp(line, "calibrate") == 0) {
            state_machine_trigger(STATE_CALIBRATE);

        } else if (strcmp(line, "run") == 0) {
            state_machine_trigger(STATE_PICK);

        } else if (strcmp(line, "status") == 0) {
            ESP_LOGI(TAG, "State: %s | Component: %d/%d",
                     state_name(state_machine_get()),
                     /* g_comp_idx, g_placement_count — 需要暴露, 省略 */ 666, 0);

        } else if (strcmp(line, "calib") == 0) {
            cmd_calib_show();

        } else if (strcmp(line, "stop") == 0) {
            state_machine_trigger(STATE_ERROR);

        } else if (strcmp(line, "help") == 0 || strcmp(line, "?") == 0) {
            printf("Commands:\n"
                   "  test      — Camera → Doubao vision test\n"
                   "  home      — Home all axes\n"
                   "  calibrate — Find fiducial mark\n"
                   "  run       — Auto pick-and-place\n"
                   "  status    — Show state & progress\n"
                   "  calib     — Show calibration params\n"
                   "  stop      — Emergency stop\n"
                   "  help      — This message\n");

        } else if (strlen(line) > 0) {
            printf("Unknown command. Type 'help'.\n");
        }
    }
}

/* ================================================================
 * 主入口
 * ================================================================ */
void app_main(void) {
    ESP_LOGI(TAG, "AI-PNP P4 starting...");

    /* 1. NVS */
    calib_init();

    /* 2. 摄像头 */
    esp_err_t err = camera_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init FAILED. Check wiring/power.");
    }

    /* 3. WiFi */
    wifi_init_sta();

    /* 4. UART → S3 (可选, test 命令不需要) */
    // motion_init();

    /* 5. 状态机 */
    state_machine_init();

    /* 6. CLI */
    xTaskCreate(cli_task, "cli", 4096, NULL, 5, NULL);

    /* 7. 状态机轮询 (低优先级) */
    while (1) {
        if (state_machine_get() != STATE_IDLE) {
            state_machine_run();
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
