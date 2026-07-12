# AMem AI Agent 架构文档

适用分支：`NativeAgent`（基线来自 `AIChat`）
最后更新：2026-07-13

本文描述当前工作区中的内置 AI Chat、默认关闭的 legacy HTTP IPC、尚未进入产品运行时的 native IPC framing/Hello/request-session/catalog/Observe-dispatch/transport 基础，以及它们共享的设备协议层。Python MCP 代理已经删除。代码走读见 [`agent_walkthrough.md`](./agent_walkthrough.md)，已确认风险和修复优先级见 [`agent_project_issues.md`](./agent_project_issues.md)，NativeAgent 的目标设计和迁移顺序见 [`native_agent_refactor_plan.md`](./native_agent_refactor_plan.md)。

> 本文中的“内置 Agent”指 `gui/ai/` 中由 `ChatWindow` 驱动的 model -> tool -> model 循环。当前没有受支持的外部 Agent adapter；HTTP IPC 默认不编译，只有显式 `ENABLE_LEGACY_HTTP_IPC=ON` 才恢复该待替换或删除的旧入口。native codec、bounded framed I/O、Observe-only Hello、严格 request/session 和安全 Named Pipe 已有无设备测试，但 `ENABLE_NATIVE_IPC` 默认 OFF，主程序没有 start 路径，也没有 `MemService` dispatch/审批，因此不等于 native IPC 可用。

## 1. 系统总览

AMem 默认有 GUI 与内置 Agent 两个设备能力入口；迁移构建可显式加入第三个：

```text
GUI windows --------------------+
                                |
In-app AI Agent                 +--> MemService -> socket/client_singleton.h
ChatWindow -> ToolDefinitions --+                    -> WinSocketClientMgr
                                |                    -> Android server/device
Opt-in HTTP IPC :28100 ---------+
```

`socket/client_singleton.h` 及 `socket/*Commands.cpp` 是设备协议的主要真相源。GUI、内置 Agent 和 IPC handler 都不应各自重写协议。

内置 Agent 完成四件事：

1. 把用户消息、清洗后的历史、provider 配置和工具定义组装为模型请求。
2. 在后台执行 HTTPS/SSE，把 token、完成、错误投递回 ImGui 主线程。
3. 模型返回 tool call 时，执行预算控制、写类审批、工具调用和结果回喂。
4. 保存会话并记录 run trace。

启用后的 HTTP IPC 不经过 `AgentController`、`AgentRunner` 或内置审批框，handler 会直接调用设备命令。默认关闭消除了标准构建的监听面，但 opt-in 入口仍未鉴权，因此其代码级安全问题尚未关闭。

## 2. 目录与职责

| 层 | 主要文件/类 | 职责 |
|----|-------------|------|
| UI | `ChatWindow`, `ChatWindowSettings.cpp` | 对话、会话切换、设置、写工具审批、每帧消费 `UIMessageQueue` |
| 编排 | `AgentController` | provider 查找、请求构造、run id/state/trace |
| 工具循环 | `AgentRunner` | step/call 预算、审批状态机、失败后的跳过、工具审计消息 |
| Provider | `AIProvider`, `ClaudeProvider`, `OpenAIProvider`, `DeepSeekProvider` | provider 请求/响应适配和流式 tool call 拼装 |
| HTTP | `HttpClient` | cpp-httplib + OpenSSL、代理、SSE、协作式取消 |
| 工具调度 | `AgentTaskExecutor` | 有界串行队列、绝对 deadline、run cancellation 和 joinable worker 生命周期 |
| 工具注册 | `ToolExecutor`, `ToolDefinitions.cpp` | canonical schema、安全分类、同步执行、结果规范化和 provider 广告 |
| 原生内存服务 | `mem/`, `MemJsonTools`（AI `AgentMemTools` alias） | 强类型结果、地址/分页校验、target/generation 校验和 AI/IPC 共享 JSON adapter |
| 会话 | `ChatSession`, `SessionManager` | 历史清洗、token/条数裁剪、会话文件和索引 |
| 配置 | `ApiKeyStore`, `AiSettings`, `DefaultSystemPrompt.h` | provider 配置、DPAPI key、全局设置、默认 prompt |
| UI 桥 | `UIMessageQueue` | 内置 Agent 后台线程向 ImGui 主线程投递消息 |
| 全局目标 | `AppContext` | PID、process handle、`processRevision`、模块/符号缓存 |
| IPC | `IpcProtocol`, `IpcFramedConnection`, `IpcHandshakeSession`, `IpcRequestProtocol`, `IpcRequestSession`, `NamedPipeServer`, `NativePipeSecurity`, `IpcServer` | native frame/I/O/Hello/request session/安全 transport 基础；默认关闭的回环 HTTP JSON 入口 |
| 协议排障 | `tools/protocol_reference/` | 可选标准库脚本；不参与产品运行，也不是协议真相源 |
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
- 对 33 个已退役工具名所在的完整调用组降级为普通 assistant 文本，保留脱敏参数和已记录结果，但不再发送 provider tool protocol，也不能重新执行。

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

