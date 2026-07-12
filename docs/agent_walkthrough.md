# AMem AI Agent 代码走读

适用分支：`NativeAgent`（基线来自 `AIChat`）
最后更新：2026-07-13

本文按实际调用顺序解释内置 AI Chat 如何启动、请求模型、审批并执行工具、回喂结果、取消和退出。组件清单见 [`agent_architecture.md`](./agent_architecture.md)，当前问题编号见 [`agent_project_issues.md`](./agent_project_issues.md)，目标重构步骤见 [`native_agent_refactor_plan.md`](./native_agent_refactor_plan.md)。

代码定位以函数名为主。行号会随提交变化，不应作为维护文档的稳定锚点。

## 1. 启动阶段：第一条消息之前发生什么

入口是 `ChatWindow::ChatWindow()`。

### 1.1 注册组件

构造函数调用幂等初始化：

- `ProviderRegistry::initBuiltinProviders()`
- `ToolExecutor::initBuiltinTools()`

当前 provider 为 Claude、OpenAI-compatible、DeepSeek。LuaJIT 构建的工具注册表包含 24 个 canonical 名称，全部可执行且全部发送给 provider，没有 hidden alias；无 LuaJIT 时不注册 `lua_execute`，其余 23 个名称保持一致。

### 1.2 加载 provider 配置

启动顺序：

```text
legacy ai_config.dat rename
  -> ApiKeyStore::loadFromFile("ai_config.json")
  -> only Missing: seedDefaultsIfEmpty() + saveToFile("ai_config.json")
  -> Loaded: commit validated temporary configs
  -> Invalid/IoError: preserve file and prior in-memory state
  -> configure live providers
```

API key 在文件中是 DPAPI 密文，endpoint/model 是明文。

`loadFromFile()` 返回统一的 `PersistenceLoadResult`。解析和字段类型/范围校验都在临时对象中完成，只有 `Loaded` 才提交；只有 `Missing` 才播种并保存默认配置。`Invalid`/`IoError` 会记录错误并保留原文件及旧内存状态，不再从启动路径抛出字段类型异常。A-04 已关闭。

设置 UI 还有相反方向的问题：已保存 provider key 无法通过清空输入框删除。key 为空时 Save 会跳过该 provider，`ApiKeyStore::removeConfig()` 没有 UI 入口。实现“Forget provider”时需要显式删除并清零明文 edit buffer。

### 1.3 加载全局设置

`AiSettings::loadOrDefault("ai_settings.json")` 读取：

- active provider
- system prompt
- proxy
- tool timeout
- agent step/call budget
- token limit
- auto-approve writes

然后构造函数把 snapshot 应用到：

- `ToolExecutor`
- `HttpClient`
- `ChatSession`
- `ChatWindow` 的预算和代理编辑字段

`loadOrDefault()` 同样返回 `Loaded`/`Missing`/`Invalid`/`IoError`，完整校验临时 snapshot 后提交；只有真正缺失时才写默认文件，损坏或 I/O 失败不会覆盖原文件。

### 1.4 初始化会话

```text
SessionManager::init("ai_sessions", "ai_session.json")
  -> load and validate index into temporary state
  -> Invalid: preserve index as .corrupt* and scan valid session JSON
  -> preservation failure/IoError: disable automatic index write-back
  -> optional legacy migration
  -> choose/create active id
  -> ChatSession::load(active session file)
```

注意顺序：全局 prompt/token 已先写进 `session_`，随后 `ChatSession::load()` 又从会话文件读取同名字段。因此已有会话值会覆盖全局值。切换会话也一样；新会话只清消息，会继承之前留在 `session_` 的值。见 A-12。

会话加载也先校验临时状态。损坏活动会话不会绑定到 `ChatSession`，退出、切换或下一条消息调用 `saveBound()` 时不会覆盖它；窗口会保留原文件并创建新的可写会话。所有会话写入复用 `utils::installTempFile()`。

## 2. 用户发送消息

### 2.1 建立新 run

发送动作在主线程完成：

1. 把 user message 加入 `ChatSession`，并自动持久化。
2. `AgentController::resetForNewRun()` 生成新的 run id。
3. 清空流式内容和旧 active id。
4. 调用 `ChatSession::getMessagesForRequest()`。
5. 进入 `ChatWindow::dispatchAgentRequest()`。

`getMessagesForRequest()` 是 provider 协议正确性的关键边界。它只输出完整匹配的 assistant tool calls 和 tool results，过滤孤儿/重复 id 和运行时 system notice。若完整调用组包含 33 个已退役名称中的任意一个，整组会转为普通 assistant 文本，保留脱敏参数和已记录结果，但不再发送 tool protocol，也不会重新执行。

### 2.2 构造 provider 请求

`AgentController::dispatchModelRequest()`：

1. 从 `ProviderRegistry` 找当前 provider。
2. 校验 API key、model 和 endpoint。
3. 取 `ToolExecutor::getToolDefinitions()`。
4. 构造 `CompletionRequest`。
5. 调 `provider->sendCompletion()`。
6. 把 run 标为 `WaitingModel`。

