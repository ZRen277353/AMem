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
| Stop/`cancelRequest()` | 设置 HTTP token，调用 `AgentTaskExecutor::cancelRun()`，清 active id 并结束当前编排；mutation 最终状态在 UI callback 前写独立审计 | 撤回已发送写操作、让迟到回执进入原会话/trace |
| HTTP cancellation token | content receiver 在数据块边界中止 | 阻塞 read 立刻结束、worker 已 join |
| 排队任务取消/deadline | worker 在执行前返回 `cancelled_before_start`/`timed_out_before_start` | 已开始操作被抢占 |
| 活动工具取消/deadline | 同一 `OperationContext` 传到 service 和 socket I/O；worker 始终受管 | legacy executor 立即响应、已发送设备命令被撤回 |
| deadline 后完成 | 只读结果归一为 `timed_out`；写结果保留 `completed_after_deadline` 或底层不确定状态，并写 mutation audit | 原会话接收已过期结果 |
| runId 过滤 | 迟到结果不污染新 UI run；mutation/session-effect 结果仍进入独立 Audit 表 | 迟到操作没有设备副作用 |
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
6. mutation 和 symbol-session effect 先写入有界 `AgentMutationAuditLog`；该步骤发生在 completion callback 之前。
7. completion callback 把 `ToolResult` 投递到 `UIMessageQueue`；旧 run 结果仍可被 UI 过滤，不影响独立审计。

当前 LuaJIT 构建的注册表有 **57 个可执行名称**。其中 33 个旧名称是隐藏兼容 alias，不发送给 provider；模型实际收到 24 个定义。无 `HAVE_LUAJIT` 时 canonical/alias Lua 均不注册，目录为 55 个可执行、32 个隐藏、23 个广告定义。当前 LuaJIT 目录如下（H=hidden）：

| 域 | 工具（R=当前 `ReadOnly`，W=当前 `Write`） |
|----|--------------------------------------------|
| 状态/驱动 | `status` R, `get_status` H, `get_server_version` H, `get_architecture` H, `driver_initialize` W, `init_driver` H |
| 内存读 | `memory_read` R, `read_memory` H, `memory_read_value` R, `read_value` H, `disassemble` R, `read_disassembly` H |
| 内存写 | `memory_write` W, `write_bytes` H, `memory_write_value` W, `write_value` H |
| 扫描 | `scan_start` W, `scan_refine` W, `scan_results` R, `scan_clear` W, `scan_set_range` H, `scan_value` H, `scan_next` H, `scan_fuzzy` H, `scan_hex` H, `get_scan_count` H, `get_scan_results` H, `clear_scan` H |
| 进程/模块 | `process_list` R, `get_process_list` H, `list_processes` H, `process_open` W, `open_process` H, `module_list` R, `get_module_list` H, `list_modules` H, `module_resolve` R, `get_module_base` H, `pointer_resolve` R, `resolve_offset_chain` H |
| 断点 | `breakpoint_set` W, `breakpoint_remove` W, `breakpoint_hits` R, `breakpoint_suspend` W, `breakpoint_resume` W, `set_breakpoint` H, `remove_breakpoint` H, `read_breakpoint_info` H, `suspend_breakpoint` H, `resume_breakpoint` H |
| 符号 | `symbol_resolve` R, `symbol_list` R, `resolve_symbol` H, `symbol_init` H, `symbol_find` H |
| 脚本 | `lua_execute` W, `execute_lua` H（两者均受 `HAVE_LUAJIT` 约束） |

当前二元安全模型把 `Write` 定义为需要审批的目标/主机 mutation。规范 `symbol_resolve`/`symbol_list` 不修改目标内存，因此保持 ReadOnly；它们内部的 active-table session mutation 由 transaction + epoch 约束。默认 prompt 已只列规范名称且不再错误声称需要写审批。未来扩展 effect 元数据时应把它们标为 `SessionMutation`，但不重新暴露 `symbol_init` 前置步骤。

### 7.2 审批边界

`ToolSafety::Write` 且 `autoApproveWrites=false` 时，`AgentRunner` 产生 `NeedsConfirmation`。审批框展示工具名、arguments、预期 connection generation、PID 和 process revision。

`ToolRegistration::targetPolicy` 进一步区分：

- `None`：只绑定 connection generation，例如 `status`、`process_list`。
- `Bound`：执行前后绑定完整 target snapshot。
- `Selection`：审批时绑定旧 selection，成功结果携带并显式推进新 snapshot。

当前审批仍不包含：

- 进程名
- catalog-owned effect/resource metadata（当前独立审计按 canonical/alias 名称归类）
- endpoint/provider 数据去向