内置工具执行已经收敛到一个受管 worker；legacy HTTP client handler 的生命周期仍未闭环。Native transport 基础使用一个可 join 的串行 server/handler thread；request session 再拥有一个 joinable serial dispatch worker，使 handler reader 可在执行期间接收 Cancel。产品当前不启动这些 native 线程：

| 线程/任务 | 创建位置 | 所有权现状 | 主要行为 |
|-----------|----------|------------|----------|
| ImGui 主线程 | `main.cpp` | 主循环拥有 | UI、run 状态、会话、队列消费 |
| HTTP worker | `HttpClient::postAsync()` | detached，`inFlight_` 计数 | HTTPS、SSE、provider 完成回调 |
| Agent 工具 worker | `AgentTaskExecutor` 构造 | 单个 joinable `std::thread` | 串行取队列、同步调用 `ToolExecutor`、投递 completion callback |
| IPC accept thread | `IpcServer::Start()` | `serverThread_`，可 join | accept 客户端 |
| IPC client handler | `IpcServer::ServerThread()` | 每连接 detached，未登记 | HTTP 解析、handler、发送响应 |
| Native pipe server/handler | `NamedPipeServer::start()` | 单个 joinable `std::thread`；当前只由测试启动 | overlapped accept、串行 handler、stop event/`CancelIoEx`、实例复用 |
| Native request dispatch | `IpcRequestSession::run()` | 每个已建立 session 一个 owned/joinable worker；当前只由测试启动 | 单 active request、server-owned capability 检查、cooperative deadline/Cancel、bounded response |

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
| 活动工具取消/deadline | 同一 `OperationContext` 传到 service/host 和 socket I/O；worker 始终受管 | 阻塞设备命令被抢占、已发送设备命令被撤回 |
| deadline 后完成 | 只读结果归一为 `timed_out`；写结果保留 `completed_after_deadline` 或底层不确定状态，并写 mutation audit | 原会话接收已过期结果 |
| runId 过滤 | 迟到结果不污染新 UI run；mutation/session-effect 结果仍进入独立 Audit 表 | 迟到操作没有设备副作用 |
| `SocketIoTimeout` | 给当前线程的 socket I/O 设置期限；I/O 失败会 poison session | 事务取消、任务所有权、自动重连/状态恢复 |
| `DeviceSession` poison | 推进 generation、拒绝新请求并等待显式重连 | 自动恢复 driver/process/scan/breakpoint 状态 |
| 外部 HTTP client timeout | client 停止等待 | detached IPC handler/设备请求已取消 |

### 5.3 退出顺序与边界

`main.cpp` 当前按条件依次调用：

1. `IpcServer::Stop()`（仅 `HAVE_LEGACY_HTTP_IPC`）
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

写类审批只存在于内置 Agent。HTTP IPC 请求不经过该状态机。

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

