/*
 * motion.c — MKS 指令发送实现
 */

#include "motion.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "motion";

esp_err_t motion_init(void) {
    uart_config_t uart_cfg = {
        .baud_rate  = MOTION_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(MOTION_UART_PORT, 256, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed");
        return err;
    }
    err = uart_param_config(MOTION_UART_PORT, &uart_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed");
        return err;
    }
    err = uart_set_pin(MOTION_UART_PORT, MOTION_UART_TX, MOTION_UART_RX,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) ESP_LOGE(TAG, "uart_set_pin failed");
    else ESP_LOGI(TAG, "UART%d init @ %d baud (TX=%d RX=%d)",
                  MOTION_UART_PORT, MOTION_UART_BAUD,
                  MOTION_UART_TX, MOTION_UART_RX);
    return err;
}

void motion_send(const char *fmt, ...) {
    char buf[128];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf) - 1, fmt, args);
    va_end(args);
    if (len > 0 && len < (int)sizeof(buf) - 1) {
        buf[len]     = '\n';
        buf[len + 1] = '\0';
        int written = uart_write_bytes(MOTION_UART_PORT, buf, len + 1);
        if (written > 0)
            ESP_LOGI(TAG, "→ S3: %s", buf);
    }
}
