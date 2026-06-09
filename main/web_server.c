/*
 * web_server.c — liuPNP Web 调试控制台
 *
 * 提供:
 *   1. GET  /          → 控制面板 HTML 页面 (内嵌)
 *   2. GET  /status    → JSON: { state, wifi, comp_idx, comp_total }
 *   3. POST /api/gcode → 发送 G-code 指令到 MKS
 *   4. GET  /api/log   → 最近 N 条指令日志
 *
 * HTTP 服务器运行在端口 80。
 */

#include "web_server.h"
#include "main.h"
#include "motion.h"
#include "state_machine.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "web";

/* ---- 指令日志环形缓冲 ---- */
#define LOG_MAX_ENTRIES  50
static char g_cmd_log[LOG_MAX_ENTRIES][128];
static int  g_cmd_log_idx = 0;
static int  g_cmd_log_cnt = 0;

void web_log_cmd(const char *cmd) {
    if (g_cmd_log_cnt < LOG_MAX_ENTRIES) {
        snprintf(g_cmd_log[g_cmd_log_cnt], sizeof(g_cmd_log[0]), "%s", cmd);
        g_cmd_log_cnt++;
    } else {
        snprintf(g_cmd_log[g_cmd_log_idx], sizeof(g_cmd_log[0]), "%s", cmd);
        g_cmd_log_idx = (g_cmd_log_idx + 1) % LOG_MAX_ENTRIES;
    }
}

/* ================================================================
 * HTML 页面 (内嵌)
 * ================================================================ */
