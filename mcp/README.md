# AMem MCP 服务文档

AMem MCP Server 是一个基于 [Model Context Protocol](https://modelcontextprotocol.io/) 的服务，通过 HTTP 代理模式桥接 AMem GUI 内嵌的 IPC Server，将 GUI 的全部 C++ 能力暴露为 MCP 工具，供 AI 助手（如 Claude）直接调用。

## 架构

```
AI 助手 (Claude)  ←── stdio ──→  MCP Server (Python)  ←── HTTP JSON ──→  AMem GUI (C++ IPC Server :28100)
                                                                              ↕
                                                                        Android 设备 (Socket)
```

MCP Server 本身不直接与 Android 设备通信，所有操作都委托给 AMem GUI 的 IPC Server（默认监听 `127.0.0.1:28100`）。

## 环境要求

- Python 3.10+
- AMem GUI 已启动（IPC Server 自动随 GUI 启动）

## 安装

```bash
cd mcp
pip install -r requirements.txt
```

依赖仅 `mcp>=1.0.0`（MCP Python SDK）。

## 启动

```bash
# 默认 stdio 模式，连接 localhost:28100
python server.py

# 指定 IPC 地址和端口
python server.py --ipc-host 127.0.0.1 --ipc-port 28100
```

## 配置 MCP 客户端

在 Claude Code 的 MCP 配置中添加：

```json
{
  "mcpServers": {
    "amem": {
      "command": "python",
      "args": ["server.py"],
      "cwd": "<项目路径>/mcp"
    }
  }
}
```

## 通信协议

MCP Server 通过 HTTP POST 向 IPC Server 发送 JSON 请求：

```json
// 请求
{
  "method": "read_memory",
  "params": { "address": "0x7f12345000", "size": 256 }
}

// 响应
{
  "success": true,
  "result": { "hex": "48656c6c6f...", "size": 256 }
}
```

超时时间 30 秒，仅支持本地回环地址。

---

## 工具列表

### 状态与连接

| 工具 | 说明 |
|------|------|
| `get_status()` | 获取 GUI 当前状态（连接状态、PID、进程名） |
| `get_server_version()` | 获取 Android 服务端版本信息 |
| `get_architecture()` | 获取目标设备内存架构类型 |
| `init_driver(card_name)` | 初始化内核读写驱动，需传入授权卡密 |

### 进程管理

| 工具 | 说明 |
|------|------|
| `list_processes()` | 列出 Android 设备上所有运行中的进程 |
| `open_process(pid)` | 打开指定 PID 的进程，后续操作针对此进程 |
| `list_modules()` | 列出当前进程加载的所有模块（基址、大小、名称） |

### 内存读写

| 工具 | 参数 | 说明 |
|------|------|------|
| `read_memory(address, size=256)` | address: 地址(支持0x前缀), size: 字节数(最大65536) | 读取内存，返回 hex dump 格式 |
| `read_value(address, data_type="dword")` | data_type: byte/word/dword/qword/float/double | 读取单个值 |
| `write_value(address, value, data_type="dword")` | value: 要写入的值 | 写入单个值 |
| `write_bytes(address, hex_string)` | hex_string: 如 "90 90 90" | 写入原始字节 |

### 内存扫描

| 工具 | 参数 | 说明 |
|------|------|------|
| `scan_set_range(memory_type="all")` | 见下方内存类型表 | 设置扫描的内存区域 |
| `scan_value(value, data_type, scan_type="exact")` | scan_type: exact/unknown/greater/less/between | 首次扫描 |
| `scan_next(value, data_type, scan_type="exact")` | scan_type: exact/increased/decreased/changed/unchanged/increased_by/decreased_by/greater/less | 再次扫描（在上次结果中筛选） |
| `scan_fuzzy(data_type, scan_type="unknown")` | scan_type: unknown/increased/decreased/changed/unchanged | 模糊扫描（未知初始值） |
| `scan_hex(hex_pattern)` | hex_pattern: 如 "48 65 6C 6C 6F" | 十六进制模式扫描 |
| `get_scan_count()` | — | 获取当前扫描结果总数 |
| `get_scan_results(offset=0, count=20)` | count 最大 1000 | 分页获取扫描结果 |
| `clear_scan()` | — | 清除所有扫描结果 |

**内存类型 (`scan_set_range` 参数)**

| 值 | 说明 |
|----|------|
| `all` | 全部内存（默认） |
| `anonymous` | 匿名映射 |
| `c_alloc` | C malloc 分配 |
| `c_heap` | C 堆 |
| `c_data` | C 数据段 |
| `c_bss` | C BSS 段 |
| `java_heap` | Java 堆 |
| `java` | Java 内存 |
| `stack` | 栈 |
| `code_app` | 应用代码段 |
| `code_system` | 系统代码段 |

### 硬件断点

| 工具 | 参数 | 说明 |
|------|------|------|
| `set_breakpoint(address, bp_type=1, bp_size=4)` | bp_type: 1=执行, 2=写入, 3=读写; bp_size: 1/2/4/8 | 设置硬件断点 |
| `remove_breakpoint(address)` | — | 移除断点 |
| `read_breakpoint_info(address)` | — | 读取断点命中信息（含 ARM64 寄存器状态） |
| `suspend_breakpoint(address)` | — | 暂停断点（不删除） |
| `resume_breakpoint(address)` | — | 恢复已暂停的断点 |

### Lua 脚本

| 工具 | 参数 | 说明 |
|------|------|------|
| `execute_lua(code)` | code: Lua 脚本代码 | 在 GUI 内执行 Lua 脚本，可使用 mem/process/scan/bp 等全部 API |

### 模块辅助

| 工具 | 参数 | 说明 |
|------|------|------|
| `get_module_base(module_name)` | 模块名称 | 获取指定模块的基址 |
| `resolve_offset_chain(module, base_offset, offsets, deref_final=True)` | offsets: 偏移链列表如 [0x10, 0x20] | 解析指针链，获取最终地址 |

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
9. set_breakpoint("0x7f1234", 2, 4)      # 设置写入断点
10. read_breakpoint_info("0x7f1234")     # 查看谁修改了这个地址
```

## 文件结构

```
mcp/
├── server.py          # MCP Server 主程序，定义所有工具
├── ipc_client.py      # HTTP IPC 客户端，与 GUI 通信
├── amem_client.py     # 二进制协议客户端（参考实现，MCP 不使用）
└── requirements.txt   # Python 依赖
```
