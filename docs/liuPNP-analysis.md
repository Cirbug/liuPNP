# liuPNP 项目分析报告

> 分析日期：2026-06-09
> 项目仓库：https://github.com/Cirbug/liuPNP.git

---

## 1. 项目概述

**liuPNP** 是一个 **ESP32 赛道比赛作品**，目标是用 **多模态大模型（豆包 Vision API）替代传统机器视觉算法和流程编排**，实现全自动 PCB 贴片机（Pick-and-Place Machine）。

- **类型**：AI-Native 嵌入式系统
- **平台**：ESP32-P4-WiFi6（主控）+ ESP32-S3（执行层）+ MKS（运动控制）
- **SDK**：ESP-IDF v5.4 + FreeRTOS
- **核心理念**：抛弃 OpenPNP，用 AI 驱动视觉识别和流程编排
- **比赛定位**：低成本、组件化、AI 驱动的 ESP32 赛道作品

---

## 2. 硬件架构

```
┌──────────────────────────────────────────────────────────────┐
│                     PC (mjpg-streamer)                        │
│                 仅作为 HTTP 图像推流服务器                       │
│                 两个 USB 摄像头 → 推流                           │
└───────────────┬──────────────────────────────────────────────┘
                │ WiFi / 有线
                ▼
┌──────────────────────────────┐    UART    ┌──────────────────────────────┐
│     ESP32-P4-WiFi6 (主控)    │◄─────────►│   ESP32-S3-DevKitC-1 (执行层) │
│     ────────────────────     │  GPIO 串口  │   ──────────────────────      │
│  • AI 推理 + 视觉分析         │           │  • G-code 转发                 │
│  • 贴装流程状态机             │           │  • 传感器采集                   │
│  • 标定参数管理 (NVS)         │           │  • OV5640 DVP 下视摄像头         │
│  • OV5647 MIPI CSI 上视摄像头  │           │  • USB OTG → MKS 主控           │
│  • TF 卡 (贴片坐标 CSV)       │           │                                │
└───────────────┬──────────────┘           └───────────────┬──────────────┘
                │                                          │ USB OTG
                ▼                                          ▼
      OV5647 MIPI CSI                             MKS 主控 (Type-B USB)
      上视摄像头 (定焦)                             步进电机 / 飞达 / 吸嘴
      检查吸嘴取料姿态
```

### 各层角色

| 层级 | 芯片 | 摄像头 | 职责 |
|------|------|--------|------|
| **L1 PC** | PC | 2× USB 摄像头 | HTTP 图像推流，不参与决策 |
| **L2 主控** | ESP32-P4-WiFi6 | OV5647 MIPI CSI（上视，定焦） | AI 推理、流程编排、标定管理 |
| **L3 执行层** | ESP32-S3-DevKitC-1 N16R8 | OV5640 DVP（下视，拍 PCB Mark 点） | G-code 转发、传感器回传 |
| **L4 运动控制** | MKS 主控 | — | 步进电机 / 飞达 / 吸嘴驱动 |

### 双摄像头分工

| 摄像头 | 接口 | 连接 | 用途 | 重要性 |
|--------|------|------|------|--------|
| OV5640 DVP | 下视 | S3 | 拍摄 PCB Mark 点，计算 PCB 实际位置与理论坐标的偏差 | **核心** |
| OV5647 MIPI CSI | 上视 | P4 | 吸嘴取料后检查元件姿态和角度偏移 | 锦上添花 |

---

## 3. 软件架构

### 3.1 模块总览

基于 **ESP-IDF v5.4 + FreeRTOS**，运行在 ESP32-P4 上。整体采用轮询式状态机 + CLI 任务双线架构：

```
app_main()
  ├─ calib_init()          # NVS 初始化
  ├─ camera_init()         # OV5647 MIPI CSI 初始化
  ├─ wifi_init_sta()       # WiFi 连接
  ├─ state_machine_init()  # 加载标定 + 坐标文件
  ├─ xTaskCreate(cli_task) # 串口命令行（高优先级）
  └─ while(1)              # 状态机轮询（低优先级）
       └─ state_machine_run()
```

