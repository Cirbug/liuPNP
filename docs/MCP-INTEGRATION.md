# MCP 双向集成方案

> 目标：打通 OpenClaw 云端 AI 与 Cline VSCode 本地环境，实现双向工具调用。

---

## 架构总览

```
┌─────────────────────────────────┐      ┌─────────────────────────────┐
│  云服务器 101.43.33.186           │      │  Windows 本机 (小何清)         │
│                                  │      │                              │
│  OpenClaw (AI Agent)             │      │  VSCode + Cline 插件          │
│  ├─ 内部工具:                    │      │  ├─ 编辑器 / 终端 / 文件       │
│  │   wecom_mcp (企微通讯录)       │      │  ├─ ESP-IDF 工具链             │
│  │   wecom_mcp (日程/待办)        │◄────►│  └─ MCP Server (内置)          │
│  │   qqbot_channel_api (QQ频道)  │ MCP  │                              │
│  │   ─────────────────           │ 协议  │  ┌─────────┐                 │
│  │   [MCP 桥接服务 :9400]         │      │  │  liuPNP  │   D:\git\liuPNP│
│  │   ─── 方向 A: 我 ← Cline       │      │  │  工程    │                 │
│  │   ─── 方向 B: Cline → 我       │      │  └─────────┘                 │
│  └───────────────────────────────┘      └─────────────────────────────┘
```

**方向 A — Cline 调我**：Cline 连入云端 MCP Server，获得企微/QQ/日程等工具能力  
**方向 B — 我调 Cline**：AI 通过 MCP 操作本地 VSCode（文件、终端、编译）

---

## 方向 B — AI 调 Cline（先做，5 分钟）

### Cline 端配置

1. 打开 VSCode → `Ctrl+,` → 搜索 `cline.mcp`
2. 启用 `cline.mcp.server.enabled`
3. 确认端口（默认 `127.0.0.1:XXXXX`，需记下）
4. 如果是 `127.0.0.1` 只监听本地，需要改为 `0.0.0.0` 或用 frp/隧道暴露出去

### 穿透方案（服务器能连到你本机）

Cline MCP Server 默认只监听 localhost，需要隧道：

```
方案1: SSH 反向隧道
  Windows: ssh -R 9401:127.0.0.1:XXXXX ubuntu@101.43.33.186
  效果: 服务器 127.0.0.1:9401 → Windows 本地 Cline MCP

方案2: frp (已有内网穿透节点)
  在 frpc.ini 添加 MCP 端口转发
```

### 调用流程

```
AI (服务器)                    Cline (Windows)
    │                              │
    ├──── MCP Request ───────────► │
    │     tool: "terminal_exec"    │──► 执行 idf.py build
    │     args: {cmd: "..."}       │
    │                              │
    │◄─── MCP Response ──────────  │
    │     result: "编译成功"        │
```

### Cline 暴露的工具（预计）

| 工具名 | 功能 | 用途 |
|--------|------|------|
| `terminal_exec` | 执行终端命令 | 编译、烧录、git 操作 |
| `file_read` | 读取文件 | AI 读取你的本地代码 |
| `file_write` | 写入文件 | AI 直接改代码 |
| `file_search` | 搜索文件/内容 | 代码定位 |

---

## 方向 A — Cline 调 AI（后续，半天开发）

### MCP 桥接服务

在服务器上新增一个 Python/Node 服务，将 OpenClaw 内部工具暴露为标准 MCP 端点。

```
~/.openclaw/mcp-bridge/
├── server.py          # MCP 协议服务器（stdio / SSE）
├── tools/
│   ├── wecom.py       # 企微通讯录/日程/待办
│   └── qqbot.py       # QQ 频道管理
└── config.yaml        # 授权配置
```

### 暴露的工具

| MCP Tool | 功能 | 对应 OpenClaw 工具 |
|----------|------|-------------------|
| `wecom_list_contacts` | 查通讯录 | `wecom_mcp call contact getContact` |
| `wecom_list_schedules` | 查日程 | `wecom_mcp call schedule` |
| `wecom_create_todo` | 创建待办 | `wecom_mcp call todo` |
| `wecom_send_message` | 发企微消息 | `wecom_mcp call msg` |
| `qq_list_guilds` | 查 QQ 频道 | `qqbot_channel_api` |

### Cline 端配置（mcpServers JSON）

```json
{
  "mcpServers": {
    "openclaw-tools": {
      "type": "sse",
      "url": "http://101.43.33.186:9400/sse"
    }
  }
}
```

### 调用流程

```
Cline (Windows)                 MCP Bridge (服务器)          OpenClaw
    │                                │                         │
    ├── MCP Request ───────────────► │                         │
    │   tool: "wecom_list_contacts"  │── 内部 API ──────────►  │
    │                                │                         │
    │                                │◄─ 结果 ─────────────── │
    │◄── MCP Response ────────────  │                         │
    │   [{name:"张三", ...}, ...]    │
```

---

## 实施计划

| 步骤 | 内容 | 耗时 | 状态 |
|------|------|------|------|
| B.1 | 你启用 Cline MCP Server | 5min | ⬜ 等待你操作 |
| B.2 | 建立隧道（SSH -R 或 frp） | 10min | ⬜ 等待 |
| B.3 | AI 验证连通 + 首次调终端 | 5min | ⬜ |
| A.1 | AI 编写 MCP 桥接服务 | 2h | ⬜ |
| A.2 | 你填入 Cline mcpServers 配置 | 2min | ⬜ |
| A.3 | 联调验证 | 30min | ⬜ |

---

## 安全考虑

- MCP 桥接服务仅监听 `127.0.0.1`，通过 SSH 隧道供 Cline 访问
- 不暴露到公网，依赖 SSH 认证保护
- 企微等操作保持原有权限边界，不做越权