endpoint 校验目前只要求 `https://`。它不会验证域名归属。特别是 OpenAI provider 的默认值是第三方 `https://ai.ikik.net/v1`，发送前要把 endpoint 当作显式信任决策。

`CompletionRequest` 仍只带 run id，不直接序列化 PID/handle/revision；同一 `AgentController` 的 `AgentRunContext` 在首轮请求前捕获 connection generation 和 target snapshot。后续审批、工具出队和结果回收都使用该 context，run id 只负责异步消息隔离。

provider 的 `getCapabilities().maxContextTokens` 当前没有参与这里的请求构造。会话 token limit 只按消息字节数/4裁剪，也没有计入当前 24 个广告工具 schema 和输出预留，所以 UI 显示“未超限”不代表实际 provider context 一定可接受。

## 3. HTTP 和 SSE 后台路径

### 3.1 Provider 适配

各 provider 在调用线程把通用结构转成各家 JSON：

- system message 的位置
- assistant tool calls
- tool result role/block
- model 和 stream 参数

之后调用 `HttpClient::postAsync()`。

### 3.2 HTTP worker

`postAsync()`：

1. 注册 request id/cancellation token，增加 `inFlight_`。
2. 复制 timeout/proxy 配置。
3. 创建 detached worker。
4. cpp-httplib 发 HTTPS POST。
5. `content_receiver` 累积原始响应，并把数据喂给 `SSEParser`。
6. provider callback 拼接 content/tool calls。
7. 完成时通过请求 callback 向 `UIMessageQueue` 投递。

后台线程不直接调用 ImGui，这是正确边界。

当前没有以下总量限制：

- 单 SSE 行/事件
- HTTP 累计响应
- assistant content
- tool argument fragments

局部 tool call 数量和 arguments 上限是在结果进入 `ChatWindow` 后才校验，不能替代网络层上限。

### 3.3 HTTP 完成计数的时序

每条结束路径当前是：

```text
removeFromActive()
  -> inFlight_--
  -> completeSafely()
  -> provider callback
  -> UIMessageQueue::push()
```

所以 `HttpClient::shutdown()` 观察到 `inFlight_ == 0` 时，最后一个完成回调可能仍在运行。这是 A-05 的具体来源。

provider 完成条件在独立状态机中统一：

- `SSEParser` 按分片/行拼接 `data:`，把 `[DONE]` 交给回调，并在 EOF flush 最后一个未闭合事件。
- Claude event 先经 `InspectClaudeStreamEvent()`，要求 `message_start` 与 `message_stop`。
- OpenAI/DeepSeek event 先经 `InspectOpenAICompatibleStreamEvent()`，要求合法 `choices` 起始，并接受非空 `finish_reason` 或 `[DONE]` 作为 terminal。
- `StreamTerminalTracker` 保留首个 malformed/schema error；重复 terminal 是幂等的。

HTTP 2xx 正常关闭但 SSE 截断、malformed 或完全不是 SSE 时，完成回调返回 `InvalidResponse`。partial 文本/tool arguments 仍在错误响应中，`ChatWindow` 会显示错误并在 tool-call 处理前退出，因此不会执行。

## 4. 主线程消费模型结果

`ChatWindow::pollMessages()` 每帧 `tryPop()`。

### 4.1 runId 过滤

- `Token`/`Completion`/`Error` 对比 `activeDispatchRunId_`。
- `ToolResult` 对比 `activeToolRunId_`。
- id 为空或不匹配的迟到消息直接丢弃。

这能防止旧响应写进新会话，但不会取消旧网络请求或工具副作用。

### 4.2 流式文本

`Token` 追加到 `streamingContent_`。取消时，已收到的部分内容会作为 assistant message 保存，再追加 `[cancelled]` system notice。

`streamingContent_` 当前没有硬字节上限。

### 4.3 完成响应

`Completion` 分为：

- `ProviderError`：交给 `displayErrorForCategory()`。
- 纯文本：保存 assistant message，run 完成。
- 带 tool calls：先由 `validateAndNormalizeToolCalls()` 校验，再进入 `processToolCalls()`。

当前入口限制：

- 最多 64 个 tool calls
- id 最多 256 字节
- name 最多 64 字节
- 单个 arguments 最多 512 KiB

这些是有用的最后防线，但 provider/HTTP 仍应更早拒绝超大输入。

## 5. 工具审批和执行

### 5.1 AgentRunner 开始一批工具

```text
ChatWindow::processToolCalls()
  -> AgentController::beginToolCalls()
  -> AgentRunner::beginToolCalls()
  -> AgentRunner::runUntilBlocked()
```

`beginToolCalls()` 增加 step，应用 `maxAgentSteps` 和 `maxToolCallsPerTurn`。

`runUntilBlocked()` 查 `ToolExecutor::getToolSafety(name)`：

- `ReadOnly`：返回 `NeedsExecution`。
- `Write` + auto approve：记录 trace 后返回 `NeedsExecution`。
- `Write` + 默认策略：返回 `NeedsConfirmation`。

### 5.2 审批框