### 3.2 CLI 命令

| 命令 | 功能 | 对应状态 |
|------|------|---------|
| `test` | 拍照 → 豆包 → 打印识别结果 | 直接调用，不走状态机 |
| `home` | 全轴归零 | `HOMING` |
| `calibrate` | 标定 Mark 点 | `CALIBRATE` |
| `run` | 自动运行贴装流程 | `PICK` → 循环 |
| `status` | 查看当前状态和进度 | 只读 |
| `calib` | 查看标定参数 | 只读 |
| `stop` | 紧急停止 | `ERROR` |
| `help` | 帮助信息 | — |

### 3.3 核心模块

| 模块 | 文件 | 行数 | 功能 | 完成度 |
|------|------|------|------|--------|
| 主入口 | `main.c` | 206 | NVS/WiFi/Camera 初始化，CLI 任务，状态机轮询 | ✅ |
| 摄像头 | `camera.c/h` | 62+31 | OV5647 MIPI CSI 采集，1600×1200 JPEG | ⚠️ 桩实现 |
| 豆包 API | `doubao.c/h` | 147+31 | JPEG → Base64 → HTTP POST → 解析 JSON | ⚠️ 框架完成 |
| 视觉任务 | `vision.c/h` | 73+19 | Mark 点识别 + 吸嘴元件检查（基于豆包） | ⬜ 待 Prompt 调优 |
| 运动控制 | `motion.c/h` | 52+24 | UART 发送 G-code 给 S3 | ⚠️ 框架完成 |
| 标定 | `calib.c/h` | 75+32 | 标定参数 NVS 存取（像素↔毫米换算） | ✅ |
| 坐标加载 | `placements.c/h` | 59+35 | 解析嘉立创 EDA 导出的 CSV | ⚠️ 框架完成 |
| 状态机 | `state_machine.c/h` | 174+40 | 8 状态贴装流程编排 | ⚠️ 框架完成 |
| Base64 | `base64.c/h` | 33 | 轻量 Base64 编码（图片 → data URL） | ✅ |
| Kconfig | `Kconfig.projbuild` | 27 | WiFi/API Key 可配置项 | ✅ |

### 3.4 依赖关系

```
main.c
  ├── camera ──→ esp_camera (ESP-IDF 组件)
  ├── doubao ──→ base64 + esp_http_client + cJSON
  ├── vision ──→ camera + doubao
  ├── motion ──→ driver/uart
  ├── calib ───→ nvs_flash
  ├── placements ──→ stdio (fopen)
  └── state_machine ──→ calib + placements + vision + motion + camera
```

---

## 4. 贴装流程（状态机）

### 4.1 状态定义（8 个状态）

```c
typedef enum {
    STATE_IDLE,          // 空闲
    STATE_HOMING,        // 归零
    STATE_CALIBRATE,     // 标定
    STATE_PICK,          // 取料
    STATE_BOTTOM_ALIGN,  // 下视对位
    STATE_PLACE,         // 贴装
    STATE_DONE,          // 完成
    STATE_ERROR          // 错误
} pnp_state_t;
```

### 4.2 状态流转

```
IDLE ──→ HOMING ──→ IDLE
IDLE ──→ CALIBRATE ──→ IDLE
IDLE ──→ PICK ──→ BOTTOM_ALIGN ──→ PLACE ──→ PICK（循环）──→ DONE ──→ IDLE
任何状态 ──→ ERROR ──→ IDLE
```

### 4.3 各状态行为

