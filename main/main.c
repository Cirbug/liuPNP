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
 *   feeder    查看/设置飞达坐标
 *   help      帮助
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#if CONFIG_SOC_WIFI_SUPPORTED
#include "esp_wifi.h"
#include "esp_event.h"
#endif

#include "main.h"
#include "camera.h"
#include "doubao.h"
#include "vision.h"
#include "motion.h"
#include "calib.h"
#include "state_machine.h"
#include "web_server.h"
#include "cJSON.h"

static const char *TAG = "main";

/* ---- 全局变量定义 (跨模块共享, 声明在 main.h) ---- */
placement_t    g_placements[MAX_PLACEMENTS];
int            g_placement_count = 0;
int            g_comp_idx = 0;
calibration_t  g_calib;
feeder_config_t g_feeders[MAX_FEEDERS];
int            g_feeder_count = 0;

/* ---- WiFi 连接状态 ---- */
#if CONFIG_SOC_WIFI_SUPPORTED
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static int s_wifi_connected = 0;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected — retrying...");
        esp_wifi_connect();
        s_wifi_connected = 0;
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "WiFi connected, IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        s_wifi_connected = 1;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

bool wifi_is_connected(void) { return s_wifi_connected != 0; }

static void wifi_init_sta(void) {
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t inst_any_id, inst_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &inst_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &inst_got_ip));

    wifi_config_t wifi_cfg = {0};
    strcpy((char *)wifi_cfg.sta.ssid,     CONFIG_WIFI_SSID);
    strcpy((char *)wifi_cfg.sta.password, CONFIG_WIFI_PASSWORD);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi connecting to %s...", CONFIG_WIFI_SSID);

    /* 等待连接 (最多 10 秒) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(10000));
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi OK");
    } else {
        ESP_LOGW(TAG, "WiFi timeout — continuing without network");
    }
}
#else
bool wifi_is_connected(void) { return false; }
static void wifi_init_sta(void) {
    ESP_LOGW(TAG, "WiFi not supported on ESP32-P4; use C6 co-processor");
}
#endif /* CONFIG_SOC_WIFI_SUPPORTED */

/* ---- 飞达配置 ---- */
void feeders_init_defaults(void) {
    g_feeder_count = MAX_FEEDERS;
    for (int i = 0; i < MAX_FEEDERS; i++) {
        g_feeders[i].id = i + 1;
        g_feeders[i].x_mm = FEEDER_DEFAULT_X + i * 20.0f;
        g_feeders[i].y_mm = FEEDER_DEFAULT_Y;
        g_feeders[i].z_pickup_mm = FEEDER_DEFAULT_Z_PICKUP;
    }
    ESP_LOGI(TAG, "%d feeders initialized (default positions)", g_feeder_count);
}

feeder_config_t *feeder_find(int id) {
    for (int i = 0; i < g_feeder_count; i++) {
        if (g_feeders[i].id == id) return &g_feeders[i];
    }
    return NULL;
}

/* ================================================================
 * test 命令: 拍照 → 豆包 → 看描述
 * ================================================================ */
static void cmd_test(void) {
    ESP_LOGI(TAG, "=== VISION TEST ===");
    if (!wifi_is_connected()) {
        ESP_LOGW(TAG, "WiFi not connected — test may fail");
    }

    int64_t t_start = esp_timer_get_time();
    camera_fb_t *fb = camera_capture();
    if (!fb) {
        ESP_LOGE(TAG, "Camera capture failed — check OV5647 MIPI connection");
        return;
    }
    int64_t t_capture = esp_timer_get_time();

    cJSON *json = NULL;
    esp_err_t ret = doubao_analyze_image(fb->buf, fb->len,
        "Describe this image briefly (under 200 chars). "
        "If you see a PCB, note key components. "
        "If you see round fiducial marks, note approximate positions.",
        &json);
    int64_t t_api = esp_timer_get_time();

    if (ret == ESP_OK && json) {
        char *s = cJSON_Print(json);
        ESP_LOGI(TAG, "Vision result:\n%s", s);
        free(s);
        cJSON_Delete(json);
        ESP_LOGI(TAG, "✓ Vision test PASSED "
                 "(capture: %lld ms, api: %lld ms)",
                 (t_capture - t_start) / 1000,
                 (t_api - t_capture) / 1000);
    } else {
        ESP_LOGE(TAG, "✗ Vision test FAILED — check DOUBAO_API_KEY and WiFi "
                 "(api: %lld ms)", (t_api - t_capture) / 1000);
    }
    camera_release(fb);
}