二十三个 canonical 工具（`status`、`driver_initialize`、`process_list`、`process_open`、module/pointer/disassembly resolution、四个 canonical scan、两个 canonical symbol、五个 canonical breakpoint、raw/typed memory read/write）在 service 边界消费 `OperationContext`。driver 初始化区分未发送、服务端拒绝、发送后未知及确认后 cancel/deadline，且卡密不会进入审批显示、tool audit 或 session JSON。pointer、scan 和 symbol 保持各自事务/epoch 语义；breakpoint mutation 统一区分未发送、设备拒绝、发送后未知和确认后 cancel/deadline，hit batch 在 Agent 边界限制为最新 100 项并报告丢弃数。`lua_execute` 在 host 执行前复核 target/generation，并以任务 absolute deadline 限制 Lua hook timeout；开始后不能硬取消。其余旧 executor 已有 Controller 出队/结果保护，但 actual send 仍读取共享状态；迁移完成前不能把所有 target mutation 视为完整原子边界。

### 7.3 独立 mutation audit

`AgentTaskExecutor` 在 write-classified 工具和 symbol session 工具的 completion callback 前写 `ai_mutation_audit.jsonl`。记录包含 run/tool-call id、脱敏参数摘要、approval、effect/resource domain、预期 generation/target、duration、success 和 completion。manual deny 与队列拒绝由 `ChatWindow` 在没有 worker outcome 时补写。Stop、Clear、New、session switch/delete 和窗口析构统一向 active run 发送取消请求。

单条记录上限 64 KiB；active 文件上限 4 MiB，轮转为一个 `.1` 备份；内存只保留最近 100 条供 Audit 表显示。driver message/card、Lua code/error/output、raw memory data、register/hit/items 和大字段不原样保存。JSONL 尾部损坏记录会在加载时跳过，但日志仍是明文且写盘失败不能被描述为绝对可靠审计。

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

`SocketCommand::execute*` 的端口 mutex 保证一个 request-response 不被并发响应串包。每条命令还会短暂持有可重入的 per-port transaction gate；复合操作只有显式把 `SocketCommand::TransactionLease` 保持到最后一步，才会阻止其他前端在命令之间插入。

`pointer_resolve` 已使用该高层 gate：system backend 在稳定 generation/PID/handle/revision 上获得事务，`MemService` 在事务内完成 module list、唯一匹配和每次 8-byte pointer read，释放后再做完整 target 校验。旧 GUI/Lua/IPC 使用的 `ResolveModuleOffsetChain()` 也持有同一 gate，但仍保留旧的首个子串匹配和结果契约。

canonical scan 也使用该 gate 和独立 domain mutex。`scan_start` 在一个 MAIN transaction 内发送 range 与 start；每次 scan mutation（包括 IPC/隐藏旧入口和 DEBUG stop）推进单调 epoch。`scan_refine`、`scan_results`、`scan_clear` 校验 `{target, scanEpoch}`；结果页把 count+page 放在同一事务，clear 发送后再以 count=0 确认。已发送但没有 terminal count 的 start/refine 返回 `completion_unknown`。GUI `ScanWindow` 注入同一个 `IMemService`，start/refine/results/clear/remove 均绑定 target 与 epoch；remove 在一个 transaction 内去重地址、确认前后 count 并要求 epoch 前进。Stop 只设置共享 token，由 system backend 的 progress callback 发送一次 DEBUG stop。

canonical symbol 使用独立 domain mutex 和 MAIN transaction。`symbol_resolve`/`symbol_list` 在一个事务内完成 module 唯一匹配、`SymbolInit` 与 find/page；每个 init 在已持有 transaction gate 后推进单调 epoch。续页必须带上一页 epoch，IPC/隐藏 alias 的 init 会使其失效。GUI 的 `loadSymbolTable` 在同一个 transaction 内只 init 一次，并循环读取全部 1000-item protocol page；总量限制为 1,000,000 项和 64 MiB 名称。完整 target snapshot 仍在释放 transaction 后复核，以保持 connection -> process -> domain -> port 锁顺序。

`AppContext::ModuleCache` 只保留 GUI presentation cache 职责。`MemoryViewerWindow`/`BreakpointWindow` 显式传入注入的 `IMemService`；cache miss 不持 cache mutex 做网络 I/O，返回后在 cache mutex 内复核完整 target 与当前 module，再原子安装排序后的 symbol list。这样进程切换先 invalidate 后不会被旧加载结果重新污染。