| 状态 | 操作 | 关键技术 |
|------|------|---------|
| **HOMING** | `G28` 全轴归零 → `G90` 绝对坐标模式 | G-code |
| **CALIBRATE** | 拍照 → 豆包找 Mark 点 → 记录像素坐标 | Doubao Vision |
| **PICK** | 移到飞达位置 → `Z-5` 下降 → `PUMP ON` 吸取 → `Z20` 抬起 | G-code，飞达坐标表（TODO） |
| **BOTTOM_ALIGN** | 移到下视相机 → 检查吸嘴 → 找 Mark 点 → 计算偏移补偿 | Doubao Vision，像素→毫米换算 |
| **PLACE** | 移到目标坐标 → `Z-3` 下降 → `PUMP OFF` 释放 → `Z20` 抬起 | G-code，CSV 坐标 |
| **DONE** | 全部完成 → `Z50` 归位 → 计数器归零 | — |
| **ERROR** | `M112` 急停 → `PUMP OFF` | G-code |

### 4.4 LLM 角色限定

**豆包 Vision API 只做视觉识别**：
- 找 Mark 点位置
- 判断吸嘴是否有元件
- 识别角度和偏移

**不做**：
- 运动规划
- 路径计算
- 控制系统决策

---

## 5. 通信协议

### 5.1 P4 ↔ S3（UART）

```
P4 → S3:  "G0 X12.5 Y34.2 Z5.0\n"     # G-code 指令
S3 → P4:  "OK\n"                        # 确认
P4 → S3:  "CAPTURE\n"                   # 请求拍下视图
S3 → P4:  "IMG:1024,768\n"              # 图像尺寸
          [raw pixel data]
```

| 参数 | 值 |
|------|-----|
| UART 端口 | UART_NUM_1 |
| TX/RX 引脚 | GPIO17 / GPIO18 |
| 波特率 | 921600 |

### 5.2 P4 ↔ PC（HTTP）

```
P4 GET  http://pc_ip:8080/?action=snapshot
PC →   JPEG image stream
```

### 5.3 P4 ↔ 豆包（HTTPS）

```
POST https://ark.cn-beijing.volces.com/api/v3/chat/completions
Authorization: Bearer {DOUBAO_API_KEY}
Content-Type: application/json

{
  "model": "doubao-seed-1-6-vision-250815",
  "messages": [{
    "role": "user",
    "content": [
      {"type": "image_url", "image_url": {"url": "data:image/jpeg;base64,..."}},
      {"type": "text", "text": "识别图像中的PCB Mark点坐标"}
    ]
  }]
}
```

| 参数 | 值 |
|------|-----|
| 模型 | `doubao-seed-1-6-vision-250815` |
| 超时 | 15000 ms（推理 2~8 秒 + 余量） |
| 图片格式 | Base64 JPEG data URL |
| 图片大小 | 100~200KB（JPEG quality 72） |

---

## 6. 标定系统

### 6.1 标定参数（NVS 存储）

```c
typedef struct {
    float   pixel_to_mm_x;       // X 轴像素→毫米换算
    float   pixel_to_mm_y;       // Y 轴像素→毫米换算
    int     cam_width;           // 相机图像宽
    int     cam_height;          // 相机图像高
    float   cam_offset_x_mm;     // 相机中心 X 偏移
    float   cam_offset_y_mm;     // 相机中心 Y 偏移
    float   mark1_mm_x;          // 参考 Mark 点机械 X 坐标
    float   mark1_mm_y;          // 参考 Mark 点机械 Y 坐标
} calibration_t;
```

### 6.2 默认值

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `pixel_to_mm_x/y` | 0.005 mm/px | 标定后修正 |
| `cam_width/height` | 1600×1200 | UXGA 分辨率 |
| `cam_offset_x/y_mm` | 0.0 | 相机中心偏移 |
| `mark1_mm_x/y` | 0.0 | Mark 点机械坐标 |

### 6.3 偏移补偿公式

```
偏移(mm) = (Mark像素坐标 - 图像中心) × pixel_to_mm
补偿运动: G91 → G0 X{-dx} Y{-dy} → G90
```

---

## 7. 坐标文件格式

