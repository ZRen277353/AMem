# AMem MCP Server

AMem MCP Server 是一个基于 [Model Context Protocol](https://modelcontextprotocol.io/) 的服务，通过 HTTP 代理模式桥接 AMem GUI 内嵌的 IPC Server，将选定的内存调试能力暴露为 MCP 工具，供 AI 助手（Claude Code / Claude Desktop / Codex CLI / Cursor / VS Code Copilot / Continue 等）调用。

## 架构

```
AI 助手  ←── stdio ──→  amem-mcp (Python)  ←── HTTP JSON ──→  AMem GUI (C++ IPC :28100)
                                                                       ↕
                                                                 Android 设备
```

MCP Server 本身不直接与 Android 设备通信，所有操作都委托给 AMem GUI 的 IPC Server（默认监听 `127.0.0.1:28100`）。

当前静态能力面不是一一对应：内置 AI Agent 有 32 个广告定义/39 个可执行名称（7 个隐藏 alias），IPC 有 29 个方法，MCP 有 30 个工具。MCP 的 typed read/write 是 Python wrapper；IPC `read_batch` 尚未暴露为 MCP 工具，内置 Agent 的 `read_disassembly`/`resolve_symbol` 也没有同名 IPC 方法。构建时没有 LuaJIT，MCP 仍会显示 `execute_lua`，但 GUI IPC 不会注册该方法。

> 安全提示：IPC 当前仅绑定回环地址，但没有认证 token，并允许浏览器 CORS；MCP 写操作也不经过内置 AI Chat 的审批框。只在可信本机环境运行，不要把 28100 端口代理或转发到外部网络，也不要让不可信网页/本地进程访问正在运行的 GUI。

## 环境要求

- Python 3.10+
- AMem GUI 已启动（IPC Server 随 GUI 自动启动）

## 安装

两种方式任选其一。

### A. 可编辑安装（推荐）

```bash
cd mcp
pip install -e .
```

安装后会注册 `amem-mcp` 命令，无需再写绝对路径。

### B. 仅安装依赖

```bash
cd mcp
pip install -r requirements.txt
```

用 `python -m amem_mcp` 或 `python server.py` 启动。

## 启动 / 命令行

```bash
amem-mcp                                       # pip install 后
python -m amem_mcp                             # 模块方式
python server.py                               # 兼容入口（任意 cwd）

amem-mcp --ipc-host 127.0.0.1 --ipc-port 28100 # 指定 IPC 地址
```

也支持环境变量 `AMEM_IPC_HOST` / `AMEM_IPC_PORT`（对 IDE 配置很有用）。

---

## IDE 接入

每个 IDE 需要的配置格式不同。`configs/` 目录下提供了全部样例，复制即可用。

> 以下示例假设用的是**可编辑安装**，推荐把 `command: "python", args: ["-m", "amem_mcp"]` 改为 `command: "amem-mcp"` 并删除 `args`、`env.PYTHONPATH`。

### Claude Code

项目根目录创建 `.mcp.json`（或合并到 `~/.claude.json`）：

```json
{
  "mcpServers": {
    "amem": {
      "command": "amem-mcp",
      "env": { "AMEM_IPC_PORT": "28100" }
    }
  }
}
```

如果没做 pip install：

```json
{
  "mcpServers": {
    "amem": {
      "command": "python",
      "args": ["-m", "amem_mcp"],
      "env": { "PYTHONPATH": "D:/AndroidMEM/AMem/mcp" }
    }
  }
}
```

模板：[`configs/claude-code.json`](./configs/claude-code.json)

### Claude Desktop

合并到 `%APPDATA%/Claude/claude_desktop_config.json`（macOS 是 `~/Library/Application Support/Claude/claude_desktop_config.json`）：

```json
{
  "mcpServers": {
    "amem": { "command": "amem-mcp" }
  }
}
```

模板：[`configs/claude-desktop.json`](./configs/claude-desktop.json)

### Codex CLI

合并到 `~/.codex/config.toml`（Codex 只支持用户级配置，不支持项目级）：

```toml
[mcp_servers.amem]
command = "amem-mcp"

[mcp_servers.amem.env]
AMEM_IPC_PORT = "28100"
```

注意 Codex 的 key 是 `mcp_servers`（下划线），不是 JSON 系列的 `mcpServers`。

模板：[`configs/codex.toml`](./configs/codex.toml)

### Cursor

放置于 `~/.cursor/mcp.json` 或项目内 `.cursor/mcp.json`：

```json
{
  "mcpServers": {
    "amem": { "command": "amem-mcp" }
  }
}
```

模板：[`configs/cursor.json`](./configs/cursor.json)

### VS Code (GitHub Copilot)

VS Code 的 MCP 配置 key 是 `servers` 而不是 `mcpServers`。放置于项目 `.vscode/mcp.json`：

```json
{
  "servers": {
    "amem": {
      "type": "stdio",
      "command": "amem-mcp"
    }
  }
}
```

模板：[`configs/vscode.json`](./configs/vscode.json)

### Continue (VS Code / JetBrains 插件)

合并到 `~/.continue/config.json` 的 `experimental.modelContextProtocolServers` 数组。模板：[`configs/continue.json`](./configs/continue.json)

---

## 通信协议

MCP Server 通过 HTTP POST 向 IPC Server 发送 JSON 请求：

```json
// 请求
{ "method": "read_memory", "params": { "address": "0x7f12345000", "size": 256 } }

// 响应
{ "success": true, "result": { "hex": "48656c6c6f...", "size": 256 } }
```

默认 30 秒超时，扫描类操作 60 秒。仅支持本地回环地址。

所有地址字符串都应显式使用 `0x` 前缀。当前 IPC/MCP 会把无前缀字符串按十进制解析，而内置 Agent 按十六进制解析。

部分只读方法遇到 timeout/网络错误会默认重试两次。Python timeout 不会取消 GUI 中已经开始的 C++ handler，因此旧、新请求可能重叠；不要把客户端 timeout 理解为设备操作已停止。

---

## 工具列表

### 状态与连接

| 工具 | 说明 |
|------|------|
| `get_status()` | 获取 GUI 当前状态（连接状态、PID、进程名） |
| `get_server_version()` | 获取 Android 服务端版本信息 |
| `get_architecture()` | 获取目标设备内存架构类型 |
| `init_driver(card_name)` | 初始化内核读写驱动，需传入授权卡密 |

### 进程与模块

| 工具 | 说明 |
|------|------|
| `list_processes()` | 列出 Android 设备上所有运行中的进程 |
| `open_process(pid)` | 打开指定 PID 的进程，后续操作针对此进程 |
| `list_modules(filter, offset, count)` | 列出当前进程加载的模块 |
| `get_module_base(module_name)` | 获取指定模块的基址 |
| `resolve_offset_chain(module, base_offset, offsets, deref_final)` | 解析指针链 |

### 内存读写

| 工具 | 参数 | 说明 |
|------|------|------|
| `read_memory(address, size=256)` | size 最大 65536 | 读取内存并返回 hex dump |
| `read_value(address, data_type="dword")` | byte/word/dword/qword/float/double/xor | 读取单个值 |
| `write_value(address, value, data_type="dword")` | byte/word/dword/qword/float/double/xor | 写入单个值 |
| `write_bytes(address, hex_string)` | 如 `"90 90 90"` | 写入原始字节 |

### 内存扫描

| 工具 | 参数 | 说明 |
|------|------|------|
| `scan_set_range(memory_type="all")` | 见下表 | 设置扫描的内存区域 |
| `scan_value(value, data_type, scan_type="exact", value2="")` | `exact/greater/less/between` | 首次扫描；between 需要 value2，未知初值请用 `scan_fuzzy(..., "unknown")` |
| `scan_next(value, data_type, scan_type="exact", value2="")` | `exact/increased/decreased/changed/unchanged/increased_by/decreased_by/greater/less/between` | 再次扫描；between 需要 value2 |
| `scan_fuzzy(data_type, scan_type="unknown")` | `unknown/increased/decreased/changed/unchanged` | 模糊扫描 |
| `scan_hex(hex_pattern)` | 如 `"48 65 6C 6C 6F"` | 十六进制模式扫描 |
| `get_scan_count()` | — | 获取当前扫描结果总数 |
| `get_scan_results(offset=0, count=20)` | count 最大 1000 | 分页获取扫描结果 |
| `clear_scan()` | — | 清除所有扫描结果 |

**内存类型** (`scan_set_range` 参数)：`all` / `anonymous` / `c_alloc` / `c_heap` / `c_data` / `c_bss` / `java_heap` / `java` / `stack` / `code_app` / `code_system` / `video` / `ashmem` / `bad` / `other`

### 硬件断点

断点类型编号与 AMem 内部（GUI / Lua / IPC）完全一致，MCP 层不做任何翻译：

| `bp_type` | 语义 | 字符串别名 |
|-----------|------|-----------|
| `1` | 读 | `"read"` |
| `2` | 写 | `"write"` |
| `3` | 读写 | `"readwrite"` / `"access"` |
| `4` | 执行 | `"execute"` |

| 工具 | 参数 | 说明 |
|------|------|------|
| `set_breakpoint(address, bp_type=2, bp_size=4)` | `bp_type`: 1~4 整数或字符串别名; `bp_size`: 1/2/4/8（执行断点强制 4） | 设置硬件断点 |
| `remove_breakpoint(address)` | — | 移除断点 |
| `read_breakpoint_info(address)` | — | 读取断点命中信息（含 ARM64 寄存器状态） |
| `suspend_breakpoint(address)` | — | 暂停断点（不删除） |
| `resume_breakpoint(address)` | — | 恢复已暂停的断点 |

### Lua 脚本

| 工具 | 说明 |
|------|------|
| `execute_lua(code, timeout_seconds=30)` | 在 GUI 内执行 Lua，可使用 mem/process/scan/bp 等全部 API；timeout 最大 300 秒 |

### 符号

| 工具 | 说明 |
|------|------|
| `symbol_init(module_base)` | 初始化指定模块的符号表 |
| `symbol_list(offset, count, module_base="")` | 分页列出符号 |
| `symbol_find(module_base, symbol_name)` | 按名称查找符号地址 |

---

## MCP Resources

| URI | 说明 |
|-----|------|
| `amem://status` | 当前 AMem GUI 状态（连接、PID、进程名） |

---

## 典型使用流程

```
1. get_status()                          # 确认 GUI 已连接设备
2. list_processes()                      # 查看进程列表
3. open_process(pid=12345)               # 打开目标进程
4. list_modules()                        # 查看模块列表
5. scan_set_range("all")                 # 明确扫描区域
6. scan_value("100", "dword", "exact")   # 首次扫描值 100
7. scan_next("95", "dword", "exact")     # 值变化后再次扫描
8. get_scan_results()                    # 查看结果
9. write_value("0x7f1234", "999")        # 修改内存值
10. set_breakpoint("0x7f1234", 2, 4)     # 设置写入断点 (2=写)
11. read_breakpoint_info("0x7f1234")     # 查看谁修改了这个地址
```

---

## 目录结构

```
mcp/
├── amem_mcp/                # 主包
│   ├── __init__.py
│   ├── __main__.py          # python -m amem_mcp 入口
│   ├── app.py               # FastMCP 装配 + main()
│   ├── constants.py         # 扫描 flag / 数据类型 / 内存类型
│   ├── helpers.py           # hex_dump / encode_value / make_scan_flags
│   ├── ipc_client.py        # HTTP JSON 客户端
│   └── tools/               # 工具按域拆分
│       ├── status.py        # 状态、版本、架构、驱动
│       ├── process.py       # 进程、模块、指针链
│       ├── memory.py        # 内存读写
│       ├── scan.py          # 内存扫描
│       ├── breakpoint_.py   # 硬件断点
│       ├── lua.py           # Lua 执行
│       └── symbols.py       # 符号表
├── configs/                 # 各 IDE 配置样例
│   ├── claude-code.json
│   ├── claude-desktop.json
│   ├── codex.toml
│   ├── continue.json
│   ├── cursor.json
│   └── vscode.json
├── reference/               # 二进制协议参考实现（MCP 不用）
│   ├── README.md
│   └── amem_client.py
├── pyproject.toml           # pip 安装入口，注册 amem-mcp 命令
├── requirements.txt
├── server.py                # 兼容入口
└── README.md
```

## 开发

添加新工具：在 `amem_mcp/tools/` 下新建或编辑模块，实现 `register(mcp, ipc)` 函数，并在 `tools/__init__.py` 的 `register_all` 里注册。所有扫描/类型常量都在 `amem_mcp/constants.py`，复用已有 helper 可避免重复的编码逻辑。

同时更新或校验：

1. `ipc/IpcServer.cpp` 中的方法、参数和 feature gate。
2. 内置 `gui/ai/ToolDefinitions.cpp` 是否也需要该能力。
3. 地址、scan flag、错误结构、分页和输出上限是否一致。
4. timeout 后旧 handler 继续运行时，重试是否仍安全。
5. 文档中的 MCP 30 / IPC 29 / Agent 32 advertised（39 executable）能力矩阵；后续应改为自动生成/测试。
