# AMem AI Agent 架构文档

适用分支：`NativeAgent`（基线来自 `AIChat`）
最后更新：2026-07-10

本文描述当前工作区中的内置 AI Chat、IPC/MCP 桥接和它们共享的设备协议层。代码走读见 [`agent_walkthrough.md`](./agent_walkthrough.md)，已确认风险和修复优先级见 [`agent_project_issues.md`](./agent_project_issues.md)，NativeAgent 的目标设计和迁移顺序见 [`native_agent_refactor_plan.md`](./native_agent_refactor_plan.md)。

> 本文中的“内置 Agent”指 `gui/ai/` 中由 `ChatWindow` 驱动的 model -> tool -> model 循环。“外部 Agent”指通过 Python MCP server 和本地 IPC 调用 AMem 的 Claude Code、Codex、Cursor 等客户端。两者不是同一套编排器，但最终调用同一设备命令层。

## 1. 系统总览

AMem 有三个设备能力入口：

```text
GUI windows --------------------+
                                |
In-app AI Agent                 +--> socket/client_singleton.h
ChatWindow -> ToolDefinitions --+       -> WinSocketClientMgr (3 ports)
                                |       -> Android server/device
External AI Agent               |
MCP (Python) -> IPC :28100 -----+
```

`socket/client_singleton.h` 及 `socket/*Commands.cpp` 是设备协议的主要真相源。GUI、内置 Agent 和 IPC handler 都不应各自重写协议。

内置 Agent 完成四件事：

1. 把用户消息、清洗后的历史、provider 配置和工具定义组装为模型请求。
2. 在后台执行 HTTPS/SSE，把 token、完成、错误投递回 ImGui 主线程。
3. 模型返回 tool call 时，执行预算控制、写类审批、工具调用和结果回喂。
4. 保存会话并记录 run trace。

外部 MCP 路径不经过 `AgentController`、`AgentRunner` 或内置审批框。Python MCP tool 直接调用 IPC，IPC handler 再调用设备命令。外部客户端是否审批写操作由客户端自身策略决定。

## 2. 目录与职责

| 层 | 主要文件/类 | 职责 |
|----|-------------|------|
| UI | `ChatWindow`, `ChatWindowSettings.cpp` | 对话、会话切换、设置、写工具审批、每帧消费 `UIMessageQueue` |
| 编排 | `AgentController` | provider 查找、请求构造、run id/state/trace |
| 工具循环 | `AgentRunner` | step/call 预算、审批状态机、失败后的跳过、工具审计消息 |
| Provider | `AIProvider`, `ClaudeProvider`, `OpenAIProvider`, `DeepSeekProvider` | provider 请求/响应适配和流式 tool call 拼装 |
| HTTP | `HttpClient` | cpp-httplib + OpenSSL、代理、SSE、协作式取消 |
| 工具注册 | `ToolExecutor`, `ToolDefinitions.cpp` | schema 校验、安全分类、执行、超时、广告/隐藏兼容名称 |
| 原生内存服务 | `mem/`, `AgentMemTools` | 强类型结果、地址/分页校验、target/generation 校验和 Agent JSON adapter |
| 会话 | `ChatSession`, `SessionManager` | 历史清洗、token/条数裁剪、会话文件和索引 |
| 配置 | `ApiKeyStore`, `AiSettings`, `DefaultSystemPrompt.h` | provider 配置、DPAPI key、全局设置、默认 prompt |
| UI 桥 | `UIMessageQueue` | 内置 Agent 后台线程向 ImGui 主线程投递消息 |
| 全局目标 | `AppContext` | PID、process handle、`processRevision`、模块/符号缓存 |
| IPC | `IpcServer` | 回环 HTTP JSON 入口，方法注册和 C++ handler |
| MCP | `mcp/amem_mcp/` | FastMCP stdio server、参数转换、HTTP IPC client |
| 协议 | `client_singleton.h`, `*Commands.cpp`, `SocketCommand.h` | Android 请求/响应、端口锁、超时和结果校验 |

## 3. 内置 Agent 的核心对象

### 3.1 消息和工具

核心结构定义在 `AIProvider.h`：

```cpp
enum class Role { System, User, Assistant, Tool };

struct ToolCall {
    std::string id;
    std::string name;
    std::string arguments; // JSON text
};

struct ChatMessage {
    Role role;
    std::string content;
    std::vector<ToolCall> toolCalls;
    std::string toolCallId;
    std::string name;
    long long timestamp;
    long long durationMs;
};

enum class ToolSafety { ReadOnly, Write };
```

