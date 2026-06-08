/*
 * motion.h — MKS 指令发送 (UART → ESP32-S3)
 * P4 通过 UART 把 G-code 发给 S3, S3 转发给 MKS 控制器
 */

#ifndef MOTION_H
#define MOTION_H

#include <stdarg.h>
#include "driver/uart.h"

/* UART 到 S3 */
#define MOTION_UART_PORT    UART_NUM_1
#define MOTION_UART_TX      17
#define MOTION_UART_RX      18
#define MOTION_UART_BAUD    921600

/* 初始化 UART */
esp_err_t motion_init(void);

/* 发送格式化字符串 (自动追加 \n) */
void motion_send(const char *fmt, ...);

#endif