/* ================================================================
 * calib 命令: 查看标定参数
 * ================================================================ */
static void cmd_calib_show(void) {
    ESP_LOGI(TAG, "pixel_to_mm:  (%.6f, %.6f)", g_calib.pixel_to_mm_x, g_calib.pixel_to_mm_y);
    ESP_LOGI(TAG, "cam_size:     %d x %d", g_calib.cam_width, g_calib.cam_height);
    ESP_LOGI(TAG, "cam_offset:   (%.3f, %.3f) mm", g_calib.cam_offset_x_mm, g_calib.cam_offset_y_mm);
    ESP_LOGI(TAG, "mark1_mm:     (%.3f, %.3f)", g_calib.mark1_mm_x, g_calib.mark1_mm_y);
}

/* ================================================================
 * feeder 命令: 查看飞达配置
 * ================================================================ */
static void cmd_feeder_show(void) {
    printf("Feeder config:\n");
    printf("  ID  X(mm)    Y(mm)    Z_pickup\n");
    printf("  --- -------  -------  --------\n");
    for (int i = 0; i < g_feeder_count; i++) {
        printf("  %-3d %-7.1f  %-7.1f  %-5.1f\n",
               g_feeders[i].id,
               g_feeders[i].x_mm,
               g_feeders[i].y_mm,
               g_feeders[i].z_pickup_mm);
    }
}

/* ================================================================
 * CLI 任务
 * ================================================================ */
static void cli_task(void *arg) {
    char line[128];
    ESP_LOGI(TAG, "CLI ready. Commands: test | home | calibrate | run | status | calib | feeder | help");

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
            if (g_placement_count == 0) {
                printf("No placements loaded. Put placements.csv on TF card.\n");
            } else {
                state_machine_trigger(STATE_PICK);
            }

        } else if (strcmp(line, "status") == 0) {
            ESP_LOGI(TAG, "State: %s | WiFi: %s | Component: %d/%d",
                     state_name(state_machine_get()),
                     wifi_is_connected() ? "OK" : "DOWN",
                     g_comp_idx, g_placement_count);

        } else if (strcmp(line, "calib") == 0) {
            cmd_calib_show();

        } else if (strcmp(line, "feeder") == 0) {
            cmd_feeder_show();

        } else if (strcmp(line, "stop") == 0) {
            state_machine_trigger(STATE_ERROR);

        } else if (strcmp(line, "help") == 0 || strcmp(line, "?") == 0) {
            printf("Commands:\n"
                   "  test      — Camera → Doubao vision test\n"
                   "  home      — Home all axes\n"
                   "  calibrate — Find fiducial mark\n"
                   "  run       — Auto pick-and-place\n"
                   "  status    — Show state, WiFi & progress\n"
                   "  calib     — Show calibration params\n"
                   "  feeder    — Show feeder positions\n"
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
    esp_err_t err = calib_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init FAILED: %s", esp_err_to_name(err));
    }

    /* 2. 摄像头 */
    err = camera_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init FAILED. Check wiring/power.");
    }

    /* 3. WiFi (带状态回调) */
    wifi_init_sta();

    /* 3.5 Web 调试控制台 */
    err = web_server_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Web server start FAILED");
    }

    /* 4. UART → S3 */
    err = motion_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Motion UART init FAILED — motor commands disabled");
    }

    /* 5. 飞达默认配置 */
    feeders_init_defaults();

    /* 6. 状态机 (加载标定 + 坐标) */
    state_machine_init();

    /* 7. CLI */
    xTaskCreate(cli_task, "cli", 4096, NULL, 5, NULL);

    /* 8. 状态机轮询 (低优先级) */
    while (1) {
        if (state_machine_get() != STATE_IDLE) {
            state_machine_run();
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}