### 7.1 CSV 格式（嘉立创 EDA 导出）

```csv
Designator,Footprint,Mid X(mm),Mid Y(mm),Layer,Rotation,Feeder
C1,C0603,12.5,23.4,TopLayer,90,1
R2,R0805,45.0,10.2,TopLayer,0,2
U3,SOP-8,80.0,60.5,TopLayer,180,3
```

### 7.2 解析逻辑

- 跳过第一行表头
- 跳过空行
- 最多支持 200 个元件（`MAX_PLACEMENTS`）
- 飞达编号默认为 1（可选字段）

---

## 8. 开发状态总览

### 8.1 完成度矩阵

| 模块 | 状态 | 完成度 | 关键问题 |
|------|------|--------|---------|
| 项目框架 + CMake | ✅ 编译通过 | 100% | ESP32-P4 目标，IDF v5.4 |
| 标定 NVS 存取 | ✅ 完成 | 100% | 读写正常 |
| Base64 编码 | ✅ 完成 | 100% | 轻量实现 |
| 状态机框架 | ⚠️ 框架完成 | 60% | 流程编排逻辑待验证 |
| 豆包 API | ⚠️ 框架完成 | 50% | 需填 API Key，未实测 |
| 运动控制 | ⚠️ 框架完成 | 40% | G-code 格式待验证，飞达/相机位置硬编码 |
| CSV 解析 | ⚠️ 框架完成 | 40% | 嘉立创 EDA 格式适配待验证 |
| WiFi | ⚠️ 条件编译 | 10% | P4 无内置 WiFi，需 C6 协处理器 |
| 摄像头驱动 | ❌ 桩实现 | 5% | 需改 `esp_driver_cam` MIPI CSI |
| HTTP 拉图 | ⬜ 未开始 | 0% | 从 PC mjpg-streamer 拉帧 |
| 视觉分析 | ⬜ 未开始 | 0% | Prompt 工程 + 坐标解析 |
| 标定流程 | ⬜ 未开始 | 0% | LLM 驱动标定 |
| S3 执行层 | ⬜ 未开始 | 0% | 独立工程，G-code 转发 + DVP 采集 |
| MCP 集成 | 📄 设计阶段 | 10% | 设计文档已有 |
| 实物测试 | ⬜ 未开始 | 0% | 硬件已就位 |

### 8.2 待办优先级

**P0（阻塞实物测试）**：
- [ ] P4 MIPI CSI 摄像头驱动（`esp_driver_cam` 替代 `esp_camera` 桩）
- [ ] 豆包 API Key 配置 + 验证 API 连通性
- [ ] PC mjpg-streamer 推流 + P4 HTTP 拉图实现

**P1（核心流程）**：
- [ ] 视觉 Prompt 工程（Mark 点识别精度调优）
- [ ] UART 通信 P4 ↔ S3 实现与测试
- [ ] S3 DVP 摄像头 + G-code 转发固件

**P2（完整系统）**：
- [ ] 标定流程完整实现
- [ ] 状态机完整联调与异常处理
- [ ] 实物贴装测试与精度验证

**P3（优化增强）**：
- [ ] WiFi 迁移到 C6 协处理器（ESP-AT / ESP-Hosted）
- [ ] MCP 双向集成
- [ ] 错误恢复 / 抛料重试机制

---

## 9. 代码质量评估

### 9.1 优点

| 方面 | 评价 |
|------|------|
| **架构设计** | 模块分离清晰，状态机 + CLI 双线架构合理 |
| **代码可读性** | 简洁直白，注释充分，核心逻辑易理解 |
| **构建系统** | CMake + Kconfig 配置灵活，多目标支持 |
| **嵌入式适配** | 轮询式状态机适合 FreeRTOS，NVS 持久化标定 |
| **API 设计** | OpenAI-compatible 格式，扩展性好 |

### 9.2 问题与改进建议