`ChatWindow::drawToolConfirmationModal()` 展示：

- tool name
- pretty-printed arguments
- expected connection generation
- expected PID 和 process revision
- Approve/Deny

`AgentController` 按 `ToolTargetPolicy` 处理目标：

- `None`：只要求 connection generation 不变。
- `Bound`：要求 PID、handle、revision 和 generation 全部不变。
- `Selection`：审批与 send 前绑定旧 selection，成功后验证并推进新 target。

二十三个已迁移 canonical 工具会把 run 的 `OperationContext` 直接传入 `MemService`。例如 `process_open` 的安全路径是：

```text
模型请求切到进程 B
  -> 等待用户审批
  -> 校验审批时的旧 target A
  -> 用户批准
  -> send 前再次校验 A
  -> open B 返回新 snapshot
  -> 当前状态仍等于返回 snapshot 时才更新 run
```

driver、module/pointer/disassembly/symbol resolution、四个 canonical scan、五个 canonical breakpoint 和 raw/typed memory read/write 都已在 service 边界消费 context。`driver_initialize` 返回未发送/拒绝/完成未知/确认后取消或超时回执，并在显示、审计和持久化时脱敏 card。pointer/scan/symbol 保持 transaction/epoch 语义；breakpoint set/remove/suspend/resume 返回确认回执或 `completion_unknown`，hits 返回最新最多 100 项以及 `available/dropped`，没有 continuation cursor。旧名称已经从注册表和 JSON adapter 中删除。`lua_execute` 在 host 边界复核 target，使用 Agent absolute deadline，并只把开始后的取消作为回执标记。

GUI 的 `BreakpointWindow` 已完成 breakpoint 域迁移：窗口构造时注入 `IMemService`，添加、删除、启用、暂停、恢复和命中刷新均捕获当前 target/generation。命中读取在 DEBUG 端口排空一次无 cursor 响应，只保留最新 50,000 条；`BreakpointHit` 保存详情页所需的 GPR、`orig_x0`、syscall、FPSR/FPCR 和全部向量寄存器。超过上限时 GUI 显示丢弃较早命中的日志。

GUI symbol cache 也不再直接编排 `SymbolInit -> SymbolGetList`。`MemoryViewerWindow` 与 `BreakpointWindow` 把注入的 `IMemService` 传给 `ModuleCache`；cache miss 调用 `loadSymbolTable`，在一个 MAIN transaction 中完成 module 唯一匹配、一次 init 和全部页读取。service 限制 1,000,000 项/64 MiB 名称并在释放 transaction 后复核 target+epoch；cache 随后在自身 mutex 内再次复核 target 与 module，才安装排序结果。

### 5.3 受管工具队列

批准或只读工具进入 `ChatWindow::startToolExecution()`：

```text
ChatWindow
  -> AgentTaskExecutor bounded queue (captures absolute deadline)
  -> one owned, joinable worker
       -> reject cancelled/expired-before-start task
       -> synchronous ToolExecutor::execute(call, runOperationContext)
            -> parse JSON and validate schema
            -> SocketIoTimeout::ScopedTimeout(deadline)
            -> MemService or Lua host boundary
       -> AgentMutationAuditLog(write/session effect, before callback)
       -> completion callback
  -> UIMessageQueue(ToolResult)
```

队列最多保留 64 个任务，并按提交顺序串行执行。`ToolExecutor` 不再创建额外线程，因此 active task、completion callback 和 worker 都处于同一生命周期；`shutdown()` 返回即证明 worker 已退出。

排队取消或超时不会进入 executor，分别返回 `cancelled_before_start` 或 `timed_out_before_start`。活动任务使用同一个 cancellation token 和绝对 deadline；已迁移 service 会在 send 前、I/O 期间和结果规范化时观察它们。

worker 不会强杀正在运行的 C++ 调用。只读操作在 deadline 后才返回时会归一为 `timed_out`；写操作保留 `completed_after_deadline`、`completed_after_cancel_request` 或 `completion_unknown`，避免把已经发送的副作用误报为未执行。write-classified 和 symbol-session outcome 在 callback 前写独立审计，因此 callback 被 stale run gate 丢弃或抛异常也不会删除最终状态。当前注册的 service/host executor 都消费 context，但阻塞中的设备或 Lua 调用仍可能直到自身检查点、socket deadline 或函数返回才响应取消。

### 5.4 socket 层

executor 调 `client_singleton.h` 中的命令。现代命令应使用 `SocketCommand::execute*`：

```text
acquire shared DeviceSession request lease
  -> acquire recursive per-port transaction gate
  -> EnsureOpenHandle (when required)
  -> acquire request-response port mutex
  -> verify lease generation is current
  -> send/receive one command
```

connect/disconnect/reconnect 持有 exclusive lifecycle lease；I/O 错误、EOF 或 partial failure 会 poison session、推进 generation 并拒绝新请求，已删除用待处理字节尝试恢复协议同步的旧路径。