canonical breakpoint 使用 service domain mutex，单条 mutation 的设备确认和本地 cleanup tracker 更新处于同一 MAIN transaction。`ClearTrackedKernelBreakpoints()` 持 gate 完成 tracker snapshot 和逐项 remove，避免并发 set 落在 cleanup 缝隙；disconnect/reconnect 不向旧 target 发命令，只清本地 tracker。四种 mutation 使用相同 receipt：未发送可重试，已发送无响应为非重试 `completion_unknown`，确认后才报告 completed/cancel/deadline 状态。`BreakpointWindow` 显式注入同一个 `IMemService`，mutation 与 hit refresh 每次捕获 target context。hits 协议没有 offset/cursor，socket 层必须排空一次响应；`ReadKernelBreakpointInfoTail` 按块接收并只保留最新 tail。service DTO 保存 GPR、`orig_x0`、syscall、FPSR/FPCR 和 32 个 128-bit vector registers；GUI 上限 50,000，Agent 上限 100，二者返回 `available/dropped` 而不是虚假 continuation cursor。

当前复合序列包括：

- 旧 IPC/隐藏 alias 的 `ScanSetRange` -> `ScanValue`/fuzzy/hex scan
- 旧 IPC/隐藏 alias 的 `SymbolInit` -> `SymbolGetList`
- `AppContext::selectProcess()` 的清理/open/set PID/cache 流程

canonical pointer/scan/symbol 与 GUI scan/symbol cache 已迁移；旧 IPC/隐藏 scan、IPC/隐藏 symbol 和 process selection 分步流程仍可能被插入。旧 mutation 会使 native epoch 失效，但旧调用自身仍没有 canonical completion/session 契约。新增复合工具时应增加高层事务锁、revision/epoch 校验，或把操作下沉为服务端单命令。持有 transaction gate 时不得再获取 `AppContext` 的 process-state mutex，避免与 process -> command 的既有锁顺序反转。

### 8.3 地址语义

规范 raw/typed memory read/write 地址、`pointer_resolve` offsets 和 breakpoint 地址均拒绝无 `0x` 前缀的字符串；隐藏兼容 alias 和尚未迁移的内置工具仍保留旧十六进制解析，IPC/MCP 对无前缀字符串仍按十进制解析。跨前端继续只使用明确的 `0x` 地址字符串。`memory_write_value` 的 qword 参数和 breakpoint hit/register 值应使用字符串，避免 JSON/模型链路损失 64-bit 精度。

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
| `ai_sessions/<id>.json` | `ChatSession` | 消息、tool calls/results、prompt、token limit | 明文；driver card 字段会脱敏，但仍可能包含 Lua、地址、内存数据和其他完整参数/结果 |
| `ai_mutation_audit.jsonl` / `.1` | `AgentMutationAuditLog` | mutation/session-effect 完成摘要 | 明文、字段脱敏；64 KiB/record，4 MiB active + 一个轮转备份 |

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
| 内置 Agent（LuaJIT） | 24 个广告定义 / 57 个可执行名称 | 33 个旧名称仅作隐藏兼容 |
| 内置 Agent（无 LuaJIT） | 23 个广告定义 / 55 个可执行名称 | 32 个旧名称仅作隐藏兼容；Lua 不注册 |
| IPC | 29 | 原始 C++ handler；含未被 MCP 包装的 `read_batch` |
| MCP | 30 | Python wrapper 把 typed read/write 映射到 IPC |

内置 Agent 另有 `disassemble`、`symbol_resolve`、`breakpoint_hits` 等规范名称。无 LuaJIT 时内置 Agent 不注册 `lua_execute`/`execute_lua`，而 IPC/MCP 的行为仍不同。新增能力时不能只验证“socket 命令存在”，需要 capability/feature-gate 契约。

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
2. 所有 legacy process-bound executor 都在实际 send 边界消费 run snapshot。
3. 端口锁能让复合业务操作成为事务。
4. loopback IPC 等同于已鉴权。
5. 所有 AI 持久化文件都严格原子且损坏时不覆盖。
6. ReadOnly 一定没有共享状态变化。
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

当前无设备 CTest `native_agent_mem_service` 的 22 个测试组覆盖地址/scalar codec、driver receipt/card redaction、进程与模块分页/解析、事务化 pointer resolution、disassembly、scan/symbol session/full-table transaction、breakpoint receipt/rich hit batch、scan 取消/完成未知、mutation audit 脱敏/轮转/晚到 callback 前持久化、service/adapter、raw/typed write 完成语义、target/generation、连接 lease/poison、审批期间切换/重连、同批 target 推进、非目标工具、队列取消/timeout、active cancellation、shutdown join、晚到结果拒绝和隐藏 alias。以下路径仍缺测试：

- provider SSE/full-response 解析和完整终止验证
- ChatSession 工具配对与裁剪
- config/index 损坏恢复
- AgentRunner 预算上限、auto approve 和 denial 的完整组合
- ToolExecutor schema 和错误契约
- IPC HTTP/auth/sendAll/capability
- fake transport partial I/O、迟到响应和三端口 reconnect
- C++/IPC/MCP 名称、常量和 feature gate 对齐

真实 Android 设备测试保留给协议兼容、驱动和硬件断点 smoke test。