`ToolDefinition::parametersSchema` 是 JSON Schema 字符串。`ToolExecutor` 实现一个受限 schema 子集，执行前会解析和校验模型参数。

### 3.2 请求、响应和错误

`CompletionRequest` 携带 `runId`、消息、工具、model 和回调。`ProviderError` 把错误分为 `Network`、`Authentication`、`RateLimit`、`InvalidResponse`、`Cancelled`、`Timeout`、`Unknown`。

当前请求结构不携带 PID、handle 或 `processRevision`。因此 runId 只标识编排轮次，不标识设备目标。这是已知缺口，不应把 runId 当作目标一致性保护。

### 3.3 Run 和 trace

`AgentRun` 保存：

- `id`
- `AgentRunState`
- model turn/tool step 计数
- 最多 128 条 `AgentTraceEvent`
- 待审批工具
- `AgentApprovalDecision`

`AgentRunner` 是不依赖 ImGui 的状态机。UI 只消费它返回的 `Outcome`：

- `NeedsConfirmation`
- `NeedsExecution`
- `ReadyForFollowUp`
- `Stopped`

`approvalDecision` 当前会在 outcome 更新后很快重置为 `Pending`，可靠的审批审计来源是 trace 和 tool message。

## 4. 一次 model -> tool -> model 循环

```text
User send
  -> ChatSession::getMessagesForRequest()
  -> AgentController::dispatchModelRequest()
  -> provider::sendCompletion()
  -> HttpClient::postAsync()
  -> UIMessageQueue(Token / Completion / Error)
  -> ChatWindow::pollMessages()
       |
       +-- plain assistant text -> complete
       |
       +-- tool calls
             -> AgentRunner::beginToolCalls()
             -> ReadOnly: execute
             -> Write: approval or auto-approve
             -> ChatWindow::startToolExecution()
             -> ToolExecutor::execute()
             -> UIMessageQueue(ToolResult)
             -> AgentRunner::completeToolExecution()
             -> follow-up model request
```

### 4.1 历史清洗

所有 provider 请求必须使用 `ChatSession::getMessagesForRequest()`，而不是直接发送 `messages_`。它负责：

- 只注入配置的 system prompt，过滤运行时 system 通知。
- 拒绝空/重复 tool call id。
- 只输出完整的 assistant tool calls + 对应 tool results。
- 丢弃孤儿 tool result。
- 对不完整工具组降级为安全的纯文本历史。

这条边界用于满足 Anthropic/OpenAI 风格 API 对 tool use/result 配对的要求。

### 4.2 预算和失败传播

`AgentRunner::Config` 当前包含：

- `maxAgentSteps`：默认 12，范围 `[1, 64]`。
- `maxToolCallsPerTurn`：默认 16，范围 `[1, 64]`。
- `autoApproveWrites`：默认 false。

单个工具失败后，本批剩余工具会标记为 skipped，再把失败审计回喂模型。工具 executor 返回的顶层 `error` 或 `success=false` 会由 `ToolExecutor` 转为失败。

### 4.3 流式完成语义

HTTP 2xx 不等于 provider stream 完整：

- Claude 只有收到 `message_stop` 才是完整流。
- OpenAI/DeepSeek 需要合法 `finish_reason` 或等价终止信号。
- 公共 `SSEParser` 当前会过滤 `[DONE]`。

当前实现没有在完成回调中强制校验这些终止条件。Claude 的 `completed` 和 DeepSeek 的 `finished` 会被设置但不读取，OpenAI 不记录终止状态。因此截断的文本/tool fragments 可能被作为成功 `Completion`。修复前，不能把 2xx + connection close 当作模型完整回答。

## 5. 实际线程与生命周期模型

当前实现不只有“主线程 + 一个 HTTP worker + 一个工具 worker”。实际线程如下：