static const char INDEX_HTML[] =
"<!DOCTYPE html>\n"
"<html lang=\"zh-CN\">\n"
"<head>\n"
"<meta charset=\"UTF-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
"<title>liuPNP — MKS 调试控制台</title>\n"
"<style>\n"
"*{box-sizing:border-box;margin:0;padding:0}\n"
"body{font-family:'Segoe UI',sans-serif;background:#0f1923;color:#d4d4d4;max-width:700px;margin:0 auto;padding:16px}\n"
"h1{color:#00d4aa;font-size:20px;margin-bottom:4px}\n"
".sub{color:#888;font-size:12px;margin-bottom:16px}\n"
".status{display:flex;gap:12px;margin-bottom:16px}\n"
".status .chip{background:#1a2a36;border:1px solid #2a3a46;border-radius:6px;padding:6px 12px;font-size:13px}\n"
".status .chip span{color:#00d4aa;font-weight:bold}\n"
".section{background:#1a2a36;border:1px solid #2a3a46;border-radius:8px;padding:12px;margin-bottom:12px}\n"
".section h2{color:#b0b0b0;font-size:14px;margin-bottom:8px;text-transform:uppercase;letter-spacing:1px}\n"
"input,button,select{font-family:inherit;border:1px solid #2a3a46;border-radius:4px;padding:8px 12px;font-size:14px;background:#0f1923;color:#d4d4d4;outline:none}\n"
"input:focus,select:focus{border-color:#00d4aa}\n"
"button{cursor:pointer;background:#00d4aa;color:#0f1923;border:none;font-weight:bold;transition:background .2s}\n"
"button:hover{background:#00e4ba}\n"
"button.danger{background:#e05555;color:#fff}\n"
"button.danger:hover{background:#f06565}\n"
".jog{display:grid;grid-template-columns:repeat(3,60px);grid-template-rows:repeat(3,44px);gap:4px;justify-content:center}\n"
".jog button{width:60px;height:44px;font-size:18px}\n"
".jog .xy-label{display:flex;align-items:center;justify-content:center;font-size:12px;color:#888}\n"
".gcode-input{display:flex;gap:8px;margin-bottom:8px}\n"
".gcode-input input{flex:1}\n"
".gcode-input button{width:100px}\n"
"#log{font-family:'Consolas','Courier New',monospace;font-size:12px;max-height:300px;overflow-y:auto;background:#0f1923;border:1px solid #2a3a46;border-radius:4px;padding:8px}\n"
"#log div{padding:2px 0;border-bottom:1px solid #1a2a36}\n"
"#log .gcode{color:#00d4aa}\n"
"#log .err{color:#e05555}\n"
".quick-btns{display:flex;flex-wrap:wrap;gap:6px}\n"
".quick-btns button{font-size:12px;padding:5px 10px}\n"
"</style>\n"
"</head>\n"
"<body>\n"
"<h1>liuPNP MKS Console</h1>\n"
"<div class=\"sub\">ESP32-P4 贴片机调试面板</div>\n"
"<div class=\"status\">\n"
"  <div class=\"chip\">状态: <span id=\"st_state\">—</span></div>\n"
"  <div class=\"chip\">WiFi: <span id=\"st_wifi\">—</span></div>\n"
"  <div class=\"chip\">进度: <span id=\"st_progress\">—</span></div>\n"
"</div>\n"
"\n"
"<div class=\"section\">\n"
"  <h2>手动 Jog 控制</h2>\n"
"  <div style=\"display:flex;gap:8px;margin-bottom:8px;justify-content:center\">\n"
"    <label>步距(mm):</label>\n"
"    <select id=\"jog_step\">\n"
"      <option value=\"0.1\">0.1</option>\n"
"      <option value=\"0.5\" selected>0.5</option>\n"
"      <option value=\"1\">1</option>\n"
"      <option value=\"5\">5</option>\n"
"      <option value=\"10\">10</option>\n"
"      <option value=\"50\">50</option>\n"
"    </select>\n"
"  </div>\n"
"  <div class=\"jog\">\n"
"    <div></div>\n"
"    <button onclick=\"jog('Y',1)\">Y+</button>\n"
"    <div></div>\n"
"    <button onclick=\"jog('X',-1)\">X-</button>\n"
"    <div class=\"xy-label\">XY</div>\n"
"    <button onclick=\"jog('X',1)\">X+</button>\n"
"    <div></div>\n"
"    <button onclick=\"jog('Y',-1)\">Y-</button>\n"
"    <div></div>\n"
"  </div>\n"
"  <div style=\"display:flex;gap:4px;justify-content:center;margin-top:8px\">\n"
"    <button onclick=\"jog('Z',1)\" style=\"width:60px\">Z+</button>\n"
"    <button onclick=\"jog('Z',-1)\" style=\"width:60px\">Z-</button>\n"
"  </div>\n"
"</div>\n"
"\n"
"<div class=\"section\">\n"
"  <h2>G-code 指令</h2>\n"
"  <div class=\"gcode-input\">\n"
"    <input id=\"gcode_cmd\" type=\"text\" placeholder=\"例: G0 X10 Y20 Z5\" onkeydown=\"if(event.key==='Enter')sendGcode()\">\n"
"    <button onclick=\"sendGcode()\">发送</button>\n"
"  </div>\n"
"  <div class=\"quick-btns\">\n"
"    <button onclick=\"sendCmd('G28')\">G28 归零</button>\n"
"    <button onclick=\"sendCmd('G90')\">G90 绝对</button>\n"
"    <button onclick=\"sendCmd('G91')\">G91 相对</button>\n"
"    <button onclick=\"sendCmd('M114')\">M114 位置</button>\n"
"    <button onclick=\"sendCmd('M17')\">M17 使能</button>\n"
"    <button onclick=\"sendCmd('M18')\">M18 释放</button>\n"
"    <button onclick=\"sendCmd('PUMP ON')\">吸嘴 ON</button>\n"
"    <button onclick=\"sendCmd('PUMP OFF')\">吸嘴 OFF</button>\n"
"    <button onclick=\"sendCmd('M112')\" class=\"danger\">M112 急停</button>\n"
"  </div>\n"
"</div>\n"
"\n"
"<div class=\"section\">\n"
"  <h2>指令日志</h2>\n"
"  <div id=\"log\"><div style=\"color:#888\">等待指令...</div></div>\n"
"</div>\n"
"\n"
"<script>\n"
"async function sendGcode() {\n"
"  var inp=document.getElementById('gcode_cmd');\n"
"  var cmd=inp.value.trim();\n"
"  if(!cmd)return;\n"
"  await sendCmd(cmd);\n"
"  inp.value='';\n"
"  inp.focus();\n"
"}\n"
"async function sendCmd(cmd) {\n"
"  try{\n"
"    var r=await fetch('/api/gcode',{method:'POST',headers:{'Content-Type':'text/plain'},body:cmd});\n"
"    var t=await r.text();\n"
"    addLog(cmd,t.startsWith('OK')?'gcode':'err');\n"
"  }catch(e){addLog(cmd,'err');}\n"
"}\n"
"function jog(axis,dir) {\n"
"  var step=parseFloat(document.getElementById('jog_step').value);\n"
"  sendCmd('G91\\\\nG0 '+axis+String(dir*step)+'\\\\nG90');\n"
"}\n"
"function addLog(cmd,cls) {\n"
"  var log=document.getElementById('log');\n"
"  if(log.children.length>=50)log.removeChild(log.firstChild);\n"
"  var d=document.createElement('div');\n"
"  d.className=cls;\n"
"  d.textContent=cmd;\n"
"  log.appendChild(d);\n"
"  log.scrollTop=log.scrollHeight;\n"
"}\n"
"async function refreshStatus() {\n"
"  try{\n"
"    var r=await fetch('/status');\n"
"    var j=await r.json();\n"
"    document.getElementById('st_state').textContent=j.state||'?';\n"
"    document.getElementById('st_wifi').textContent=j.wifi?'OK':'DOWN';\n"
"    document.getElementById('st_progress').textContent=(j.comp_idx||0)+'/'+(j.comp_total||0);\n"
"  }catch(e){}\n"
"}\n"
"setInterval(refreshStatus,2000);\n"
"refreshStatus();\n"
"</script>\n"
"</body>\n"
"</html>\n";