当前 LuaJIT 构建的注册表有 **24 个 canonical 名称**，全部可执行且全部向 provider 广告，没有内置 hidden alias。无 `HAVE_LUAJIT` 时不注册 `lua_execute`，目录为 23 个可执行/广告定义。`native_agent_catalog` CTest 精确校验这组名称，并拒绝 `ToolDefinitions.cpp` 重新依赖 `client_singleton.h` 或 `AppContext.h`。当前 LuaJIT 目录如下：

| 域 | 工具（R=当前 `ReadOnly`，W=当前 `Write`） |
|----|--------------------------------------------|
| 状态/驱动 | `status` R, `driver_initialize` W |
| 内存读 | `memory_read` R, `memory_read_value` R, `disassemble` R |
| 内存写 | `memory_write` W, `memory_write_value` W |
| 扫描 | `scan_start` W, `scan_refine` W, `scan_results` R, `scan_clear` W |
| 进程/模块 | `process_list` R, `process_open` W, `module_list` R, `module_resolve` R, `pointer_resolve` R |
| 断点 | `breakpoint_set` W, `breakpoint_remove` W, `breakpoint_hits` R, `breakpoint_suspend` W, `breakpoint_resume` W |
| 符号 | `symbol_resolve` R, `symbol_list` R |
| 脚本 | `lua_execute` W（受 `HAVE_LUAJIT` 约束） |

当前二元安全模型把 `Write` 定义为需要审批的目标/主机 mutation。规范 `symbol_resolve`/`symbol_list` 不修改目标内存，因此保持 ReadOnly；它们内部的 active-table session mutation 由 transaction + epoch 约束。默认 prompt 已只列规范名称且不再错误声称需要写审批。33 个旧名称已经退役，不在注册表中；旧会话中的完整调用组由 `getMessagesForRequest()` 转为不可执行的 assistant 历史文本。未来扩展 effect 元数据时应把 symbol 操作标为 `SessionMutation`，但不重新暴露 `symbol_init` 前置步骤。

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

二十三个非 Lua canonical 工具（`status`、`driver_initialize`、`process_list`、`process_open`、module/pointer/disassembly resolution、四个 canonical scan、两个 canonical symbol、五个 canonical breakpoint、raw/typed memory read/write）在 service 边界消费 `OperationContext`。driver 初始化区分未发送、服务端拒绝、发送后未知及确认后 cancel/deadline，且卡密不会进入审批显示、tool audit 或 session JSON。pointer、scan 和 symbol 保持各自事务/epoch 语义；breakpoint mutation 统一区分未发送、设备拒绝、发送后未知和确认后 cancel/deadline，hit batch 在 Agent 边界限制为最新 100 项并报告丢弃数。`lua_execute` 在 host 执行前复核 target/generation，并以任务 absolute deadline 限制 Lua hook timeout；开始后不能硬取消。内置目录已无 legacy executor，新增 process-bound 工具仍必须在实际 service/host/send 边界消费相同 context。

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

canonical scan 也使用该 gate 和独立 domain mutex。`scan_start` 在一个 MAIN transaction 内发送 range 与 start；每次 scan mutation（包括 IPC 旧入口和 DEBUG stop）推进单调 epoch。`scan_refine`、`scan_results`、`scan_clear` 校验 `{target, scanEpoch}`；结果页把 count+page 放在同一事务，clear 发送后再以 count=0 确认。已发送但没有 terminal count 的 start/refine 返回 `completion_unknown`。GUI `ScanWindow` 注入同一个 `IMemService`，start/refine/results/clear/remove 均绑定 target 与 epoch；remove 在一个 transaction 内去重地址、确认前后 count 并要求 epoch 前进。Stop 只设置共享 token，由 system backend 的 progress callback 发送一次 DEBUG stop。

canonical symbol 使用独立 domain mutex 和 MAIN transaction。`symbol_resolve`/`symbol_list` 在一个事务内完成 module 唯一匹配、`SymbolInit` 与 find/page；每个 init 在已持有 transaction gate 后推进单调 epoch。续页必须带上一页 epoch，IPC 的 init 会使其失效。GUI 的 `loadSymbolTable` 在同一个 transaction 内只 init 一次，并循环读取全部 1000-item protocol page；总量限制为 1,000,000 项和 64 MiB 名称。完整 target snapshot 仍在释放 transaction 后复核，以保持 connection -> process -> domain -> port 锁顺序。

