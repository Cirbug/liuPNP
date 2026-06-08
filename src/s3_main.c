/*
 * AI-PNP 执行层 — ESP32-S3
 *
 * 职责:
 *   1. UART接收P4指令 → 透传G-code到MKS (USB CDC)
 *   2. 第二个OV5640摄像头(DVP) → 拍照 → SPI给P4
 *   3. GPIO管理: PWM照明、气泵/真空阀、限位传感器
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_camera.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_slave.h"

static const char *TAG = "PNP_EXEC";

/* ---- UART到P4 ---- */
#define UART_P4_PORT    UART_NUM_1
#define UART_P4_TX      4
#define UART_P4_RX      5
#define UART_P4_BAUD    921600

/* ---- USB CDC到MKS ---- */
// 通过USB OTG连接MKS, TinyUSB CDC

/* ---- GPIO定义 ---- */
#define GPIO_LIMIT_X_MIN    6
#define GPIO_LIMIT_X_MAX    7
#define GPIO_LIMIT_Y_MIN    8
#define GPIO_LIMIT_Y_MAX    9
#define GPIO_LIMIT_Z_MIN    10
#define GPIO_PUMP_RELAY     12    // 真空泵继电器
#define GPIO_LED_PWM        13    // 环形灯PWM

/* ---- DVP摄像头引脚(OV5640) ---- */
#define DVP_PIN_PWDN    -1
#define DVP_PIN_RESET   -1
#define DVP_PIN_XCLK    14
#define DVP_PIN_SIOD    2
#define DVP_PIN_SIOC    3
#define DVP_PIN_D7      16
#define DVP_PIN_D6      17
#define DVP_PIN_D5      18
#define DVP_PIN_D4      19
#define DVP_PIN_D3      20
#define DVP_PIN_D2      21
#define DVP_PIN_D1      26
#define DVP_PIN_D0      27
#define DVP_PIN_VSYNC   35
#define DVP_PIN_HREF    36
#define DVP_PIN_PCLK    37

/* ---- 命令缓冲 ---- */
#define CMD_BUF_SIZE 256
static char cmd_buf[CMD_BUF_SIZE];
static int  cmd_idx = 0;

/* ---- 转发G-code到MKS ---- */
static void mks_send(const char *cmd) {
    // TODO: TinyUSB CDC write
    ESP_LOGI(TAG, "→ MKS: %s", cmd);
}

/* ---- GPIO初始化 ---- */
static void gpio_init(void) {
    // 限位开关 (上拉输入, 触发为低)
    gpio_config_t input_cfg = {
        .pin_bit_mask = (1ULL << GPIO_LIMIT_X_MIN) |
                        (1ULL << GPIO_LIMIT_X_MAX) |
                        (1ULL << GPIO_LIMIT_Y_MIN) |
                        (1ULL << GPIO_LIMIT_Y_MAX) |
                        (1ULL << GPIO_LIMIT_Z_MIN),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&input_cfg);

    // 继电器 (推挽输出)
    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << GPIO_PUMP_RELAY),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
    };
    gpio_config(&out_cfg);
    gpio_set_level(GPIO_PUMP_RELAY, 0); // 默认关闭

    // LED PWM
    ledc_timer_config_t ledc_timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num       = LEDC_TIMER_0,
        .freq_hz         = 5000,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_ch = {
        .gpio_num   = GPIO_LED_PWM,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_0,
        .timer_sel  = LEDC_TIMER_0,
        .duty       = 0,
        .hpoint     = 0,
    };
    ledc_channel_config(&ledc_ch);
}

/* ---- 命令解析 ---- */
static void handle_command(const char *cmd) {
    char cmd_type[8] = {0};
    sscanf(cmd, "%7s", cmd_type);

    if (strcmp(cmd_type, "PUMP") == 0) {
        char action[4] = {0};
        sscanf(cmd, "PUMP %3s", action);
        if (strcmp(action, "ON") == 0) {
            gpio_set_level(GPIO_PUMP_RELAY, 1);
            ESP_LOGI(TAG, "PUMP ON");
        } else {
            gpio_set_level(GPIO_PUMP_RELAY, 0);
            ESP_LOGI(TAG, "PUMP OFF");
        }
    }
    else if (strcmp(cmd_type, "LED") == 0) {
        int duty;
        if (sscanf(cmd, "LED %d", &duty) == 1) {
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
        }
    }
    else if (strcmp(cmd_type, "M112") == 0) {
        // 紧急停止
        gpio_set_level(GPIO_PUMP_RELAY, 0);
        mks_send(cmd); // 转发到MKS
    }
    else {
        // 默认: 透传G-code给MKS
        mks_send(cmd);
    }
}

/* ---- UART接收任务: 从P4收指令 ---- */
static void uart_rx_task(void *arg) {
    uint8_t ch;
    while (1) {
        int len = uart_read_bytes(UART_P4_PORT, &ch, 1, pdMS_TO_TICKS(100));
        if (len > 0) {
            if (ch == '\n' || ch == '\r') {
                if (cmd_idx > 0) {
                    cmd_buf[cmd_idx] = '\0';
                    handle_command(cmd_buf);
                    cmd_idx = 0;
                }
            } else if (cmd_idx < CMD_BUF_SIZE - 1) {
                cmd_buf[cmd_idx++] = ch;
            }
        }
    }
}

/* ---- DVP摄像头初始化 ---- */
static void dvp_camera_init(void) {
    camera_config_t config = {
        .pin_pwdn  = DVP_PIN_PWDN,
        .pin_reset = DVP_PIN_RESET,
        .pin_xclk  = DVP_PIN_XCLK,
        .pin_siod  = DVP_PIN_SIOD,
        .pin_sioc  = DVP_PIN_SIOC,
        .pin_d7    = DVP_PIN_D7,
        .pin_d6    = DVP_PIN_D6,
        .pin_d5    = DVP_PIN_D5,
        .pin_d4    = DVP_PIN_D4,
        .pin_d3    = DVP_PIN_D3,
        .pin_d2    = DVP_PIN_D2,
        .pin_d1    = DVP_PIN_D1,
        .pin_d0    = DVP_PIN_D0,
        .pin_vsync = DVP_PIN_VSYNC,
        .pin_href  = DVP_PIN_HREF,
        .pin_pclk  = DVP_PIN_PCLK,

        .xclk_freq_hz = 20000000,
        .ledc_timer   = LEDC_TIMER_1,
        .ledc_channel = LEDC_CHANNEL_1,

        .pixel_format = PIXFORMAT_JPEG,
        .frame_size   = FRAMESIZE_UXGA,
        .jpeg_quality = 12,
        .fb_count     = 2,

        .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
    };

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DVP Camera init failed: 0x%x", err);
    } else {
        ESP_LOGI(TAG, "DVP Camera initialized (OV5640 DVP, UXGA JPEG)");
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "=== AI-PNP Executor (S3) ===");

    gpio_init();

    // UART: 接收P4指令
    uart_config_t uart_cfg = {
        .baud_rate = UART_P4_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };
    uart_param_config(UART_P4_PORT, &uart_cfg);
    uart_set_pin(UART_P4_PORT, UART_P4_TX, UART_P4_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(UART_P4_PORT, 2048, 2048, 0, NULL, 0);

    // DVP摄像头
    dvp_camera_init();

    // 初始化USB CDC (连接MKS)
    // TODO: TinyUSB CDC init

    ESP_LOGI(TAG, "Ready. Waiting for P4 commands...");

    xTaskCreate(uart_rx_task, "uart_rx", 4096, NULL, 10, NULL);
}