| 线程/任务 | 创建位置 | 所有权现状 | 主要行为 |
|-----------|----------|------------|----------|
| ImGui 主线程 | `main.cpp` | 主循环拥有 | UI、run 状态、会话、队列消费 |
| HTTP worker | `HttpClient::postAsync()` | detached，`inFlight_` 计数 | HTTPS、SSE、provider 完成回调 |
| 工具外层 worker | `ChatWindow::startToolExecution()` | detached，`inFlightWorkers_` 计数 | 等待 `ToolExecutor::execute()`，投递 `ToolResult` |
| 工具内层 executor | `runExecutorAsync()` | detached，未登记 | 真正执行 socket/tool 函数 |
| IPC accept thread | `IpcServer::Start()` | `serverThread_`，可 join | accept 客户端 |
| IPC client handler | `IpcServer::ServerThread()` | 每连接 detached，未登记 | HTTP 解析、handler、发送响应 |

### 5.1 UI 线程边界

对内置 Agent 而言，后台线程不得直接触碰 ImGui 或 `AgentRun`。它们通过 `UIMessageQueue` 投递：

- `Token`
- `Completion`
- `Error`
- `ToolResult`

`ChatWindow::pollMessages()` 每帧消费，并用 active run id 丢弃陈旧消息。

### 5.2 取消和超时的真实语义

| 操作 | 当前效果 | 不保证 |
|------|----------|--------|
| Stop/`cancelRequest()` | 设置 HTTP token、清 active id、结束当前编排 | 不取消已开始的工具，不回滚写操作 |
| HTTP cancellation token | content receiver 在数据块边界中止 | 阻塞 read 立刻结束、worker 已 join |
| 只读工具 timeout | 外层等待者返回 timeout | 内层 executor 已退出 |
| 写工具 timeout | 外层继续 `future.get()` 等真实结果 | 用户 Stop 能终止它 |
| runId 过滤 | 迟到结果不污染新 UI run | 迟到操作没有设备副作用 |
| `SocketIoTimeout` | 给当前线程的 socket I/O 设置期限 | 事务取消、任务所有权、连接状态自动恢复 |
| `DrainPending()` | 丢弃调用瞬间已经可读的旧字节 | timeout 后迟到响应不会污染下一请求 |
| MCP HTTP timeout | Python 停止等待，部分读方法会重试 | 旧 IPC handler/设备请求已取消 |

### 5.3 退出顺序与边界

`main.cpp` 当前依次调用：

1. `IpcServer::Stop()`
2. `HttpClient::shutdown()`
3. `ToolExecutor::shutdown()`
4. ImGui/renderer teardown

三个 shutdown 都不是完整排空保证：

- IPC Stop 不等待 client handler。
- HTTP 在完成回调前减少 `inFlight_`。
- ToolExecutor 只统计外层工具 worker，不统计只读超时后继续运行的内层 executor。
- HTTP/工具等待上限为 3 秒。

因此这里应称为 **best-effort teardown**。修复前，不要在文档或新代码注释中声称 detached worker 一定不会越过单例析构。

连接按钮和自动重连还有另一条生命周期：`ConnectMultiPort()`/`DisconnectMultiPort()` 没有与端口请求共享统一的 connection gate，可在 Agent/IPC send/recv 期间直接 `Close()`/替换 socket。`WindowsSocketClient::sock_` 和 `connected_` 也不是受同一 mutex 保护的并发状态。

## 6. Agent 状态机

UI 有粗粒度 `ChatWindow::State`，编排器有细粒度 `AgentRunState`。

```text
Idle
  -> WaitingModel
       -> Completed / Failed / Cancelled
       -> ExecutingTools
            -> WaitingApproval
                 -> Approved -> ExecutingTools
                 -> Denied   -> FollowUp
            -> ToolResult -> next tool or FollowUp
  -> FollowUp -> WaitingModel
```

写类审批只存在于内置 Agent。MCP/IPC 请求不经过该状态机。

`AgentRunner` 会保持工具调用顺序：结果必须和当前 pending call 的 id/name/arguments 匹配，否则停止当前批次。runId 再在 UI 消息层隔离上一个 run 的迟到结果。

## 7. 工具系统

### 7.1 注册和执行

`ToolExecutor::execute()` 的顺序：

1. 在注册表中查工具。
2. 解析 arguments JSON。
3. 用注册 schema 校验。
4. 启动 executor。
5. 解析返回 JSON，识别顶层 `error` 或 `success=false`。
6. 生成 `ToolResult`。

当前注册表有 **39 个可执行名称**。其中 7 个旧名称是隐藏兼容 alias，不发送给 provider；模型实际收到 32 个定义。首批迁移如下（H=hidden）：