`AppContext::ModuleCache` 只保留 GUI presentation cache 职责。`MemoryViewerWindow`/`BreakpointWindow` 显式传入注入的 `IMemService`；cache miss 不持 cache mutex 做网络 I/O，返回后在 cache mutex 内复核完整 target 与当前 module，再原子安装排序后的 symbol list。这样进程切换先 invalidate 后不会被旧加载结果重新污染。

canonical breakpoint 使用 service domain mutex，单条 mutation 的设备确认和本地 cleanup tracker 更新处于同一 MAIN transaction。`ClearTrackedKernelBreakpoints()` 持 gate 完成 tracker snapshot 和逐项 remove，避免并发 set 落在 cleanup 缝隙；disconnect/reconnect 不向旧 target 发命令，只清本地 tracker。四种 mutation 使用相同 receipt：未发送可重试，已发送无响应为非重试 `completion_unknown`，确认后才报告 completed/cancel/deadline 状态。`BreakpointWindow` 显式注入同一个 `IMemService`，mutation 与 hit refresh 每次捕获 target context。hits 协议没有 offset/cursor，socket 层必须排空一次响应；`ReadKernelBreakpointInfoTail` 按块接收并只保留最新 tail。service DTO 保存 GPR、`orig_x0`、syscall、FPSR/FPCR 和 32 个 128-bit vector registers；GUI 上限 50,000，Agent 上限 100，二者返回 `available/dropped` 而不是虚假 continuation cursor。

当前复合序列包括：

- 旧 IPC 的 `ScanSetRange` -> `ScanValue`/fuzzy/hex scan
- 旧 IPC 的 `SymbolInit` -> `SymbolGetList`
- `AppContext::selectProcess()` 的清理/open/set PID/cache 流程

canonical pointer/scan/symbol 与 GUI scan/symbol cache 已迁移；旧 IPC scan/symbol 和 process selection 分步流程仍可能被插入。旧 mutation 会使 native epoch 失效，但旧调用自身仍没有 canonical completion/session 契约。新增复合工具时应增加高层事务锁、revision/epoch 校验，或把操作下沉为服务端单命令。持有 transaction gate 时不得再获取 `AppContext` 的 process-state mutex，避免与 process -> command 的既有锁顺序反转。

### 8.3 地址语义

内置 Agent 的 raw/typed memory、scan ranges、disassembly、`pointer_resolve` offsets 和 breakpoint 地址均只接受带 `0x` 前缀的字符串；退役 alias 不可执行。HTTP IPC 对无前缀字符串仍按十进制解析，因此跨前端继续只使用明确的 `0x` 地址字符串。`memory_write_value` 的 qword 参数和 breakpoint hit/register 值应使用字符串，避免 JSON/模型链路损失 64-bit 精度。

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

## 10. IPC 迁移状态

### 10.1 Native framing、Hello、request session 与 transport 基础

`ipc/IpcProtocol.*` 定义与 transport 无关的帧编解码。默认测试构建独立验证它；显式 `ENABLE_NATIVE_IPC=ON` 时 codec 和 `NativeIpcTransport` 链入 `ImGuiProject`，但 `main.cpp` 不创建 server。header 固定 24 bytes，并逐字段按 little endian 编码，不发送 C++ struct 内存：

| Offset | 字段 |
|--------|------|
| 0 | magic `uint32`，wire bytes 为 `AMEM` |
| 4 | major `uint16`，当前为 1 |
| 6 | minor `uint16`，当前为 0；版本必须精确匹配 `1.0` |
| 8 | message type `uint16` |
| 10 | flags `uint16`，当前必须为 0 |
| 12 | request id `uint64` |
| 20 | payload length `uint32` |

