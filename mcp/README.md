# AMem MCP Server

AMem MCP Server 是一个基于 [Model Context Protocol](https://modelcontextprotocol.io/) 的服务，通过 HTTP 代理模式桥接 AMem GUI 内嵌的 IPC Server，将 GUI 的全部 C++ 能力暴露为 MCP 工具，供 AI 助手（Claude Code / Claude Desktop / Codex CLI / Cursor / VS Code Copilot / Continue 等）直接调用。

## 架构

```
AI 助手  ←── stdio ──→  amem-mcp (Python)  ←── HTTP JSON ──→  AMem GUI (C++ IPC :28100)
                                                                       ↕
                                                                 Android 设备
```

MCP Server 本身不直接与 Android 设备通信，所有操作都委托给 AMem GUI 的 IPC Server（默认监听 `127.0.0.1:28100`）。

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

默认 30 秒超时，扫描类操作 60 秒。仅支持本地回环地址。GUI 返回 HTTP 400/500 时，如果响应体是 IPC JSON 错误，MCP 会保留并返回原始错误信息，便于定位参数错误或 GUI 侧异常。

### 输入校验

MCP 层会在请求进入 GUI IPC 前做第一道校验，避免明显非法参数传到 C++ 层：

- 地址、模块基址、指针偏移必须是非负整数，支持十进制和 `0x` 十六进制字符串。
- `size` / `count` 必须大于 0；`read_memory` 最大 65536 字节，分页类 `count` 最大 1000。
- `hex_string` / `hex_pattern` 会去掉空白字符，但必须非空、长度为偶数且是有效十六进制。
- `data_type` 必须是 `byte` / `word` / `dword` / `qword` / `float` / `double` / `xor`。
- `scan_type` 和 `memory_type` 必须是下文列出的合法值；拼写错误会直接报错，不会静默回退到默认值。
- `init_driver(card_name)` 和 `execute_lua(code)` 不接受空字符串。

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
| `write_value(address, value, data_type="dword")` | 整数支持 `0x` 前缀 | 写入单个值 |
| `write_bytes(address, hex_string)` | 如 `"90 90 90"`；必须是有效偶数长度 hex | 写入原始字节 |

### 内存扫描

| 工具 | 参数 | 说明 |
|------|------|------|
| `scan_set_range(memory_type="all")` | 见下表 | 设置扫描的内存区域 |
| `scan_value(value, data_type, scan_type="exact")` | `exact/unknown/greater/less/between` | 首次扫描 |
| `scan_next(value, data_type, scan_type="exact")` | `exact/increased/decreased/changed/unchanged/increased_by/decreased_by/greater/less` | 再次扫描 |
| `scan_fuzzy(data_type, scan_type="unknown")` | `unknown/increased/decreased/changed/unchanged` | 模糊扫描 |
| `scan_hex(hex_pattern)` | 如 `"48 65 6C 6C 6F"` | 十六进制模式扫描 |
| `get_scan_count()` | — | 获取当前扫描结果总数 |
| `get_scan_results(offset=0, count=20)` | count 最大 1000 | 分页获取扫描结果 |
| `clear_scan()` | — | 清除所有扫描结果 |

**内存类型** (`scan_set_range` 参数)：`all` / `anonymous` / `c_alloc` / `c_heap` / `c_data` / `c_bss` / `java_heap` / `java` / `stack` / `code_app` / `code_system` / `video` / `ashmem` / `bad`

**数据类型** (`data_type` 参数)：`byte` / `word` / `dword` / `qword` / `float` / `double` / `xor`

**扫描类型** (`scan_type` 参数)：`exact` / `unknown` / `greater` / `less` / `between` / `increased` / `increased_by` / `decreased` / `decreased_by` / `changed` / `unchanged`

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
| `execute_lua(code)` | 在 GUI 内执行 Lua，可使用 mem/process/scan/bp 等全部 API |

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
5. scan_value("100", "dword", "exact")   # 首次扫描值 100
6. scan_next("95", "dword", "exact")     # 值变化后再次扫描
7. get_scan_results()                    # 查看结果
8. write_value("0x7f1234", "999")        # 修改内存值
9. set_breakpoint("0x7f1234", 2, 4)      # 设置写入断点 (2=写)
10. read_breakpoint_info("0x7f1234")     # 查看谁修改了这个地址
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

### 快速自检

在仓库根目录运行：

```bash
python -m compileall -q mcp
python -c "import sys; sys.path.insert(0, 'mcp'); from amem_mcp.app import build_server; mcp, ipc = build_server(); print(type(mcp).__name__, ipc.base_url)"
python -c "import asyncio, sys; sys.path.insert(0, 'mcp'); from amem_mcp.app import build_server; mcp, _ = build_server(); print(len(asyncio.run(mcp.list_tools())))"
```

期望工具数量为 30。配置文件可用以下方式检查：

```bash
python -m json.tool .mcp.json
python -m json.tool mcp/configs/claude-code.json
python -c "import tomllib, pathlib; tomllib.loads(pathlib.Path('mcp/pyproject.toml').read_text(encoding='utf-8')); tomllib.loads(pathlib.Path('mcp/configs/codex.toml').read_text(encoding='utf-8'))"
```