| 域 | 工具（R=当前 `ReadOnly`，W=当前 `Write`） |
|----|--------------------------------------------|
| 状态/驱动 | `status` R, `get_status` H, `get_server_version` H, `get_architecture` H, `init_driver` W |
| 内存读 | `memory_read` R, `read_memory` H, `read_value` R, `read_disassembly` R |
| 内存写 | `memory_write` W, `write_bytes` W, `write_value` W |
| 扫描 | `scan_set_range` W, `scan_value` W, `scan_next` W, `scan_fuzzy` W, `scan_hex` W, `get_scan_count` R, `get_scan_results` R, `clear_scan` W |
| 进程/模块 | `process_list` R, `get_process_list` H, `list_processes` H, `process_open` W, `open_process` H, `get_module_list` R, `list_modules` R, `get_module_base` R, `resolve_offset_chain` R |
| 断点 | `set_breakpoint` W, `remove_breakpoint` W, `read_breakpoint_info` R, `suspend_breakpoint` W, `resume_breakpoint` W |
| 符号 | `resolve_symbol` R, `symbol_init` R, `symbol_list` R, `symbol_find` R |
| 脚本 | `execute_lua` W |

`symbol_init` 和 `symbol_list` 当前按“不会修改目标内存”分类为 ReadOnly，但会改变服务端 active symbol table。默认 prompt 仍把它们称为 write-classified，这是待统一的安全语义。

### 7.2 审批边界

`ToolSafety::Write` 且 `autoApproveWrites=false` 时，`AgentRunner` 产生 `NeedsConfirmation`。审批框展示工具名和 arguments。

当前审批不包含：

- PID/进程名/process revision
- 当前 scan/symbol epoch
- endpoint/provider 数据去向

扩展危险工具时，不能只依赖工具名分类；还需要目标绑定和资源域元数据。

## 8. 共享状态与事务边界

### 8.1 目标进程

`AppContext` 持有：

- `selectedPid`
- `processHandle`
- `processRevision`
- selected process name
- module/symbol cache

`selectProcess()` 会清理旧进程相关服务、打开新 handle、设置当前 PID、失效缓存并推进 revision。首批 `MemService` 操作在执行前后校验 target snapshot，但当前 Agent run/审批仍没有从模型产出时捕获该 revision。

### 8.2 端口锁只保证单命令

`SocketCommand::execute*` 和端口 mutex 保证一个 request-response 不被并发响应串包。它们不保证多个命令组成的业务操作原子。

当前复合序列包括：

- `ScanSetRange` -> `ScanValue`/fuzzy/hex scan
- `SymbolInit` -> `SymbolGetList`
- `AppContext::selectProcess()` 的清理/open/set PID/cache 流程

GUI、内置 Agent、IPC/MCP 可在两步之间插入。新增复合工具时应增加高层事务锁、revision/epoch 校验，或把操作下沉为服务端单命令。

### 8.3 地址语义

规范 `memory_read` 已拒绝无 `0x` 前缀的地址；隐藏 `read_memory` 和尚未迁移的内置工具仍保留旧十六进制解析，IPC/MCP 对无前缀字符串仍按十进制解析。跨前端继续只使用明确的 `0x` 地址字符串。

### 8.4 timeout、连接 generation 与协议恢复

Android 协议在共享 TCP 字节流上没有 request id/帧 generation。当前 `Send()`/`Receive()` 遇 `WSAETIMEDOUT` 后保留连接，下一请求只做一次非阻塞 `DrainPending()`。

socket manager 已增加单调 connection generation，首批 `MemService` 操作会在执行前后拒绝跨 generation 结果；但 connect/disconnect 仍没有 lifecycle gate，timeout 也尚未 poison 连接。generation 校验是检测边界，不是并发关闭问题的修复。

这个策略不能覆盖“旧响应在 drain 后才到”的情况，也不能修复 partial send/receive。可靠边界应是：

1. timeout 后把连接标为 poisoned。
2. 在 lifecycle exclusive lock 下关闭/重连。
3. 生成新的 connection generation。
4. 使 process handle、Agent target snapshot 和在途命令都绑定 generation。

仅把 `connected_` 改成 atomic 不足以解决 socket handle 被并发关闭和旧状态跨连接复用。

## 9. 持久化与数据边界

所有路径相对进程工作目录：

