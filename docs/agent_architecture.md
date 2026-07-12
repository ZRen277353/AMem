# AMem AI Agent 架构文档

适用分支：`NativeAgent`（基线来自 `AIChat`）
最后更新：2026-07-12

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
| 工具调度 | `AgentTaskExecutor` | 有界串行队列、绝对 deadline、run cancellation 和 joinable worker 生命周期 |
| 工具注册 | `ToolExecutor`, `ToolDefinitions.cpp` | schema 校验、安全分类、同步执行、结果规范化、广告/隐藏兼容名称 |
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

`CompletionRequest` 本身不携带设备目标；目标一致性由同一 `AgentController` 持有的 `AgentRunContext` 负责。run 创建时捕获 connection generation 和 target snapshot，后续模型请求、审批与工具批次沿用该 context。`runId` 仍只负责异步消息隔离，不能替代 snapshot 校验。

### 3.3 Run 和 trace

`AgentRun` 保存：

- `id`
- `AgentRunState`
- model turn/tool step 计数
- 最多 128 条 `AgentTraceEvent`
- 待审批工具
- `AgentApprovalDecision`
- `AgentRunContext`：run id、`OperationContext`、connection generation、可选 target snapshot 和 cancellation token

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
             -> AgentTaskExecutor::enqueue()
             -> owned worker -> ToolExecutor::execute()
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

内置工具执行已经收敛到一个受管 worker；HTTP 和 IPC client handler 的生命周期仍未闭环：

| 线程/任务 | 创建位置 | 所有权现状 | 主要行为 |
|-----------|----------|------------|----------|
| ImGui 主线程 | `main.cpp` | 主循环拥有 | UI、run 状态、会话、队列消费 |
| HTTP worker | `HttpClient::postAsync()` | detached，`inFlight_` 计数 | HTTPS、SSE、provider 完成回调 |
| Agent 工具 worker | `AgentTaskExecutor` 构造 | 单个 joinable `std::thread` | 串行取队列、同步调用 `ToolExecutor`、投递 completion callback |
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
| Stop/`cancelRequest()` | 设置 HTTP token，调用 `AgentTaskExecutor::cancelRun()`，清 active id 并结束当前编排 | 撤回已发送写操作、让迟到回执进入当前会话/trace |
| HTTP cancellation token | content receiver 在数据块边界中止 | 阻塞 read 立刻结束、worker 已 join |
| 排队任务取消/deadline | worker 在执行前返回 `cancelled_before_start`/`timed_out_before_start` | 已开始操作被抢占 |
| 活动工具取消/deadline | 同一 `OperationContext` 传到 service 和 socket I/O；worker 始终受管 | legacy executor 立即响应、已发送设备命令被撤回 |
| deadline 后完成 | 只读结果归一为 `timed_out`；写结果保留 `completed_after_deadline` 或底层不确定状态 | Stop 后 UI 一定展示最终回执 |
| runId 过滤 | 迟到结果不污染新 UI run | 迟到操作没有设备副作用 |
| `SocketIoTimeout` | 给当前线程的 socket I/O 设置期限；I/O 失败会 poison session | 事务取消、任务所有权、自动重连/状态恢复 |
| `DeviceSession` poison | 推进 generation、拒绝新请求并等待显式重连 | 自动恢复 driver/process/scan/breakpoint 状态 |
| MCP HTTP timeout | Python 停止等待，部分读方法会重试 | 旧 IPC handler/设备请求已取消 |

### 5.3 退出顺序与边界

`main.cpp` 当前依次调用：

1. `IpcServer::Stop()`
2. `HttpClient::shutdown()`
3. `AgentTaskExecutor::shutdown()`
4. `DisconnectMultiPort()`
5. ImGui/renderer teardown

工具路径已有明确的排空保证：`AgentTaskExecutor::shutdown()` 停止接收新任务，标记 active/queued task 取消，向未开始任务交付取消结果，并 join 唯一 worker。返回后不会再有 Agent 工具访问 socket，因此设备断开排在它之后。

进程级 teardown 仍不是完整排空保证：

- IPC Stop 不等待 client handler。
- HTTP 在完成回调前减少 `inFlight_`。
- HTTP 等待上限为 3 秒，且 worker 仍是 detached。

