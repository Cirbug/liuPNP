/*
 * web_server.h — liuPNP Web 调试控制台
 */

#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include "esp_err.h"

/* 启动 HTTP 服务器 (端口 80)，注册所有路由 */
esp_err_t web_server_start(void);

/* 记录 G-code 指令到 Web 日志 */
void web_log_cmd(const char *cmd);

#endif