普通命令只短暂持有 transaction gate。`pointer_resolve`/旧 `ResolveModuleOffsetChain()` 会把同一个 gate 保持到模块查询和全部 pointer read 结束，因此其他 caller 不能插入；规范 service 在 gate 外做完整 target snapshot 校验，在 gate 内只做稳定 revision 和无 target-lock 的 generation/cancel 检查。

canonical scan 另持有 scan domain mutex 和 MAIN transaction gate：start 覆盖 set-range+scan，results 覆盖 count+page，clear 用后续 count=0 确认。GUI `ScanWindow` 使用注入的同一 `IMemService`：首次/再次扫描捕获 target context，结果分页与 clear 携带最新 epoch，选中删除在单个 transaction 内确认 count 与新 epoch。长扫描的 progress callback 同时转发 GUI 进度并观察 cancellation token；GUI Stop 只设置 token，backend 经 DEBUG 端口请求一次 stop。若主命令仍返回 terminal count，结果标为 `completed_after_cancel_request`，否则 sent request 保留 `completion_unknown`。

canonical symbol 另持有 symbol domain mutex 和 MAIN transaction gate。`symbol_resolve`/`symbol_list` 都在 gate 内完成 module 唯一匹配、`SymbolInit` 和 find/page；init 取得 gate 后才推进 epoch，保证 epoch 顺序与实际 mutation 顺序一致。offset > 0 的 list 必须携带上一页 epoch，跨前端 init 会返回 `symbol_session_changed`。

canonical breakpoint mutation 在同一个 MAIN transaction 内接收设备确认并更新 cleanup tracker。cleanup 持 gate 完成 snapshot + remove；断线只清本地 tracker，避免把旧地址施加到新 target。若 request 已开始但未收到响应，service 返回非重试 `completion_unknown`；确认完成后即使 cancellation/deadline 已到，也保留 `completed_after_*`。hits socket 响应按块读取并丢弃页外数据，Agent 的寄存器和时间使用字符串避免 64 位精度损失。

以下序列仍不是事务：

- 旧 IPC 的 `ScanSetRange` -> scan command
- `SymbolInit` -> `SymbolGetList`
- `AppContext::selectProcess()` 的多步清理/open/set PID

另一个 GUI/Agent/Native IPC caller 可在两条命令之间插入。

### 5.5 工具结果

executor 返回 JSON 字符串。`ToolExecutor::extractToolError()` 会识别：

- 顶层非空 `error`
- `success: false`

`AgentRunner::completeToolExecution()` 校验结果是否匹配当前 pending call，然后：

- 成功：写 tool audit，继续下一工具。
- 失败：写失败 audit，本批剩余工具标 skipped。
- 批次结束：`ReadyForFollowUp`。

`AgentRunner::makeToolMessage()` 通常会把 arguments 和 result/details 完整写入会话。driver card 是当前例外：原始参数仅在进程内用于执行和当前 tool-call 连续性，审批、审计和 `ChatSession` JSON 使用 `[REDACTED]`。Lua、地址、内存和其他工具数据仍会明文落盘并可能在下一次模型请求中发送到 provider。

## 6. 回喂模型

`ChatWindow::sendFollowUpAfterTools()` 再次调用：

```text
session_.getMessagesForRequest()
  -> dispatchAgentRequest()
  -> provider
```

历史此时包含：

```text
assistant(tool calls)
tool(result for call 1)
tool(result for call 2)
...
```

循环直到：

- 模型返回纯文本。
- provider 失败。
- 用户停止。
- 达到 agent step limit。

## 7. Stop、超时和退出不是同一件事

### 7.1 用户 Stop

`ChatWindow::cancelRequest()`：

1. 设置 HTTP token。
2. 调用 `AgentTaskExecutor::cancelRun()`，标记当前 active/queued task 的 token。
3. 活动 mutation 立即显示 cancel requested，而不是声称已取消。
4. 保存部分流式内容。
5. 记录 cancelled notice/trace。
6. 把 UI 和 run 恢复到 Idle。

排队任务不会再执行；已迁移的 active service 会观察取消，但已经发送到设备的写命令不能撤回。迟到工具结果仍不会进入旧会话/trace，但 `AgentTaskExecutor` 会先把 approval/effect/target/completion 摘要写入 `ai_mutation_audit.jsonl`，Audit 表按新到旧显示最近 100 条。Clear、New、session switch/delete 和窗口析构也通过同一 active-run cancellation 入口。

排查“点 Stop 后设备还是变化”时，先区分任务是否已发送；Stop 是取消请求，不是写操作回滚。

### 7.2 工具 deadline

deadline 在入队时固定，因此队列等待也消耗预算。任务到队首前超时不会执行；活动任务把剩余预算传给 socket lock/I/O。

deadline 不能强行抢占已经阻塞的 C++/Lua 调用。唯一 worker 会继续拥有它直到返回，后续队列和 shutdown 也会等待；不会出现未登记的后台 executor，但响应速度仍取决于底层命令何时到达 cancellation/deadline 检查点。

`WindowsSocketClient` 现在把 timeout、EOF 和其他 I/O 失败视为协议可能失步：

```text
old recv times out
  -> mark DeviceSession poisoned
  -> advance connection generation
  -> close the failed client
  -> reject subsequent request leases
  -> explicit reconnect under lifecycle exclusive lock
```