消息类型为 `Hello`、`HelloAck`、`Request`、`Response`、`Cancel` 和 `Error`。handshake 的 request id 必须为 0；request/response/cancel 必须非零；error 可为 0 或非零。request payload 硬上限为 1 MiB，其余帧为 4 MiB，调用方传入更大配置也不能抬高协议上限。decoder 在读取 payload 前验证 magic、精确版本、type、flags、id 和 length；payload 必须是合法 UTF-8。partial header/payload 返回 `NeedMoreData` 且不消费输入，一次 decode 只消费一帧。

`IpcFramedConnection` 先精确读取 24-byte header，并通过 `DecodeHeader()` 在 payload 分配前完成 header 校验；随后才按已验证长度分配和精确读取 payload。读写使用 overlapped I/O、同一个绝对 deadline 和 server stop event，覆盖碎片化读取与 short write。partial header/payload 后关闭属于 protocol error，Stop 与 deadline 分别返回 cancelled/timed-out 状态。

`IpcHandshakeSession` 要求第一帧是 request id 0 的 `Hello`，并把 Hello JSON 限制为 16 KiB。`client_name` 必填、1..128 bytes 且不允许 ASCII control；可选 `client_version` 最大 64 bytes；`requested_capabilities` 最多四项，只接受无重复的 `Observe`、`TargetSelection`、`TargetMutation`、`HostExecution`。`HelloAck` 仅授予客户端实际请求的 `Observe`，请求到的三种 privileged capability 明确进入 denied list。错误首帧类型或 JSON/schema/capability 会收到 structured `Error`；不支持的 frame version 或超限 header 直接关闭。拒绝路径只做最长 1 秒的 bounded drain，不用阻塞式 `FlushFileBuffers`。

`IpcRequestProtocol` 固定 Request JSON 为 `{method, params, timeout_ms?}`。`method` 必填、1..128 bytes 且无 ASCII control，`params` 必须是 object，unknown fields 直接拒绝；`timeout_ms` 默认 30 秒、范围 1..300000。Cancel payload 必须是空 object `{}`。Response 使用 `ok`、统一 `completion` 和 `result`/`error` envelope；dispatcher 返回的 JSON、error token 和最终 payload 在写入前再次验证并受 4 MiB 上限约束。

`IpcRequestSession` 在 `HelloAck` 后维持 reader loop，同一连接只允许一个 active request；第二个并发请求返回 `session_busy`。request id 在连接生命周期内只能使用一次，最多记住 1024 个 unique id，达到上限后返回 `session_request_limit` 并要求 reconnect，避免无界去重集合。method 所需 capability 由 server-owned `IIpcRequestDispatcher::resolveCapability()` 决定，client payload 不能自报；当前 Hello 只 grant Observe，因此 privileged method 在 fake dispatcher 边界也会在执行前被拒绝。

handler/caller thread 持续读取 Request/Cancel，一个 owned joinable worker 串行调用 dispatcher。相对 `timeout_ms` 在接收时固定为 `steady_clock` absolute deadline；deadline、client Cancel、session invalidation 和 server Stop 向同一个 cancellation context 发信号。Cancel 没有独立成功 ack，active request 的最终 Response 承载真实 completion；unknown/invalid Cancel 返回 Error。取消仍是 cooperative，dispatcher 必须观察 context，不能据此声称已发送的设备操作被撤回。session idle timeout 为 5 分钟，response write timeout 为 5 秒，dispatcher validity 默认每 250 ms 复核。

`IpcMethodCatalog` 与内置 Agent 的 24 个 canonical name 由 `native_agent_catalog` 同时校验。分类固定为 12 Observe、1 TargetSelection、9 TargetMutation、2 HostExecution；target policy 为 3 None、20 Bound、1 Selection。只有 12 个 Observe descriptor 标记为 `executableWithoutApproval`；`driver_initialize` 与 Lua 归入 HostExecution，scan mutation 与 breakpoint/memory write 归入 TargetMutation。