| 问题 | 严重程度 | 建议 |
|------|---------|------|
| `main.h` 文件缺失（文档列出但不存在） | 低 | 创建或从文档中移除引用 |
| `motion_send` 在 `motion_init()` 未调用时 UART 写入失败 | 中 | 在 `motion_send` 中添加 UART 就绪检查 |
| `main.c` 中 `motion_init()` 被注释 | 中 | 添加条件编译或运行时检测 |
| 飞达位置/相机位置硬编码为占位值（`50.0f, 50.0f`） | 高 | 添加飞达坐标配置表和相机位置标定 |
| HTTP 拉图模块未实现（`http_client.c/h`） | 高 | 实现 HTTP 拉图模块 |
| 豆包 API Key 硬编码在头文件 | 中 | 迁移到 Kconfig 或 NVS 存储 |
| 缺少错误恢复和异常处理 | 中 | 增加超时重试、抛料重试逻辑 |
| 无单元测试 | 低 | 添加关键模块的单元测试 |
| 状态机 `status` 命令数据使用硬编码占位值（`666, 0`） | 低 | 暴露 `g_comp_idx` 和 `g_placement_count` |

---

## 10. 关键文件清单

```
liuPNP/
├── CMakeLists.txt                       # ESP-IDF 项目根 (5 行)
├── sdkconfig.defaults.esp32p4           # P4 默认 Kconfig
├── sdkconfig.defaults.esp32s3           # S3 默认 Kconfig
├── sdkconfig.defaults.esp32c3~esp32h2  # 其他芯片默认配置
├── README.md                            # 项目简介 (1 行)
├── dependencies.lock                    # 依赖锁定
├── pytest_blink.py                      # 测试脚本
├── main/
│   ├── CMakeLists.txt                   # 主组件依赖 (23 行)
│   ├── Kconfig.projbuild                # WiFi/API Key 配置项 (27 行)
│   ├── main.c                           # 入口 + CLI (206 行)
│   ├── camera.c/h                       # 摄像头驱动 (62+31 行)
│   ├── doubao.c/h                       # 豆包 API (147+31 行)
│   ├── vision.c/h                       # 视觉任务 (73+19 行)
│   ├── motion.c/h                       # 运动控制 (52+24 行)
│   ├── calib.c/h                        # 标定参数 (75+32 行)
│   ├── placements.c/h                   # CSV 解析 (59+35 行)
│   ├── state_machine.c/h                # 状态机 (174+40 行)
│   ├── base64.c/h                       # Base64 编码 (33 行)
├── components/
│   ├── cJSON/                           # JSON 解析库
│   ├── esp_camera/                      # 摄像头驱动（当前为桩）
│   ├── esp_jpeg.zip                     # JPEG 库
│   └── led_strip.zip                    # LED 灯带库
└── docs/
    ├── liuPNP-framework.md              # 框架设计文档 (240 行)
    └── liuPNP-analysis.md               # 本分析报告
```

---

## 11. 总结

liuPNP 是一个**理念新颖、架构清晰的 AI-Native 贴片机项目**。核心创新在于用多模态大模型替代传统的 OpenCV 图像处理管道，大幅降低了视觉系统的开发复杂度。

当前项目处于**早期原型阶段**：软件框架已完成并通过编译，但关键硬件驱动（MIPI CSI 摄像头）和网络通信（HTTP 拉图、API 实测）尚未打通。项目的最大技术风险在于：

1. **MIPI CSI 驱动适配**：ESP32-P4 的 `esp_driver_cam` 相对较新，文档和示例较少
2. **豆包 API 延迟**：15 秒超时对于实时贴装可能偏慢，需要评估实际响应时间
3. **视觉 Prompt 精度**：Mark 点识别达到亚毫米精度需要持续调优 Prompt 工程
4. **WiFi 方案**：P4 无内置 WiFi，需 C6 协处理器方案，增加了系统复杂度

建议优先完成 P0 阻塞项，打通硬件链路后再迭代视觉算法和流程优化。