| 文件 | 管理者 | 内容 | 安全/一致性说明 |
|------|--------|------|-----------------|
| `ai_config.json` | `ApiKeyStore` | provider endpoint/model/API key | key 用 Windows DPAPI；加载失败处理仍可能覆盖文件 |
| `ai_settings.json` | `AiSettings` | provider 选择、prompt、代理、预算、token limit、自动审批 | 明文；损坏与缺失当前都可能写默认值 |
| `ai_sessions/index.json` | `SessionManager` | 会话元数据和 active id | 损坏时可能重建为空索引并孤立会话文件 |
| `ai_sessions/<id>.json` | `ChatSession` | 消息、tool calls/results、prompt、token limit | 明文；可能包含卡密、Lua、地址和内存数据 |

### 9.1 全局设置与会话字段冲突

`ai_settings.json` 把 system prompt/token limit 定义为全局设置；会话文件也保存同名字段。启动时先应用全局值，再 load 会话，因此会话值胜出。创建新会话还会继承前一个会话留在内存中的值。

维护者需要明确选择“全局”或“每会话”模型，不能继续让两种来源隐式竞争。

### 9.2 写盘保证

`ApiKeyStore`、`AiSettings`、`SessionManager` 使用 `utils::installTempFile()`。`ChatSession` 使用自己的 `.tmp` + rename，失败后覆盖 copy。后者在 Windows 常见目标已存在场景中不是严格原子替换。

任何加载器都应先完整解析到临时对象，失败时保留原文件和原内存状态。当前实现尚未全部满足。

### 9.3 远端 provider 边界

会发送给 provider 的数据包括 system prompt、用户消息、模型历史、工具参数和工具结果。DPAPI 只保护本地磁盘上的 provider key，不会保护发送给 endpoint 的数据。

`OpenAIProvider` 当前默认 endpoint 是 `https://ai.ikik.net/v1`，即第三方兼容网关。UI 只验证 HTTPS，使用者必须把 endpoint 域名本身视为信任决策。

### 9.4 Context 预算

provider 声明了 `maxContextTokens`，但当前没有调用方读取 `getCapabilities()`。会话 token limit 可设到 1,000,000，裁剪只按消息 UTF-8 字节数/4，未计工具 schema、provider JSON 开销和输出 token 预留。

因此 `tokenLimit` 只是本地近似阈值，不是“请求一定适配当前模型”的保证。自定义 endpoint/model 还需要显式的 context 配置。

## 10. IPC 与 MCP

### 10.1 IPC 协议

`IpcServer` 监听 `127.0.0.1:28100`，接受：

```json
{
  "method": "read_memory",
  "params": {
    "address": "0x1234",
    "size": 16
  }
}
```

响应为：

```json
{
  "success": true,
  "result": {}
}
```

请求 parser 已校验 method/path/Content-Length，并设置 1 MiB 请求上限。当前响应发送只有一次 `send()`，没有 short-write 循环。

### 10.2 MCP 层

`mcp/amem_mcp/` 是 Python 3.10+ FastMCP package：

- `server.py`/入口负责 stdio MCP。
- `tools/` 按 status/process/memory/scan/breakpoint/lua/symbols 分域。
- `ipc_client.py` 通过 HTTP 调用 GUI。
- `constants.py` 镜像 C++ 扫描 flag、数据类型和内存区枚举。
- `helpers.py` 做整数、字节和输出格式转换。

GUI 必须已运行并连接设备。Python server 不直接连接 Android。

三条工具面当前并不一一对应：

| 入口 | 静态名称数 | 说明 |
|------|------------|------|
| 内置 Agent | 32 个广告定义 / 39 个可执行名称 | 7 个旧名称仅作隐藏兼容 |
| IPC | 29 | 原始 C++ handler；含未被 MCP 包装的 `read_batch` |
| MCP | 30 | Python wrapper 把 typed read/write 映射到 IPC |

内置 Agent 另有 `read_disassembly`、`resolve_symbol` 等。无 LuaJIT 时三层对 `execute_lua` 的可见性也不同。新增能力时不能只验证“socket 命令存在”，需要 capability/feature-gate 契约。

### 10.3 当前 IPC 安全边界

IPC 只绑定 loopback，但没有认证，并返回 `Access-Control-Allow-Origin: *`，还接受浏览器 OPTIONS。外部 MCP 路径又不经过内置 Agent 的写审批。因此新增 IPC 写能力前必须先考虑鉴权、浏览器访问和 capability，而不能只写参数校验。