这会阻止旧响应被下一请求消费，但不会自动恢复 driver、process、scan 或 breakpoint 状态。是否需要重连应结合 `status.connection_poisoned` 和当前 generation 判断。

### 7.3 应用退出

主退出顺序：

```text
ShutdownSystemNativeAgentRuntime()
  -> cancel approvals / stop pipe / join handler and request worker
  -> HttpClient::shutdown()
  -> AgentTaskExecutor::shutdown() and join
  -> DisconnectMultiPort()
  -> ImGui teardown
```

当前边界：

- Native IPC Stop 会取消审批与活动请求，并 join pipe handler 和 request worker。
- HTTP 最后 callback 不在 `inFlight_` 计数内。
- HTTP 只等待 3 秒，worker 仍是 detached。
- Agent 工具 worker 会取消 queued/active task，并等待 active executor 返回后 join。

因此工具与 Native IPC shutdown 已闭环，但整个应用退出仍受 provider HTTP detached task 约束。若调试退出崩溃、静态析构异常或偶发访问，应先检查 HTTP worker/callback；设备 socket 在 Native IPC 与工具 worker join 后才断开。

GUI 的 connect/disconnect/auto-reconnect 现在委托 `MultiPortClientManager`，它通过 `DeviceSession` exclusive lifecycle lease 串行关闭旧端口、清目标状态、连接或回滚 MAIN/DEBUG/ERROR；普通命令持 shared request lease，因此 disconnect 会等待在途请求释放。脚本故障与真实三连接 loopback 已覆盖单端口 poison、全端口重连和 generation 隔离；Android 远端状态恢复仍需设备验证。

## 8. 会话和配置的真实数据流

### 8.1 每次消息持久化

`ChatSession::addMessage()`：

1. 加消息。
2. 超过 1000 条时按完整对话组裁剪。
3. 根据估算 token 数裁剪旧组。
4. 释放锁后调用 `save()`。

最新用户回合即使超 token limit 也会保留。单条巨大消息不会被该策略删除。

普通消息保存只作用于有效绑定。退出和会话切换使用 `saveBound()`，加载失败留下的损坏路径不会被随后写回；新建/缺失会话则建立可写绑定。这个一致性修复没有增加文件总大小、单消息或会话载入分配上限，A-09 仍未关闭。

`estimateTokenCount()` 是 UTF-8 字节数/4 的启发式值，未使用 provider 声明的 64k/128k/200k context，也未计工具定义和输出预算。它适合 UI 粗略提示，不适合作为 provider 请求一定有效的证明。

### 8.2 请求历史和磁盘历史不同

磁盘可包含 runtime system notice、不完整工具组和全部审计；`getMessagesForRequest()` 会在发送前生成清洗副本。不要为了“简化 provider”而直接读取 `getMessages()`。

### 8.3 独立 mutation audit

`ai_mutation_audit.jsonl` 不属于聊天 session。write-classified 工具和 symbol active-table session effect 在 executor callback 前写入；manual deny 和队列拒绝由 UI 边界补写。单条记录最多 64 KiB，active 文件最多 4 MiB并保留一个 `.1` 备份；加载时忽略损坏 JSONL 行。

审计保留 run/tool-call id、approval、effect/resource domain、预期 generation/target、duration、success 和 completion。driver/Lua secret/error、raw memory bytes、bulk output、registers、hits、items 和大字段仅保存 omitted/size 摘要。它仍是明文文件，不替代加密会话或 OS 访问控制。

### 8.4 加载边界

`ChatSession::loadUnlocked()` 先完整解析 JSON，再按 `arr.size()` reserve，最后才裁剪到 1000 条。外部编辑或异常大文件可在裁剪前消耗大量内存。

配置管理器的字段类型校验和损坏恢复也不一致。实现修复时应统一为：

```text
read -> parse temporary -> validate all fields -> commit memory
                               |
                               +-- failure: preserve original and report
```

## 9. IPC 迁移路径

### 9.1 Native framing、Hello、request session 与 transport 基础

`ipc/IpcProtocol.*` 是 transport-independent codec。默认测试构建单独验证它；`ENABLE_NATIVE_IPC=ON` 时 codec 和 native transport 会编入应用，但 `main.cpp` 仍无 start 路径。固定 24-byte header 按 little endian 逐字段编码 `AMEM` magic、精确 `1.0` 版本、message type、零 flags、request id 和 payload length。

decoder 先用完整 header 验证版本/type/flags/id/长度，再等待或复制 payload。request 最大 1 MiB，其他帧最大 4 MiB；更大的调用方 limit 不能抬高硬上限。payload 只接受合法 UTF-8。header 或 payload 不完整时返回 `NeedMoreData`、`consumed=0`；连续帧只消费第一帧。

