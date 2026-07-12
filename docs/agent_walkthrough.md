# AMem AI Agent 代码走读

适用分支：`NativeAgent`（基线来自 `AIChat`）
最后更新：2026-07-12

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
  -> seedDefaultsIfEmpty()
  -> saveToFile("ai_config.json")
  -> configure live providers
```

API key 在文件中是 DPAPI 密文，endpoint/model 是明文。

这里有一个重要失败路径：`loadFromFile()` 的返回值当前没有被检查。若文件存在但 JSON 损坏，内存配置已清空，随后 save 可覆盖原文件。合法 JSON 中字段类型错误还可能从 `json::value()` 抛出。排查“升级后 key 消失”或“打开 AI Chat 就异常”时，先检查这一段，见 A-04。

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

`loadOrDefault()` 当前把“不存在”和“损坏”都当成加载失败，并会写默认文件。不要把它当成无损恢复机制。

### 1.4 初始化会话

```text
SessionManager::init("ai_sessions", "ai_session.json")
  -> load index
  -> optional legacy migration
  -> choose/create active id
  -> ChatSession::load(active session file)
```

注意顺序：全局 prompt/token 已先写进 `session_`，随后 `ChatSession::load()` 又从会话文件读取同名字段。因此已有会话值会覆盖全局值。切换会话也一样；新会话只清消息，会继承之前留在 `session_` 的值。见 A-12。

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

另一个完成条件问题在 provider 层：

- Claude 收到 `message_stop` 会设置 `completed`，但完成回调不检查。
- DeepSeek 收到 `finish_reason` 会设置 `finished`，但完成回调不检查。
- OpenAI 不记录 stream terminal；公共 parser 还会过滤 `[DONE]`。

因此 HTTP 2xx 正常关闭但 SSE 被截断时，三类 provider 都可能提交部分文本/tool arguments。partial 内容可以展示，但在看到合法 terminal 前不应进入工具执行。

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

另一个 GUI/Agent/HTTP IPC caller 可在两条命令之间插入。

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
IpcServer::Stop() [only HAVE_LEGACY_HTTP_IPC]
  -> HttpClient::shutdown()
  -> AgentTaskExecutor::shutdown() and join
  -> DisconnectMultiPort()
  -> ImGui teardown
```

当前边界：

- IPC 只 join accept thread，不 join client handler。
- HTTP 最后 callback 不在 `inFlight_` 计数内。
- HTTP 只等待 3 秒，worker 仍是 detached。
- Agent 工具 worker 会取消 queued/active task，并等待 active executor 返回后 join。

因此工具 shutdown 已闭环，但整个应用退出仍受 HTTP 和 IPC detached task 约束。若调试退出崩溃、静态析构异常或偶发访问，应先检查这两类任务；设备 socket 在工具 worker join 后才断开。

GUI 的 connect/disconnect/auto-reconnect 现在通过 `DeviceSession` exclusive lifecycle lease；普通命令持 shared request lease，因此关闭会等待在途请求释放。该不变量已有无设备锁测试，但真实三端口 client 的并发压力与迟到字节仍缺 fake transport/loopback 覆盖。

## 8. 会话和配置的真实数据流

### 8.1 每次消息持久化

`ChatSession::addMessage()`：

1. 加消息。
2. 超过 1000 条时按完整对话组裁剪。
3. 根据估算 token 数裁剪旧组。
4. 释放锁后调用 `save()`。

最新用户回合即使超 token limit 也会保留。单条巨大消息不会被该策略删除。

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

### 9.1 Native framing 与 transport 基础

`ipc/IpcProtocol.*` 是 transport-independent codec。默认测试构建单独验证它；`ENABLE_NATIVE_IPC=ON` 时 codec 和 native transport 会编入应用，但 `main.cpp` 仍无 start 路径。固定 24-byte header 按 little endian 逐字段编码 `AMEM` magic、精确 `1.0` 版本、message type、零 flags、request id 和 payload length。

decoder 先用完整 header 验证版本/type/flags/id/长度，再等待或复制 payload。request 最大 1 MiB，其他帧最大 4 MiB；更大的调用方 limit 不能抬高硬上限。payload 只接受合法 UTF-8。header 或 payload 不完整时返回 `NeedMoreData`、`consumed=0`；连续帧只消费第一帧。当前 `Hello`/`HelloAck` 仅定义 wire type 和 id 规则，尚没有连接级 handshake 状态机。

直接测试 `NamedPipeServer::start()` 时的调用链是：

```text
NativePipeSecurity -> protected current-user/SYSTEM read-write DACL
  -> CreateNamedPipeW(\\.\pipe\AMem.NativeAgent.v1,
       FIRST_PIPE_INSTANCE, REJECT_REMOTE_CLIENTS, max instances = 1)
  -> owned thread: overlapped ConnectNamedPipe -> serial handler
  -> DisconnectNamedPipe -> reuse the same server handle
Stop -> signal stop event -> CancelIoEx(active pipe) -> join
```

`snapshot()` 可观察 lifecycle state、accepted count、pipe name 和 last error。handler 与 accept 共用同一 owned thread，必须响应 stop event 并使用可取消 I/O。当前没有代码从产品运行时调用 `start()`，server 也不读写 `IpcProtocol` frame；handshake/capability、request deadline/cancel、GUI enable/status 和 approval broker 均未实现。调试正常应用时看不到 pipe 是预期现状。

### 9.2 默认关闭的 legacy HTTP 路径

默认构建不包含 `IpcServer.cpp`。显式配置 `ENABLE_LEGACY_HTTP_IPC=ON` 后，HTTP IPC 调用仍不进入内置 Agent 循环：