原 `AgentMemTools` 实现已提升为 `mem/MemJsonTools`，AI 保留 type alias。`IpcMemServiceDispatcher` 复用这一单一 parser/result adapter 执行 12 个 Observe method，不复制地址、scalar、分页或结果格式。dispatcher 建立时用 `IMemService::captureContext(true)` 固定 `{connectionGeneration, target}`，在 request 前后和 reader polling 边界比较当前 snapshot；变化时发送 request-id 0 的 session Error、以 `SessionInvalidated` signal 取消 active `OperationContext` 并 join worker。deadline 和 cancellation 通过同一个原子 token 进入 service。所有 privileged method 即使直接调用 dispatcher 也在参数解析/service 前返回 `approval_required`。

`NativePipeSecurity` 生成 protected DACL，仅向当前进程用户 SID 和 SYSTEM 授予 pipe read/write，不授予 owner/DACL 修改权。`NamedPipeServer` 固定 `\\.\pipe\AMem.NativeAgent.v1`，使用 `PIPE_REJECT_REMOTE_CLIENTS`、`FILE_FLAG_FIRST_PIPE_INSTANCE` 和 `nMaxInstances=1`；同一个 server handle 在连接间复用。overlapped accept 与 client handler 串行运行在一个 owned thread 上，`stop()` 先发 stop event、对活动 handle 调用 `CancelIoEx`，再 join。状态快照提供 stopped/listening/connected/stopping/failed、累计连接数、名称和错误。

尚未实现产品 runtime composition（server handler 依次装配 handshake/dispatcher/session）、GUI enable/status、privileged capability grant 和 approval broker。`ENABLE_NATIVE_IPC` 默认 OFF，`native_agent_native_ipc_gate` 还明确禁止 main 自动启动。因此 native IPC 当前不可用，也没有新的外部 target mutation 路径。

### 10.2 Legacy HTTP 协议

默认构建不包含 `IpcServer.cpp`。只有 `ENABLE_LEGACY_HTTP_IPC=ON` 时，`IpcServer` 才监听 `127.0.0.1:28100` 并接受：

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

### 10.3 Python MCP 已删除

FastMCP package、`.mcp.json`、安装元数据和 IDE 配置已经从 `NativeAgent` 删除。`tools/protocol_reference/amem_client.py` 仅是可选的 Android wire-protocol 排障脚本，不连接 GUI IPC，不参与产品构建，也不能作为新的 Agent adapter。

当前两个可调用面仍不一一对应：

| 入口 | 静态名称数 | 说明 |
|------|------------|------|
| 内置 Agent（LuaJIT） | 24 个广告定义 / 24 个可执行名称 | 0 个 hidden alias；退役调用仅保留为历史文本 |
| 内置 Agent（无 LuaJIT） | 23 个广告定义 / 23 个可执行名称 | 0 个 hidden alias；`lua_execute` 不注册 |
| Opt-in HTTP IPC | 29 | 原始 C++ handler；不经过 Agent 审批、target context 或统一结果契约 |

内置 Agent 另有 `disassemble`、`symbol_resolve`、`breakpoint_hits` 等规范名称。无 LuaJIT 时内置 Agent 不注册 `lua_execute`，而 HTTP IPC 的 feature-gate 和结果行为仍不同。新增能力时不能只验证“socket 命令存在”，需要 capability/feature-gate 契约。

### 10.4 当前 IPC 安全边界

默认关闭已移除标准构建的监听面。显式启用时，IPC 只绑定 loopback，但没有认证，并返回 `Access-Control-Allow-Origin: *`，还接受浏览器 OPTIONS；任意可访问该端口的本地客户端都能绕过内置 Agent 的写审批。因此不要新增 IPC 能力；替换或删除它之前，必须先考虑鉴权、浏览器访问和 capability。

外部 client timeout 只结束调用方等待，不会停止 detached C++ handler。没有 server request id/cancellation 前，不得建议自动重试；retry-safe 还必须包含“旧请求继续运行也不会破坏共享状态/资源”的判断。

## 11. 维护不变量与当前缺口

### 必须保持的不变量