`IpcFramedConnection::readFrame()` 先精确读取 header，调用 `DecodeHeader()` 后才按合法长度分配 payload；同一个绝对 deadline 覆盖 header 与 payload。overlapped exact transfer 会处理 fragmented read/short write，Stop event 通过 `CancelIoEx` 中断等待。若 validation poll 在 frame 中间超时，已读字节保留到下一次调用；partial header/payload 后断开仍报告 protocol error。request session 发送会导致关闭的 terminal Error 后做最多 100 ms 可取消 drain，避免 server 立即 disconnect 截断客户端尚未读完的 payload。

`IpcHandshakeSession::perform()` 消费首帧并要求 request id 0 的 `Hello`。Hello JSON 上限为 16 KiB，校验必填的 `client_name`、可选 `client_version` 和最多四项且无重复的 `requested_capabilities`。当前状态机只授予客户端请求的 `Observe`，把请求到的 `TargetSelection`、`TargetMutation`、`HostExecution` 放入 denied list。invalid first type/JSON/schema/capability 返回 structured `Error` 并做最长 1 秒 bounded drain；unsupported frame version 或 oversized header 直接关闭。

Hello 建立后的测试调用链是：

```text
handler reader -> IpcRequestSession::run()
  -> strict Request {method, params, timeout_ms?}
  -> lifetime request-id dedupe / one-active / IpcMethodCatalog capability
  -> owned dispatch worker -> IpcMemServiceDispatcher
       -> Observe: shared MemJsonTools -> IMemService(context)
       -> privileged: bounded broker submission -> decision only
  -> validated Response {ok, completion, result|error}
Cancel/deadline/invalidation/Stop -> same cooperative cancellation context
```

`timeout_ms` 默认 30 秒、最大 5 分钟，接收 Request 时转换成 server `steady_clock` absolute deadline。client 不能在 payload 中声明 capability；dispatcher registry 决定 method 需要 Observe、TargetSelection、TargetMutation 或 HostExecution。handshake 只 grant Observe；缺失 privileged capability 的 method 仅在 dispatcher 明确支持 approval submission 时进入 worker，否则在 dispatch 前拒绝。每个连接最多接纳 1024 个 unique request id；id 终身不复用，达到上限后返回 Error 并断开要求重连。Cancel 只接受 `{}`，不单独返回成功 ack；active request 的最终 Response 给出 completion。

完整 catalog 与内置 Agent 同为 24 个 canonical name：12 Observe、1 TargetSelection、9 TargetMutation、2 HostExecution。`MemJsonTools` 是 AI 和 IPC 共用的参数/结果 adapter；IPC 调用不直接读 `AppContext` 或 socket。`IpcMemServiceDispatcher` 在构造时固定 connection/target baseline，并在执行前后及 reader 每 250 ms 复核。generation、PID、handle 或 process revision 变化会发 session-level Error、取消 active context 并关闭 session。没有 broker/session binding 的 dispatcher 对 privileged method 仍返回 `approval_required`；runtime binding 后，request worker 只提交 method/client/session/target/deadline，原 params 留在 active request。reader 可继续处理 Cancel。批准后 dispatcher 消费 durable one-shot grant，再让 11 个 privileged method 进入 `MemJsonTools`/`MemService`；`lua_execute` 则进入注入的 host executor，并复用 `mem/LuaJsonTool`。

直接测试 `NamedPipeServer::start()` 时的调用链是：

```text
NativePipeSecurity -> protected current-user/SYSTEM read-write DACL
  -> CreateNamedPipeW(\\.\pipe\AMem.NativeAgent.v1,
       FIRST_PIPE_INSTANCE, REJECT_REMOTE_CLIENTS, max instances = 1)
  -> owned thread: overlapped ConnectNamedPipe -> serial handler
  -> DisconnectNamedPipe -> reuse the same server handle
Stop -> signal stop event -> CancelIoEx(active pipe) -> join
```

`NamedPipeServer::snapshot()` 可观察 lifecycle state、accepted count、pipe name 和 last error。`NativeAgentRuntime` 为每个 accepted handle 串起 framed connection -> handshake -> `IpcMemServiceDispatcher` -> request session。成功 Hello 获得 server 单调 session id；stop/start 清诊断计数但不复用 id。snapshot 提供 phase、session count/id、活动 client identity/capability、最后状态和 request/response/cancel 计数，不保存 params/result。system owner 保证 audit、broker 和 host executor 都晚于 runtime 析构；窗口提供显式启停。编译与运行默认关闭，`main.cpp` 只在设备断连前 shutdown。handler 与 dispatch worker 都受管并 join。Hello 与 GUI 显示仍明确为 `Observe + 逐请求审批`，不会授予长期 privileged capability。

privileged broker 已接入完整的 submission -> decision -> consume -> execution 产品链。worker 用 `{sessionId, requestId}` 提交 metadata 并保持 active request；Cancel/Stop/session 关闭产生精确终止状态。共享 `IpcApprovalAuditLog` 写 `native_ipc_approval_audit.jsonl`，轮转 `.1`，更新最近 100 条及成功/失败计数。schema 2 把 broker 转换记为 `approval_transition`，把每个 consumed grant 的最终摘要记为 `execution_outcome`；旧 schema 1 仍可加载。日志是明文，含 client/method/session/request、authorized/observed generation/target、success、completion 和 error code，但不含 params、result JSON 或 error message。GUI“特权安全审计”显示最近 20 条。

