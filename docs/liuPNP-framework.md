# liuPNP — AI-Native 贴片机系统

> 抛弃 OpenPNP，用多模态大模型替代传统视觉算法和流程编排。
> 目标：ESP32 赛道比赛作品，低成本、组件化、AI 驱动。

---

## 1. 硬件架构

```
┌──────────────────────────────────────────────────────────────┐
│                     PC (mjpg-streamer)                        │
│                 仅作为 HTTP 图像推流服务器                       │
│                 两个 USB 摄像头 → 推流                           │
└───────────────┬──────────────────────────────────────────────┘
                │ WiFi / 有线
                ▼
┌──────────────────────────────┐    UART    ┌──────────────────────────────┐
│     ESP32-P4-WiFi6 (主控)      │◄─────────►│   ESP32-S3-DevKitC-1 (执行层)  │
│     ────────────────────       │  GPIO 串口  │   ──────────────────────       │
│  • AI 推理 + 视觉分析           │           │  • G-code 转发                 │
│  • 贴装流程状态机               │           │  • 传感器采集                   │
│  • 标定参数管理 (NVS)           │           │  • OV5640 DVP 下视摄像头         │
│  • OV5647 MIPI CSI 上视摄像头    │           │  • USB OTG → MKS 主控           │
│  • TF 卡 (贴片坐标 CSV)         │           │                                │
└───────────────┬──────────────┘           └───────────────┬──────────────┘
                │                                          │ USB OTG
                ▼                                          ▼
      OV5647 MIPI CSI                             MKS 主控 (Type-B USB)
      上视摄像头 (定焦)                             步进电机 / 飞达 / 吸嘴
      检查吸嘴取料姿态
```

| 角色 | 芯片 | 摄像头 | 连接 |
|------|------|--------|------|
| **主控** | ESP32-P4-WiFi6 | OV5647 MIPI CSI（上视，定焦） | WiFi → PC / UART → S3 / TF 卡 |
| **执行层** | ESP32-S3-DevKitC-1 N16R8 | OV5640 DVP（下视，拍 PCB Mark 点） | USB OTG → MKS / UART → P4 |
| **运动控制** | MKS 主控 | — | Type-B USB ← S3 |
| **图像源** | PC | 2× USB 摄像头 | WiFi → P4 |

**摄像头分工**：
- **下视（核心）**：OV5640 DVP，连 S3。拍摄 PCB Mark 点，计算 PCB 实际位置与理论坐标的偏差
- **上视（锦上添花）**：OV5647 MIPI CSI，连 P4。吸嘴取料后检查元件姿态和角度偏移

---

## 2. 软件架构（三层）

```
┌─────────────────────────────────────┐
│  L1: PC 图像服务器                    │
│  mjpg-streamer 推流                  │
│  不参与决策，只提供原始图像              │
└──────────────┬──────────────────────┘
               │ HTTP (jpg 帧)
               ▼
┌─────────────────────────────────────┐
│  L2: ESP32-P4 AI 核心                │
│  ┌───────────────────────────────┐  │
│  │ 任务调度器 (FreeRTOS)          │  │
│  │  ├─ camera    (MIPI CSI 采集)  │  │
│  │  ├─ http      (拉 PC 图像帧)   │  │
│  │  ├─ doubao    (多模态视觉 API)  │  │
│  │  ├─ vision    (Mark 点/元件识别)│  │
│  │  ├─ motion    (运动规划)        │  │
│  │  ├─ calib     (标定流程)        │  │
│  │  ├─ placements(贴片坐标加载)     │  │
│  │  ├─ storage   (NVS/SPIFFS)     │  │
│  │  └─ state_machine (流程编排)    │  │
│  └───────────────────────────────┘  │
└──────────────┬──────────────────────┘
               │ UART (G-code 指令)
               ▼
┌─────────────────────────────────────┐
│  L3: ESP32-S3 执行层                  │
│  • 接收 G-code 指令                   │
│  • USB OTG → MKS 主控转发             │
│  • OV5640 DVP 下视摄像头采集           │
│  • 回传传感器状态                      │
└──────────────┬──────────────────────┘
               │ USB OTG
               ▼
         MKS 主控 (步进电机)
```

---

## 3. 项目目录结构

```
liuPNP/
├── CMakeLists.txt              # ESP-IDF 项目根
├── sdkconfig.defaults.esp32p4  # P4 默认 Kconfig
├── sdkconfig.defaults.esp32s3  # S3 默认 Kconfig
├── main/
│   ├── CMakeLists.txt          # 主组件依赖声明
│   ├── Kconfig.projbuild       # 自定义 Kconfig (WiFi/API Key)
│   ├── main.c                  # P4 入口 + WiFi 初始化
│   ├── main.h                  # 全局定义
│   ├── camera.c/h              # 摄像头模块（MIPI CSI）
│   ├── http_client.c/h         # HTTP 拉图模块
│   ├── doubao.c/h              # 豆包 Vision API 调用
│   ├── vision.c/h              # 视觉分析（Mark/元件识别）
│   ├── motion.c/h              # 运动规划（坐标→G-code）
│   ├── calib.c/h               # LLM 驱动标定流程
│   ├── placements.c/h          # 贴片坐标 CSV 解析
│   ├── base64.c/h              # Base64 编解码（图片编码）
│   └── state_machine.c/h       # 贴装流程状态机
├── components/
│   ├── cJSON/                  # JSON 解析库
│   └── esp_camera/             # ESP 摄像头驱动（当前为桩）
├── docs/
│   ├── MCP-INTEGRATION.md      # MCP 双向集成方案
│   └── liuPNP-framework.md     # 本文档
└── build/
    └── liuPNP.bin              # 编译产物
```