Python `IpcClient` 会对部分读方法在 timeout/网络错误后默认重试。HTTP timeout 只结束 Python 等待，不会停止 detached C++ handler；重试可让旧、新请求同时排队。没有 server request id/cancellation 前，retry-safe 还必须包含“旧请求继续运行也不会破坏共享状态/资源”的判断。

## 11. 维护不变量与当前缺口

### 必须保持的不变量

1. 后台线程不直接访问 ImGui/run 状态；内置 Agent 通过 `UIMessageQueue` 回主线程。
2. 每个 socket request-response 使用 `SocketCommand::execute*` 或等价端口锁。
3. provider 请求使用 `ChatSession::getMessagesForRequest()` 保持 tool use/result 配对。
4. 会修改目标的内置工具不得误标为 ReadOnly。
5. 扫描 flag/数据类型/内存区枚举变更时同步 `mcp/amem_mcp/constants.py`。
6. 地址跨前端传递时使用 `0x` 前缀。
7. 工具、IPC、provider 和持久化入口都要在分配前验证不可信长度。
8. provider streaming 成功必须有合法终止事件，partial content 不进入工具执行。
9. socket timeout 后不得假定 `DrainPending()` 已恢复协议同步。

### 尚未满足、不能假定成立的目标

1. shutdown 返回时所有后台任务均已退出。
2. Stop 会取消已开始的工具。
3. runId 能保证目标进程没有改变。
4. 端口锁能让复合业务操作成为事务。
5. loopback IPC 等同于已鉴权。
6. 所有 AI 持久化文件都严格原子且损坏时不覆盖。
7. ReadOnly 一定没有共享状态变化。
8. connect/disconnect 不会与在途 send/recv 并发。
9. provider `maxContextTokens` 会自动限制实际请求。
10. 内置 Agent、IPC 和 MCP 暴露相同能力。

## 12. 扩展检查单

### 新增设备工具

1. 在 `socket/*Commands.cpp` 实现并在 `client_singleton.h` 声明，优先使用 `SocketCommand::execute*`。
2. 明确它绑定哪个资源：process、scan session、symbol table、breakpoint 或全局 driver。
3. 判断是 target mutation、stateful read 还是纯 read，并同步默认 prompt。
4. 在 `ToolDefinitions.cpp` 添加 schema、局部长度上限和 executor。
5. 若暴露给外部 Agent，同步 IPC handler、MCP wrapper、常量和错误语义。
6. 对地址统一要求 `0x`，对列表设计分页和输出上限。
7. 若是复合操作，设计事务/revision，而不是连续调用两条各自加锁的命令。
8. 写工具审批必须显示并校验目标 PID/revision。
9. 更新机器可验证的 capability matrix，并明确 feature unavailable 行为。

### 新增 Provider

1. 实现 `AIProvider` 并在 `ProviderRegistry` 注册。
2. 网络 I/O 使用受管的 `HttpClient` 路径，回调只投递 `UIMessageQueue`。
3. 限制累计响应、单 SSE 事件、assistant content 和 tool arguments。
4. 只有看到 provider 合法终止事件才提交成功；截断流保留为 partial error。
5. 根据 provider/model context 预算消息、工具 schema 和输出预留。
6. 正确处理取消、HTTP 错误和 tool call 增量拼装。
7. 默认 endpoint 必须明确归属，非官方/第三方网关需要 UI 披露。

### 修改持久化

1. 解析到临时对象，成功后 commit。
2. 区分 missing、invalid 和 I/O error。
3. 失败保留原文件，不自动覆盖。
4. 标注哪些字段是密文、明文、会发给远端 provider。
5. 用真实 Windows 覆盖场景验证原子替换和恢复。

## 13. 测试边界

当前已有一个无设备 CTest：`native_agent_mem_service`，覆盖首批 service/adapter、target/generation 和隐藏 alias。以下路径仍缺测试：

- provider SSE/full-response 解析和完整终止验证
- ChatSession 工具配对与裁剪
- config/index 损坏恢复
- AgentRunner 预算/审批/失败顺序
- ToolExecutor schema 和错误契约
- IPC HTTP/auth/sendAll/capability
- fake socket timeout、迟到响应和 connection generation
- C++/IPC/MCP 名称、常量和 feature gate 对齐

真实 Android 设备测试保留给协议兼容、驱动和硬件断点 smoke test。