broker `consume()` 在锁内复核 session/request/deadline/generation/target 并烧毁 record，再在锁外审计；只有 durable success 返回 grant，失败不可重试复用。dispatcher 随后校验 grant metadata，在 adapter/send 前再次检查 Cancel/deadline，并在返回 response 前同步记录一次 outcome。consume 审计失败可阻止执行；outcome 审计发生在可能已发送的 mutation 之后，因此失败只进入可见 health，不覆盖真实 completion。deny、expire 或 consume 前 Cancel 没有 execution outcome。

`process_open` 是特殊的 `Selection` 路径：dispatcher 在 mutex 下暂时允许预期中的目标切换，调用 adapter 后从结构化结果取出新 snapshot，并与 service 当前 snapshot 精确比对；只有 connection generation 和完整 target 都一致时才推进 session baseline。其余方法仍绑定旧 baseline。Native completion 还新增 `timed_out_before_start`、`timed_out`、`completed_after_deadline`，确保“截止时间后收到设备确认”不会被错误表述为未执行。

### 9.2 Python MCP 与 legacy HTTP 已删除

FastMCP package、配置、安装入口和旧 loopback HTTP server 已从 `NativeAgent` 删除。产品不再提供端口式 HTTP 控制面，也没有恢复它的 CMake 选项或 compile macro。`tools/protocol_reference/amem_client.py` 只用于直接排查 Android 二进制协议，不是 GUI IPC client，也不参与 Agent 运行。

### 9.3 Native IPC 与内置 Agent

| 维度 | 内置 Agent | Native IPC |
|------|------------|------------|
| 授权 | `AgentRunner` 写审批 | Hello 仅 Observe；每个 privileged request 使用 GUI one-shot grant |
| 业务 adapter | `MemJsonTools` / `LuaJsonTool` | 同一套 `MemJsonTools` / `LuaJsonTool` |
| 地址字符串 `"1234"` | 拒绝，必须显式 `0x` | 拒绝，必须显式 `0x` |
| 生命周期 | run context + cancellation + owned tool worker | session/request id + cancellation + owned pipe/worker |
| 工具集合 | LuaJIT: 24；无 LuaJIT: 23 | 24-name catalog；Lua host 不可用时返回 feature error |

两条路径共享业务解析和目标语义，但授权主体、wire envelope 和审计日志不同。新增能力时更新 `IpcMethodCatalog` 和静态 capability gate，不复制第二套参数解析或结果 schema。

### 9.4 Native IPC 安全与响应

Native IPC 编译和运行均默认关闭。显式启用后，Named Pipe 使用当前用户/SYSTEM DACL、拒绝 remote client、单实例、严格 Hello、payload 上限、绝对 deadline、request id 和 Cancel。Hello 不授予长期 privileged capability；目标选择、目标修改和 host execution 必须逐请求审批，并在执行前消费 durable one-shot grant。

framed writer 使用 overlapped exact write 处理 short write；Stop 通过 stop event 和 `CancelIoEx` 中断等待并 join owner。客户端 timeout 通过 request deadline/cancellation 进入同一状态机，但不能撤销已发送到 Android 设备的副作用。不同 Windows 用户/session 与真实 remote client 的负向验证仍需补齐。

## 10. 扩展时的检查步骤

### 10.1 新增工具

1. 在协议层实现命令，使用 `SocketCommand::execute*`。
2. 标出资源域：process、scan、symbol、breakpoint、driver 或全局。
3. 判断是否需要 PID/revision、scan epoch 或 symbol epoch。
4. 设计 schema 和输入上限，地址强制 `0x`。
5. 设计输出上限/分页，避免把完整大列表塞给模型。
6. 选择 `ToolSafety`，并同步 `DefaultSystemPrompt.h`。
7. 在 `ToolDefinitions.cpp` 注册。
8. 若向 Native IPC 暴露能力，更新共享 catalog/adapter、capability、target policy 和 approval 测试。
9. 若包含多个设备命令，增加事务锁/revision 或服务端复合命令。
10. 测试 Stop、目标切换、超时和退出。
11. 更新 capability matrix，验证 feature gate 下工具可见性一致。

### 10.2 新增 provider

1. 实现通用消息到 provider JSON 的双向转换。
2. 保持 tool use/result 配对。
3. 所有回调只通过 `UIMessageQueue`。
4. 给原始响应、SSE event、content 和 tool args 设置上限。
5. 缺失合法 stream terminal 时返回 partial `InvalidResponse`，禁止工具执行。
6. 把 provider/model context、工具 schema 和输出预留纳入预算。
7. 校验取消与非 2xx 错误只完成一次。
8. endpoint 默认值明确域名归属和数据去向。
9. 验证 shutdown 时最后 callback 已被计数。

### 10.3 修改会话/设置

1. 先决定字段是 global 还是 per-session。
2. 读取失败不覆盖原文件。
3. 合法 JSON 的错误类型也必须被捕获并报告。
4. 敏感字段在序列化前 redaction。
5. 使用经过 Windows 目标已存在场景验证的原子替换。