因此只可把 **Agent 工具 shutdown** 称为已 join；整个应用退出仍受 HTTP/IPC detached task 限制，不能宣称所有后台任务已排空。

连接按钮、自动重连和退出现在都通过 `DeviceSession` exclusive lifecycle lease；普通命令持 shared request lease。连接替换会等待活动请求释放，失败连接会 poison 并推进 generation。该锁序已有无设备测试，真实三端口并发和迟到字节仍需 fake transport/loopback 压力验证。

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

1. `ChatWindow` 捕获 run 的 `OperationContext` 并提交到 `AgentTaskExecutor`；入队时固定绝对 deadline。
2. 唯一 worker 在执行前拒绝已取消或队列中已超时的任务。
3. `ToolExecutor` 在注册表中查工具，解析 arguments JSON 并按 schema 校验。
4. 在同一 worker 上同步调用 executor，把同一 deadline/cancellation 传入 service/socket。
5. 解析返回 JSON，识别顶层 `error`、`success=false` 和 completion 状态。
6. completion callback 把 `ToolResult` 投递到 `UIMessageQueue`。

当前注册表有 **43 个可执行名称**。其中 13 个旧名称是隐藏兼容 alias，不发送给 provider；模型实际收到 30 个定义。当前目录如下（H=hidden）：

| 域 | 工具（R=当前 `ReadOnly`，W=当前 `Write`） |
|----|--------------------------------------------|
| 状态/驱动 | `status` R, `get_status` H, `get_server_version` H, `get_architecture` H, `init_driver` W |
| 内存读 | `memory_read` R, `read_memory` H, `memory_read_value` R, `read_value` H, `read_disassembly` R |
| 内存写 | `memory_write` W, `write_bytes` H, `memory_write_value` W, `write_value` H |
| 扫描 | `scan_set_range` W, `scan_value` W, `scan_next` W, `scan_fuzzy` W, `scan_hex` W, `get_scan_count` R, `get_scan_results` R, `clear_scan` W |
| 进程/模块 | `process_list` R, `get_process_list` H, `list_processes` H, `process_open` W, `open_process` H, `module_list` R, `get_module_list` H, `list_modules` H, `module_resolve` R, `get_module_base` H, `resolve_offset_chain` R |
| 断点 | `set_breakpoint` W, `remove_breakpoint` W, `read_breakpoint_info` R, `suspend_breakpoint` W, `resume_breakpoint` W |
| 符号 | `resolve_symbol` R, `symbol_init` R, `symbol_list` R, `symbol_find` R |
| 脚本 | `execute_lua` W |

`symbol_init` 和 `symbol_list` 当前按“不会修改目标内存”分类为 ReadOnly，但会改变服务端 active symbol table。默认 prompt 仍把它们称为 write-classified，这是待统一的安全语义。

### 7.2 审批边界

`ToolSafety::Write` 且 `autoApproveWrites=false` 时，`AgentRunner` 产生 `NeedsConfirmation`。审批框展示工具名、arguments、预期 connection generation、PID 和 process revision。

`ToolRegistration::targetPolicy` 进一步区分：

- `None`：只绑定 connection generation，例如 `status`、`process_list`。
- `Bound`：执行前后绑定完整 target snapshot。
- `Selection`：审批时绑定旧 selection，成功结果携带并显式推进新 snapshot。

当前审批仍不包含：

- 进程名和持久化 effect 审计
- 当前 scan/symbol epoch
- endpoint/provider 数据去向

九个已迁移工具（`status`、`process_list`、`process_open`、`module_list`、`module_resolve`、raw/typed memory read/write）在 service 边界消费 `OperationContext`。模块解析按完整名、basename、唯一子串依次匹配并拒绝歧义；`ValueCodec` 统一 scalar 类型别名、范围、little-endian 和有限浮点写入。其余旧 executor 已有 Controller 出队/结果保护，但 actual send 仍读取共享状态；迁移完成前不能把 target mutation 视为完整原子边界。

## 8. 共享状态与事务边界

### 8.1 目标进程

`AppContext` 持有：

- `selectedPid`
- `processHandle`
- `processRevision`
- selected process name
- module/symbol cache