1. 后台线程不直接访问 ImGui/run 状态；内置 Agent 通过 `UIMessageQueue` 回主线程。
2. 每个 socket request-response 使用 `SocketCommand::execute*` 或等价端口锁。
3. provider 请求使用 `ChatSession::getMessagesForRequest()` 保持 tool use/result 配对。
4. 会修改目标的内置工具不得误标为 ReadOnly。
5. 地址跨前端传递时使用 `0x` 前缀。
6. 工具、IPC、provider 和持久化入口都要在分配前验证不可信长度。
7. provider streaming 成功必须有合法终止事件，partial content 不进入工具执行。
8. socket timeout、EOF 或 partial I/O 必须 poison 当前 `DeviceSession` generation，旧连接在显式重连前不得复用。
9. IPC header 必须逐字段编码；版本、flags、request id、UTF-8 和不可放宽的 payload 上限在 codec 边界验证。

### 尚未满足、不能假定成立的目标

1. 应用 shutdown 返回时 HTTP/IPC 后台任务均已退出。
2. 端口锁能让复合业务操作成为事务。
3. loopback IPC 等同于已鉴权。
4. 所有 AI 持久化文件都严格原子且损坏时不覆盖。
5. ReadOnly 一定没有共享状态变化。
6. 真实三端口 transport 已覆盖 timeout、partial I/O、迟到字节和 reconnect generation。
7. provider `maxContextTokens` 会自动限制实际请求。
8. 内置 Agent 与临时 IPC 暴露相同能力和结果契约。
9. Native IPC 基础已经提供 product runtime composition、privileged approval 或 GUI enable/status。

## 12. 扩展检查单

### 新增设备工具

1. 在 `socket/*Commands.cpp` 实现并在 `client_singleton.h` 声明，优先使用 `SocketCommand::execute*`。
2. 明确它绑定哪个资源：process、scan session、symbol table、breakpoint 或全局 driver。
3. 判断是 target mutation、stateful read 还是纯 read，并同步默认 prompt。
4. 在 `ToolDefinitions.cpp` 添加 schema、局部长度上限和 executor。
5. 不向旧 HTTP IPC 增加方法；若决定保留外部自动化，按计划通过受限 Named Pipe adapter 暴露 `MemService` 契约。
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

当前无设备 CTest `native_agent_mem_service` 的 23 个测试组覆盖地址/scalar codec、driver receipt/card redaction、进程与模块分页/解析、事务化 pointer resolution、disassembly、scan/symbol session/full-table transaction、breakpoint receipt/rich hit batch、scan 取消/完成未知、mutation audit 脱敏/轮转/晚到 callback 前持久化、service/adapter、raw/typed write 完成语义、target/generation、连接 lease/poison、审批期间切换/重连、同批 target 推进、非目标工具、队列取消/timeout、active cancellation、shutdown join、晚到结果拒绝和退役工具历史降级。Native IPC 另有 5 组 protocol、5 组 transport、7 组 framed-I/O、8 组 handshake、6 组 request-contract、9 组 request-session、4 组 method-catalog 与 5 组 MemService-dispatcher 测试。新增覆盖完整 24-name classification、12 Observe service mapping、privileged direct-call denial、error/retryable normalization、baseline connection/target invalidation 和 cancellation/deadline propagation。连同四个静态 gate，Debug/Release 当前各有 13 项 CTest。以下路径仍缺测试：

- provider SSE/full-response 解析和完整终止验证
- ChatSession 通用工具配对与预算裁剪
- config/index 损坏恢复
- AgentRunner 预算上限、auto approve 和 denial 的完整组合
- ToolExecutor schema 和错误契约
- legacy IPC HTTP parser/auth/sendAll
- Native IPC runtime composition、真实 GUI enable/teardown、privileged approval broker 与 target-bound approval invalidation
- 不同 Windows 用户/session 与真实 remote client 的负向身份测试
- fake transport partial I/O、迟到响应和三端口 reconnect
- C++ Agent/IPC 名称、结果和 feature gate 对齐

真实 Android 设备测试保留给协议兼容、驱动和硬件断点 smoke test。