```text
local HTTP client
  -> HTTP POST 127.0.0.1:28100
  -> IpcServer::DispatchRequest()
  -> registered C++ handler
  -> client_singleton command
```

### 9.3 与内置 Agent 的差异

| 维度 | 内置 Agent | HTTP IPC |
|------|------------|---------|
| 写审批 | `AgentRunner` + UI | AMem 内无统一审批 |
| 错误 | `ToolResult` JSON audit | `success/error` HTTP JSON |
| 地址字符串 `"1234"` | 所有地址字段拒绝；退役工具不可执行 | decimal |
| 生命周期 | runId + cancellation + connection/target snapshot | detached IPC handler，无 request cancellation |
| 工具集合 | LuaJIT: 24 广告 / 24 可执行 / 0 隐藏；无 LuaJIT: 23/23/0 | 29 个 legacy method |

跨前端测试必须使用同一组语义样例，特别是地址、扫描 flags、错误和分页。

Python MCP package、配置和安装入口已经删除。静态提取显示 opt-in HTTP IPC 仍有 29 个方法；内置 Agent 的 `disassemble`/`symbol_resolve`/`breakpoint_hits` 没有同名 IPC 方法，typed read/write、分页、错误和 feature availability 也不一致。不要把 HTTP IPC 描述为受支持的外部 Agent 面。

### 9.4 IPC 安全

默认构建不监听 28100；显式启用后的 IPC 仍然：

- 无认证。
- 允许 `Access-Control-Allow-Origin: *`。
- 接受浏览器 OPTIONS。
- 暴露写内存、进程、断点和 Lua。

因此 loopback 不是充分安全边界。新增 IPC 方法前，先处理 A-01，而不是只增加参数校验。

### 9.5 IPC 响应

请求解析已有 1 MiB 上限。响应当前构造完整 JSON 后只调用一次 `send()`；Winsock 允许 short write。客户端收到截断 JSON 时，应检查服务端发送循环，不能用盲目重试掩盖。

client timeout 不会取消旧 C++ handler。没有 server request id/cancellation 前，自动重试可能放大资源占用并重叠修改共享 symbol/target 状态，因此当前文档不提供 retry-safe 方法清单。

## 10. 扩展时的检查步骤

### 10.1 新增工具

1. 在协议层实现命令，使用 `SocketCommand::execute*`。
2. 标出资源域：process、scan、symbol、breakpoint、driver 或全局。
3. 判断是否需要 PID/revision、scan epoch 或 symbol epoch。
4. 设计 schema 和输入上限，地址强制 `0x`。
5. 设计输出上限/分页，避免把完整大列表塞给模型。
6. 选择 `ToolSafety`，并同步 `DefaultSystemPrompt.h`。
7. 在 `ToolDefinitions.cpp` 注册。
8. 不向旧 HTTP IPC 增加方法；外部自动化必须等待受限 transport 决策并复用 `MemService`。
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

### 10.4 替换或删除 IPC

1. 先确认鉴权/capability。
2. transport adapter 只映射 `MemService` 参数、错误和上限，不复制业务逻辑。
3. 使用 `sendAll()` 和响应上限。
4. handler 必须可停止和 join。
5. 明确重试是否安全；有共享状态副作用的方法不应自动重试。
6. timeout 不等于服务端取消；没有 request id/cancellation 时避免盲目重试。

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
| IPC client 返回无效 JSON | IPC 单次 `send()` 是否 short write、响应是否过大 |
| IPC 与内置地址不同 | 无前缀字符串的 reject/decimal 差异，统一改成 `0x...` |
| 退出偶发崩溃 | HTTP callback 和 legacy IPC handler 两类 detached task；工具 worker 应已 join，native pipe 当前未启动且测试路径会 join |

## 12. 建议的自动测试起点

当前 `native_agent_mem_service` 的 23 个测试组已覆盖地址/scalar codec、driver receipt/card redaction、进程与模块分页/解析、事务化 pointer resolution、disassembly、scan/symbol session/full-table transaction、breakpoint receipt/rich hit batch、scan 取消/完成未知、mutation audit 脱敏/轮转/晚到持久化、原生 service/adapter、raw/typed write 完成语义、target/generation、连接 lifecycle、工具排队/active cancellation、deadline、shutdown join 和退役工具历史降级。`native_ipc_protocol` 的 5 组测试固定 frame contract；`native_ipc_transport` 的 5 组测试固定 DACL/flags/name、同用户连接、单实例复用、status 和 joined stop；`native_agent_native_ipc_gate` 固定 default-off 与 no-main-start。catalog、no-Python-MCP 和 legacy IPC gate 保持原有边界。其余测试优先从无设备依赖的边界开始：

1. 用固定 SSE corpus 覆盖完整/截断/重复 terminal/malformed/non-SSE 2xx。
2. 用 table tests 覆盖 tool use/result 配对、预算和审批。
3. 用损坏/错误类型 JSON 覆盖三个配置管理器和会话索引。
4. 用 fake socket 构造 timeout 后迟到响应、partial send/recv 和 reconnect generation。
5. 为 Named Pipe framed I/O、handshake/capability、request deadline/cancel 和 approval invalidation 建立状态机/transport 测试。
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
  IPC Stop + HTTP bounded wait + tool worker join + device disconnect
  != proof that HTTP/IPC detached work has ended

Socket timeout
  -> poison session + advance generation
  -> reject reuse until explicit reconnect

Provider 2xx
  -> require a provider terminal event
  -> otherwise partial error, never tool execution
```