/* ================================================================
 * HTTP 处理器
 * ================================================================ */

/* GET / → HTML 页面 */
static esp_err_t handler_root(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, INDEX_HTML, strlen(INDEX_HTML));
    return ESP_OK;
}

/* GET /status → JSON */
static esp_err_t handler_status(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "state", state_name(state_machine_get()));
    cJSON_AddBoolToObject(root, "wifi", (wifi_is_connected()) ? 1 : 0);
    cJSON_AddNumberToObject(root, "comp_idx", g_comp_idx);
    cJSON_AddNumberToObject(root, "comp_total", g_placement_count);
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, s, strlen(s));
    free(s);
    return ESP_OK;
}

/* POST /api/gcode */
static esp_err_t handler_gcode(httpd_req_t *req) {
    char buf[128] = {0};
    int recv = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (recv <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[recv] = '\0';

    /* 去掉尾部换行 */
    buf[strcspn(buf, "\r\n")] = '\0';

    if (strlen(buf) == 0) {
        httpd_resp_sendstr(req, "ERR: empty command");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Web → MKS: %s", buf);
    web_log_cmd(buf);

    /* 支持 \\n 分隔的多条指令 */
    char *saveptr;
    char *cmd = strtok_r(buf, "\\n", &saveptr);
    while (cmd) {
        /* 去除前后空格 */
        while (*cmd == ' ') cmd++;
        if (strlen(cmd) > 0) {
            motion_send("%s", cmd);
        }
        cmd = strtok_r(NULL, "\\n", &saveptr);
    }

    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

/* GET /api/log → 最近的指令日志 */
static esp_err_t handler_log(httpd_req_t *req) {
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < g_cmd_log_cnt; i++) {
        cJSON_AddItemToArray(arr, cJSON_CreateString(g_cmd_log[i]));
    }
    char *s = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, s, strlen(s));
    free(s);
    return ESP_OK;
}

/* ================================================================
 * 服务器启动
 * ================================================================ */
esp_err_t web_server_start(void) {
    httpd_handle_t server = NULL;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.max_open_sockets = 4;

    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    httpd_uri_t uri_root = {
        .uri      = "/",
        .method   = HTTP_GET,
        .handler  = handler_root,
    };
    httpd_register_uri_handler(server, &uri_root);

    httpd_uri_t uri_status = {
        .uri      = "/status",
        .method   = HTTP_GET,
        .handler  = handler_status,
    };
    httpd_register_uri_handler(server, &uri_status);

    httpd_uri_t uri_gcode = {
        .uri      = "/api/gcode",
        .method   = HTTP_POST,
        .handler  = handler_gcode,
    };
    httpd_register_uri_handler(server, &uri_gcode);

    httpd_uri_t uri_log = {
        .uri      = "/api/log",
        .method   = HTTP_GET,
        .handler  = handler_log,
    };
    httpd_register_uri_handler(server, &uri_log);

    ESP_LOGI(TAG, "Web console started on http://<P4_IP>/");
    return ESP_OK;
}