---

## 4. 贴装流程（状态机）

```
空闲
 │
 ├─► [加载贴片任务] ──► 解析 TF 卡 CSV 文件
 │
 ├─► [PCB 定位] ──► P4 拉 S3 下视图 → Doubao 识别 Mark 点
 │                  计算 PCB 实际坐标系与理论坐标的仿射变换
 │
 ├─► [取料] ──► 移动到飞达位置 → 吸嘴吸取元件
 │
 ├─► [上视检查] ──► OV5647 拍摄吸嘴 → Doubao 判断姿态/角度
 │     ├─ OK ──► 继续
 │     └─ NG ──► 抛料 → 重新取料
 │
 ├─► [角度补偿] ──► 根据上视识别结果修正放置角度
 │
 ├─► [贴装] ──► 移动到 PCB 目标位置 → 放置元件
 │
 ├─► [检查] ──► 可选手动/自动确认
 │
 └─► [下一个元件] ──► 循环直到任务完成
```

**LLM 角色限定**：Doubao Vision API **只做视觉识别**（找 Mark 点、判断吸嘴状态、识别角度/偏移），不做运动规划。

---

## 5. 通信协议

### P4 ↔ S3（UART）

```
P4 → S3:  "G0 X12.5 Y34.2 Z5.0\n"     (G-code 指令)
S3 → P4:  "OK\n"                        (确认)
P4 → S3:  "CAPTURE\n"                   (请求拍下视图)
S3 → P4:  "IMG:1024,768\n"              (图像尺寸)
          [raw pixel data]
```

### P4 ↔ PC（HTTP）

```
P4 GET  http://pc_ip:8080/?action=snapshot
PC →   JPEG image stream
```

### P4 ↔ Doubao（HTTPS）

```
POST https://ark.cn-beijing.volces.com/api/v3/chat/completions
{
  "model": "doubao-vision-pro-32k",
  "messages": [{
    "role": "user",
    "content": [
      {"type": "image_url", "image_url": {"url": "data:image/jpeg;base64,..."}},
      {"type": "text", "text": "识别图像中的PCB Mark点坐标"}
    ]
  }]
}
```

---

## 6. 关键配置

| 配置项 | 位置 | 说明 |
|--------|------|------|
| WiFi SSID/密码 | `Kconfig.projbuild` 或 `menuconfig` | P4 连 WiFi |
| 豆包 API Key | `doubao.h` | ARK API Key |
| 豆包 API URL | `doubao.h` | 默认 `ark.cn-beijing.volces.com` |
| PCB Mark 点模板 | `calib.c` | 标定生成，存 NVS |
| 贴片坐标 | TF 卡 CSV | 嘉立创 EDA 导出 |

---

## 7. 当前开发状态

| 模块 | 状态 | 备注 |
|------|------|------|
| 项目框架 | ✅ 编译通过 | ESP32-P4 目标，IDF v5.4 |
| CMake/构建 | ✅ 完成 | 根 + main CMakeLists 已修复 |
| WiFi | ⚠️ 条件编译 | P4 无内置 WiFi，需 C6 协处理器 |
| 摄像头驱动 | ❌ 桩实现 | `esp_camera_stub.c`，需改 `esp_driver_cam` MIPI CSI |
| 豆包 API | ⚠️ 框架完成 | `doubao.h` 需填 API Key |
| HTTP 拉图 | ⬜ 待写 | 从 PC mjpg-streamer 拉帧 |
| 视觉分析 | ⬜ 待写 | Prompt 工程 + 坐标解析 |
| 运动规划 | ⚠️ 框架完成 | G-code 生成逻辑待验证 |
| 标定流程 | ⬜ 待写 | LLM 驱动标定 |
| CSV 解析 | ⚠️ 框架完成 | 嘉立创 EDA 导出格式适配 |
| 状态机 | ⚠️ 框架完成 | 流程编排逻辑待验证 |
| S3 执行层 | ⬜ 待写 | 独立工程，G-code 转发 + DVP 采集 |
| MCP 集成 | 📄 设计文档 | `docs/MCP-INTEGRATION.md` |
| 实物测试 | ⬜ 未开始 | 硬件已就位 |

---

## 8. 待办优先级

**P0 — 阻塞实物测试：**
- [ ] P4 MIPI CSI 摄像头驱动（`esp_driver_cam`）
- [ ] 豆包 API Key 配置 + 验证 API 连通
- [ ] PC mjpg-streamer 推流 + P4 HTTP 拉图

**P1 — 核心流程：**
- [ ] 视觉 Prompt 工程（Mark 点识别）
- [ ] UART 通信 P4 ↔ S3
- [ ] S3 DVP 摄像头 + G-code 转发

**P2 — 完整系统：**
- [ ] 标定流程实现
- [ ] 状态机完整联调
- [ ] 实物贴装测试

**P3 — 优化：**
- [ ] WiFi 迁移到 C6 协处理器
- [ ] MCP 双向集成
- [ ] 错误恢复 / 抛料重试