### 10.4 维护 Native IPC

1. 先确认鉴权/capability。
2. transport adapter 只映射 `MemService` 参数、错误和上限，不复制业务逻辑。
3. 保持 framed exact read/write、请求/响应硬上限和严格 UTF-8。
4. handler 必须可停止和 join。
5. 保持 Hello Observe-only；privileged method 只通过逐请求 durable grant。
6. timeout/Cancel 必须绑定 request id；不得声称能撤销已发送设备副作用。

## 11. 排障速查

| 现象 | 首先检查 |
|------|----------|
| 一直 `WaitingModel` | endpoint/API key、HTTP timeout、worker 是否仍在、队列 run id |
| 工具 timeout 后后续也卡 | active service/host 调用是否尚未返回、session 是否 poisoned |
| timeout 后结果完全不相关 | session 是否已 poisoned、是否错误复用了旧 generation |
| 点 Stop 后仍写入 | 写命令是否已发送；取消请求不能撤回已发送副作用 |
| 写到了意外进程 | 审批期间 `processRevision` 是否变化 |
| scan/symbol 结果串台 | 两个前端是否交错执行复合命令 |
| 正常 2xx 却得到半截回答 | provider 是否看见 `message_stop`/`finish_reason` |
| provider 报 context 太长 | 本地估算是否忽略工具 schema、输出预留和 provider 上限 |
| 切会话后 prompt 变了 | 会话文件中的 `systemPrompt`/`tokenLimit` |
| API key 配置消失 | `ai_config.json` 是否损坏后被启动流程覆盖 |
| 清空 API key 后又出现 | Save 跳过空 key，没有调用 `removeConfig()` |
| Native IPC client 收不到完整帧 | header/payload 上限、deadline、partial close 和 terminal Error drain |
| Native IPC privileged 请求被拒绝 | GUI 是否批准、grant 是否因 target/generation/deadline/session 失效 |
| 退出偶发崩溃 | provider HTTP callback 的 detached 生命周期；工具和 Native IPC worker 应已 join |

## 12. 建议的自动测试起点

当前 `native_agent_mem_service` 的 23 个测试组覆盖既有 service/Agent 边界。Native IPC 另有 6 组 security-audit、12 组 approval-broker、5 组 protocol、5 组 transport、8 组 framed-I/O、8 组 handshake、6 组 request-contract、9 组 request-session、4 组 method-catalog、15 组 dispatcher 和 10 组 runtime 测试。socket client 的 4 组与 multi-port manager 的 6 组覆盖真实 Winsock loopback、partial I/O、timeout/EOF poison、三端口回滚、request/disconnect exclusion 和 reconnect generation，Debug/Release 各连续 100 次通过；provider stream 的 14 组覆盖完整/截断/重复 terminal、空 `finish_reason`、malformed/non-SSE、分片与 partial error retention。`native_persistence_recovery` 的 4 组覆盖损坏索引恢复以及 API key、settings、session 事务式加载，连续 50/50 通过。当前共 20 项 CTest；fresh Release `ENABLE_NATIVE_IPC=ON` 在 AI Chat 关闭和开启两种配置下均为 20/20，并完成产品链接。其余测试优先从无设备依赖的边界开始：

1. 用本机假 provider HTTP/TLS 覆盖真实 content receiver、状态码和 full-response 解析。
2. 用 table tests 覆盖 tool use/result 配对、预算和审批。
3. 对持久化文件/消息大小上限和原子安装失败增加故障注入；损坏/错误类型 JSON 的事务恢复已有回归。
4. 用真实 Android 设备记录三端口 timeout/reconnect 与 driver/process/scan/breakpoint 恢复结果。
5. 用真实 GUI approval click 与 Android device 覆盖 privileged adapter/send-boundary；persistent security audit、fail-closed consume、execution outcome、session/request cancel 与 GUI status gate 已有无设备测试。
6. 在不同 Windows 用户/session 与 remote client 环境做身份负向测试。
7. 自动提取并比较内置 Agent/IPC capability、结果契约和 feature gate。

## 13. 一页调用链

```text
Startup
  config/settings -> live providers/tool/http
  session index -> active session -> load (currently can override global prompt/token)

Send
  user message -> getMessagesForRequest
  -> AgentController -> provider -> HttpClient(detached)
  -> UIMessageQueue -> pollMessages

Tool
  AgentRunner budget/safety
  -> optional approval bound to generation/PID/revision
  -> AgentTaskExecutor queue -> owned joinable worker
  -> synchronous ToolExecutor -> MemService/socket
  -> ToolResult -> follow-up model request

Stop
  cancel HTTP orchestration + signal queued/active tool context
  != roll back a sent write or persist its late result

Exit
  Native IPC cancel/join + HTTP bounded wait + tool worker join + device disconnect
  != proof that provider HTTP detached work has ended

Socket timeout
  -> poison session + advance generation
  -> reject reuse until explicit reconnect

Provider 2xx
  -> require a provider terminal event
  -> otherwise partial error, never tool execution
```