`selectProcess()` 会清理旧进程相关服务、打开新 handle、设置当前 PID、失效缓存并推进 revision。`AgentRunContext` 在首轮模型请求前捕获该 snapshot；审批、出队和结果回收均复核。`process_open` 是特殊的 `Selection` 工具：send 前校验旧 snapshot，成功后只在返回 target 与当前状态一致时推进 run。

### 8.2 端口锁只保证单命令

`SocketCommand::execute*` 和端口 mutex 保证一个 request-response 不被并发响应串包。它们不保证多个命令组成的业务操作原子。

当前复合序列包括：

- `ScanSetRange` -> `ScanValue`/fuzzy/hex scan
- `SymbolInit` -> `SymbolGetList`
- `AppContext::selectProcess()` 的清理/open/set PID/cache 流程

GUI、内置 Agent、IPC/MCP 可在两步之间插入。新增复合工具时应增加高层事务锁、revision/epoch 校验，或把操作下沉为服务端单命令。

### 8.3 地址语义

规范 raw/typed memory read/write 均拒绝无 `0x` 前缀的地址；隐藏兼容 alias 和尚未迁移的内置工具仍保留旧十六进制解析，IPC/MCP 对无前缀字符串仍按十进制解析。跨前端继续只使用明确的 `0x` 地址字符串。`memory_write_value` 的 qword 参数应使用字符串，避免 JSON/模型链路损失 64-bit 精度。

### 8.4 timeout、连接 generation 与协议恢复

Android 协议在共享 TCP 字节流上没有 request id/帧 generation。`DeviceSession` 现在为普通请求提供 shared lease，为 connect/disconnect/reconnect 提供 exclusive lifecycle gate；任意 I/O 错误、EOF 或 partial failure 都会 poison session、推进 generation、关闭失败 client 并拒绝新请求。旧的待处理字节清理恢复路径已删除。

进程切换保持 connection -> process -> port 锁顺序，同线程嵌套命令复用已有 request lease。首批 `MemService` 操作在执行前后拒绝跨 generation 结果，应用退出也会在静态析构前显式断开。

当前自动测试验证 lease 互斥、嵌套复用、poison 和 generation 失效；尚未用可注入 transport 证明真实 partial send、迟到响应和三端口重连，因此连接问题仍按“部分修复”跟踪。

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
| 内置 Agent | 30 个广告定义 / 43 个可执行名称 | 13 个旧名称仅作隐藏兼容 |
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
9. socket timeout、EOF 或 partial I/O 必须 poison 当前 `DeviceSession` generation，旧连接在显式重连前不得复用。

### 尚未满足、不能假定成立的目标

1. 应用 shutdown 返回时 HTTP/IPC 后台任务均已退出。
2. Stop 能撤回已发送操作，或保证其迟到回执进入独立审计。
3. 所有 legacy process-bound executor 都在实际 send 边界消费 run snapshot。
4. 端口锁能让复合业务操作成为事务。
5. loopback IPC 等同于已鉴权。
6. 所有 AI 持久化文件都严格原子且损坏时不覆盖。
7. ReadOnly 一定没有共享状态变化。
8. 真实三端口 transport 已覆盖 timeout、partial I/O、迟到字节和 reconnect generation。
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

当前无设备 CTest `native_agent_mem_service` 的 15 个测试组覆盖地址/scalar codec、进程与模块分页/解析、service/adapter、raw/typed write 完成语义、target/generation、连接 lease/poison、审批期间切换/重连、同批 target 推进、非目标工具、队列取消/timeout、active cancellation、shutdown join、晚到结果拒绝和隐藏 alias。以下路径仍缺测试：

- provider SSE/full-response 解析和完整终止验证
- ChatSession 工具配对与裁剪
- config/index 损坏恢复
- AgentRunner 预算上限、auto approve 和 denial 的完整组合
- ToolExecutor schema 和错误契约
- IPC HTTP/auth/sendAll/capability
- fake transport partial I/O、迟到响应和三端口 reconnect
- C++/IPC/MCP 名称、常量和 feature gate 对齐

真实 Android 设备测试保留给协议兼容、驱动和硬件断点 smoke test。
