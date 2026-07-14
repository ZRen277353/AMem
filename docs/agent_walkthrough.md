# AMem AI Agent 代码走读

适用分支：`NativeAgent`（基线来自 `AIChat`）
最后更新：2026-07-14

本文按实际调用顺序解释内置 AI Chat 如何启动、请求模型、审批并执行工具、回喂结果、取消和退出。组件清单见 [`agent_architecture.md`](./agent_architecture.md)，当前问题编号见 [`agent_project_issues.md`](./agent_project_issues.md)，目标重构步骤见 [`native_agent_refactor_plan.md`](./native_agent_refactor_plan.md)。

代码定位以函数名为主。行号会随提交变化，不应作为维护文档的稳定锚点。

## 1. 启动阶段：第一条消息之前发生什么

产品启动时，`Gui::mainLoop()` 的一次性 bootstrap 会在 `HAVE_AI_CHAT` 下直接创建 `ChatWindow`，因此 AI Chat 默认可见，不需要先从菜单打开。用户关闭窗口后，CE 菜单仍通过 `Gui::getOrCreate<AI::ChatWindow>()` 重新打开并置前。Agent 初始化入口仍是 `ChatWindow::ChatWindow()`。

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

设置 UI 使用 `ChatSettingsDraft` 同时保存 provider、prompt、数值和 proxy 草稿。弹窗每次打开都从 `ApiKeyStore`/`AiSettings` 重建 snapshot；Cancel、标题栏关闭和成功 Save 会主动清零草稿。已配置 provider 可选择 `Forget saved key`，删除先暂存在草稿中并可 Undo；Save 通过 `applyChangesAndSave()` 原子安装包含全部 update/remove 的加密配置，成功后再清空 live provider key。失败不会改变磁盘、store 或 live provider。A-15 已关闭。

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

`AiSettings` 是 prompt/token 的唯一持久化所有者。构造函数先把全局 snapshot 写进 `session_`；`ChatSession::load()` 从当前用户 DPAPI 信封载入历史，不修改这两个实时值。信封内 format v2 不保存 prompt/token；format v1 的旧字段无论类型是否有效都被忽略。每次成功 load 会立即按当前全局 token limit 裁剪历史，因此切换或新建会话不会改变全局设置。A-12 已关闭。

会话加载也先校验临时状态。`ProtectedPersistence` 验证 `AMEMAIP1` 版本、purpose、长度和 DPAPI authentication，解密 JSON 仍按 32 MiB/消息/schema 边界进入临时状态。损坏活动会话不会绑定，退出、切换或下一条消息调用 `saveBound()` 时不会覆盖它；窗口保留原文件并创建新的可写会话。有效旧明文只有在完整 schema 校验后才通过 `utils::installTempFile()` 原子迁移；启动时同样检查未打开的会话。无效旧文件保持原字节。A-11 已关闭。

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

OpenAI provider 默认使用官方 `https://api.openai.com/v1`。自定义 OpenAI-compatible endpoint 除 `https://` 外还必须有绑定到规范化完整 URL 的显式信任；修改 host 或 path 会使旧信任失效。`ChatWindow` 在启动 run 前检查一次，`AgentController` 在唯一 provider dispatch 边界再次检查，任一失败都不会发送 API key、消息或工具结果。

`HttpClient` 会从 provider header map 中提取 `Content-Type`，再只向 cpp-httplib request 写入一次。不能先复制该 header 再调用 `Request::set_header()`：cpp-httplib 的 header 容器是 multimap，后者会追加第二个值；部分 OpenAI-compatible 服务会把重复 JSON content type 拒绝为 HTTP 400。HTTPS 集成测试会断言请求只有一个 `Content-Type: application/json`。

`CompletionRequest` 仍只带 run id，不直接序列化 PID/handle/revision；同一 `AgentController` 的 `AgentRunContext` 在首轮请求前捕获 connection generation 和 target snapshot。后续审批、工具出队和结果回收都使用该 context，run id 只负责异步消息隔离。

`AgentController` 在 `sendCompletion()` 前调用 `prepareContextBudget()`：取全局用户上限和当前 provider/model context，应用可选 `contextWindowTokens` override，计入清洗消息、24 个广告工具 schema、framing/envelope 与输出预留。超限只删除 request copy 中最旧的完整 user/tool group；最新组仍超限则本地失败。预算的 output reserve 会写入 provider 请求，trace 记录 input/budget/reserve/dropped。内置 endpoint 的 override 只能收紧声明，自定义 endpoint 可显式替换默认但仍受用户上限约束。

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

1. 回收上次已完成并可 join 的 worker，拒绝 shutdown 后的新请求。
2. 复制 timeout/proxy 配置。
3. 注册 request id、cancellation token、transport stop hook 和 owned/joinable worker。
4. cpp-httplib 发 HTTPS POST。
5. `content_receiver` 累积原始响应，并把数据喂给 `SSEParser`。
6. provider callback 拼接 content/tool calls，并向 `UIMessageQueue` 投递。
7. callback 返回后标记 worker 完成；下次 dispatch 或 shutdown 才 join 并移除记录。

后台线程不直接调用 ImGui，这是正确边界。

`AiLimits.h` 固定这条链的硬边界：HTTP 原始累计 16 MiB，单 SSE 行 1 MiB、事件 2 MiB，assistant content 4 MiB。tool call 最多 64 个，id/name 为 256/64 bytes，arguments 为 512 KiB/调用、4 MiB/消息。`SSEParser` 和 provider 都在 append/materialize 前检查；HTTP/SSE 超限被归类为 `InvalidResponse`，不能进入工具执行。

这些字节上限不计算 TLS、cpp-httplib 或 nlohmann JSON DOM 的内部开销，也不解决 provider context token 预算。所有不可信 JSON 在 DOM 构造前还会限制深度、节点、单容器元素、单字符串和累计字符串，并对分配失败 fail closed。本机自签名 HTTPS 已经覆盖真实 cpp-httplib/OpenSSL transport、证书拒绝、三家 provider full-response 和结构过密拒绝；真实外部 provider 互操作仍需人工验证。

### 3.3 HTTP 完成与回收时序

每条结束路径现在是：

```text
completeSafely()
  -> provider callback
  -> UIMessageQueue::push()
  -> callback returns
  -> markRequestCompleted()
  -> next dispatch/shutdown joins worker
```

`HttpClient::shutdown()` 先禁止新 dispatch、设置全部 token 并调用每个 active client 的 best-effort `stop()`，随后 join 全部线程，因此不会在最后一个 provider callback 仍运行时返回。若 callback 自身误调用 shutdown，会返回 `false` 而不是 self-join；主退出路径检查该结果。静默 read 不保证被 `stop()` 立即打断，shutdown 可能等待配置的 I/O timeout，但不会像旧 3 秒 bounded wait 那样提前遗留 worker。A-05 已关闭。

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

`runUntilBlocked()` 查 `ToolExecutor::getToolSafety(name)`，再由 `isAutomaticallyApproved()` 选择独立策略：

- `ReadOnly`：返回 `NeedsExecution`。
- 非 Lua `Write`：只读取默认 false 的 `autoApproveWrites`。
- `lua_execute`：只读取默认 true 的 `autoApproveLuaExecution`，不受通用 YOLO 开关覆盖。
- 对应策略允许时记录 `AutoApproved` trace 后返回 `NeedsExecution`；关闭时返回 `NeedsConfirmation`。

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

driver、module/pointer/disassembly/symbol resolution、四个 canonical scan、五个 canonical breakpoint 和 raw/typed memory read/write 都已在 service 边界消费 context。`driver_initialize` 返回未发送/拒绝/完成未知/确认后取消或超时回执，并在显示、审计和持久化时脱敏 card。pointer/scan/symbol 保持 transaction/epoch 语义；breakpoint set/remove/suspend/resume 返回确认回执或 `completion_unknown`，hits 返回最新最多 100 项以及 `available/dropped`，没有 continuation cursor。旧名称已经从注册表和 JSON adapter 中删除。`lua_execute` 的一次授权对应一次完整脚本调用，脚本内部 API 不重复弹窗；host 边界先复核初始 target，再把已授权 context 的可变副本绑定到 Lua registry。generation、absolute deadline 和 cancellation 保持不变；`process.attach` 成功后仅以 service-confirmed snapshot 推进脚本 target，后续 API 使用新目标，脚本最终 snapshot 再推进 Agent/IPC baseline。外部目标变化不被视为脚本切换，开始后的取消仍只作为回执标记。

GUI 的 `BreakpointWindow` 已完成 breakpoint 域迁移：窗口构造时注入 `IMemService`，添加、删除、启用、暂停、恢复和命中刷新均捕获当前 target/generation。命中读取在 DEBUG 端口排空一次无 cursor 响应，只保留最新 50,000 条；`BreakpointHit` 保存详情页所需的 GPR、`orig_x0`、syscall、FPSR/FPCR 和全部向量寄存器。超过上限时 GUI 显示丢弃较早命中的日志。

GUI symbol cache 也不再直接编排 `SymbolInit -> SymbolGetList`。`MemoryViewerWindow` 与 `BreakpointWindow` 把注入的 `IMemService` 传给 `ModuleCache`；cache miss 调用 `loadSymbolTable`，在一个 MAIN transaction 中完成 module 唯一匹配、一次 init 和全部页读取。service 限制 1,000,000 项/64 MiB 名称并在释放 transaction 后复核 target+epoch；cache 随后在自身 mutex 内再次复核 target 与 module，才安装排序结果。

GUI 的剩余设备入口也已统一：`Gui.cpp` 只负责取得 system service，随后显式注入 `CEWindow`、连接、进程、模块、版本、扫描、内存、断点和 Lua 窗口；内存/扫描/断点子面板继续传递同一引用。连接与退出分别调用 `IMemService::connect/disconnect`，模块/进程列表按 service page 拉取，Memory Viewer 的前台/后台/批量读取、写入和冻结都使用结构化 request/receipt。`AppContext` 不再发协议命令，只有 `TargetMutation` 能发布 PID/handle/name/revision 并失效展示缓存。

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

executor 调 `MemService`，system backend 再调用 `client_singleton.h` 中的协议命令。协议实现应使用 `SocketCommand::execute*`：

```text
acquire shared DeviceSession request lease
  -> acquire recursive per-port transaction gate
  -> EnsureOpenHandle (when required)
  -> acquire request-response port mutex
  -> verify lease generation is current
  -> send/receive one command
```

connect/disconnect/reconnect 持有 exclusive lifecycle lease；I/O 错误、EOF 或 partial failure 会 poison session、推进 generation 并拒绝新请求，已删除用待处理字节尝试恢复协议同步的旧路径。

普通命令只短暂持有 transaction gate。`pointer_resolve` 会把同一个 gate 保持到模块查询和全部 pointer read 结束，因此其他 caller 不能插入；低层兼容 helper 只属于协议实现，不再被 GUI/Lua/Agent/Native IPC 当作业务入口。规范 service 在 gate 外做完整 target snapshot 校验，在 gate 内只做稳定 revision 和无 target-lock 的 generation/cancel 检查。

canonical scan 另持有 scan domain mutex 和 MAIN transaction gate：start 覆盖 set-range+scan，results 覆盖 count+page，clear 用后续 count=0 确认。GUI `ScanWindow` 使用注入的同一 `IMemService`：首次/再次扫描捕获 target context，结果分页与 clear 携带最新 epoch，选中删除在单个 transaction 内确认 count 与新 epoch。长扫描的 progress callback 同时转发 GUI 进度并观察 cancellation token；GUI Stop 只设置 token，backend 经 DEBUG 端口请求一次 stop。若主命令仍返回 terminal count，结果标为 `completed_after_cancel_request`，否则 sent request 保留 `completion_unknown`。

canonical symbol 另持有 symbol domain mutex 和 MAIN transaction gate。`symbol_resolve`/`symbol_list` 都在 gate 内完成 module 唯一匹配、`SymbolInit` 和 find/page；init 取得 gate 后才推进 epoch，保证 epoch 顺序与实际 mutation 顺序一致。offset > 0 的 list 必须携带上一页 epoch，跨前端 init 会返回 `symbol_session_changed`。

canonical breakpoint mutation 在同一个 MAIN transaction 内接收设备确认并更新 cleanup tracker。cleanup 持 gate 完成 snapshot + remove；断线只清本地 tracker，避免把旧地址施加到新 target。若 request 已开始但未收到响应，service 返回非重试 `completion_unknown`；确认完成后即使 cancellation/deadline 已到，也保留 `completed_after_*`。hits socket 响应按块读取并丢弃页外数据，Agent 的寄存器和时间使用字符串避免 64 位精度损失。

进程切换现在由 `SystemMemBackend::openProcess()` 独占业务 owner：先取得 shared request lease，再以 `TargetMutation` 把 revision 置奇数；随后在 DEBUG transaction 中停止旧扫描，在一个 MAIN transaction 中完成 breakpoint/freeze/scan cleanup、close 和 open，最后才发布非零 handle。旧 context 的在途命令要么先完成，要么在端口 gate 内因 revision 变化失败；新 capture 在 mutation 结束前不会看到半发布状态。pointer、scan、symbol 和 breakpoint 的复合序列也各自由 service transaction/epoch 保护，因此当前 GUI/Agent/Lua/Native IPC 不存在可插入裸协议步骤的产品路径。

Android server 的 `SysCall`/`Kernel` 类型只切换读写与断点实现，不改变 target identity，因此不参与 generation/revision。模块协议仍按 mapping 返回；`findResolvedModule()` 会在既有完整名/basename/唯一子串优先级内，把规范化完整路径相同的分段折叠为一个候选并选择最低 base。不同完整路径的同 basename 仍报歧义。该规则同时作用于 `module_resolve`、pointer 和 symbol module lookup，而 `module_list` 保留原始分段供诊断。

### 5.5 工具结果

executor 返回 JSON 字符串。`ToolExecutor::extractToolError()` 会识别：

- 顶层非空 `error`
- `success: false`

`AgentRunner::completeToolExecution()` 校验结果是否匹配当前 pending call，然后：

- 成功：写 tool audit，继续下一工具。
- 失败：写失败 audit，本批剩余工具标 skipped。
- 批次结束：`ReadyForFollowUp`。

`AgentRunner::makeToolMessage()` 通常会把 arguments 和 result/details 完整写入会话；driver card 仍在进入历史前使用 `[REDACTED]`，避免可复用 credential 即使在解密后也出现。Lua、地址、内存和其他工具数据保留完整 provider 连续性，但整个会话与标题索引以当前 Windows 用户 DPAPI 信封落盘。它们在进程内解密，并可能在下一次模型请求中发送到配置的 provider；本地保护不是远端数据最小化。

`ToolExecutor` 在 executor 返回后立刻把最终 JSON 限制为 4 MiB。超限结果会释放、不会写入 tool message；对可能已经发送的 mutation，completion 必须是 `completion_unknown`。tool audit 经过 result/details 省略和固定摘要两级收缩，最终受 8 MiB 普通消息上限约束。

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
- HTTP request worker 全部由 `HttpClient` 持有；shutdown 取消/stop 后等待 provider callback 返回并 join。
- transport stop 是尽力中断，静默 read 仍可能让退出等待配置的 I/O timeout。
- Agent 工具 worker 会取消 queued/active task，并等待 active executor 返回后 join。

因此正常主线程退出在断开设备和销毁 ImGui 前会排空 Native IPC、HTTP callback 和 Agent 工具 worker。若退出停顿，先检查 provider socket 是否正等待 read timeout，而不是把 join 改回 bounded wait 或 detached。

GUI 的 connect/disconnect/auto-reconnect 现在委托 `MultiPortClientManager`，它通过 `DeviceSession` exclusive lifecycle lease 串行关闭旧端口、清目标状态、连接或回滚 MAIN/DEBUG/ERROR；普通命令持 shared request lease，因此 disconnect 会等待在途请求释放。脚本故障与真实三连接 loopback 已覆盖单端口 poison、全端口重连和 generation 隔离；Android 远端状态恢复仍需设备验证。

## 8. 会话和配置的真实数据流

### 8.1 每次消息持久化

`ChatSession::addMessage()`：

1. 在入队前校验 8 MiB content、64 个 tool calls、id/name 和 arguments 预算。
2. 预检受保护的最新用户回合；即使删完旧组仍超过 16 MiB/1000 条时，事务式拒绝且不丢旧历史。
3. 加消息，按完整对话组裁剪到 1000 条和 16 MiB retained payload。
4. 根据估算 token 数裁剪旧组。
5. 释放锁后调用 `save()`；JSON 转义后的最终文件超过 32 MiB 时拒绝安装。

最新用户回合即使超 token limit 也会保留，但不能绕过消息/session 硬上限。assistant 消息无法入会话时不会执行其工具；tool outcome 无法入会话时不会继续下一工具或 follow-up。

普通消息保存只作用于有效绑定。退出和会话切换使用 `saveBound()`，加载失败留下的损坏路径不会被随后写回；新建/缺失会话则建立可写绑定。loader 在分配/解密前检查 33 MiB 信封上限，解密 JSON 保持 32 MiB 上限，拒绝超过 10,000 条的磁盘消息数组，只 reserve/物化最后 1,000 条，并在提交前验证 16 MiB retained payload。信封内 session format v2 只保存历史；v1 prompt/token 字段迁移时忽略。A-09、A-11 与 A-12 已关闭。

`estimateTokenCount()` 现在使用保守 UTF-8/message framing 估算，负责 UI 的 history retention 指示和持久化裁剪。它不等于实际 provider request：dispatch 会再叠加 64k/128k/200k provider context、工具 definitions、provider envelope 与 256..4096 output reserve。两层分离使切换 provider 时不会永久丢掉较长历史。

### 8.2 请求历史和磁盘历史不同

磁盘可包含 runtime system notice、不完整工具组和全部审计；`getMessagesForRequest()` 会在发送前生成清洗副本。不要为了“简化 provider”而直接读取 `getMessages()`。

### 8.3 独立 mutation audit

`ai_mutation_audit.jsonl` 不属于聊天 session。write-classified 工具和 symbol active-table session effect 在 executor callback 前写入；manual deny 和队列拒绝由 UI 边界补写。单条记录最多 64 KiB，active 文件最多 4 MiB并保留一个 `.1` 备份；加载时忽略损坏 JSONL 行。

审计保留 run/tool-call id、approval、effect/resource domain、预期 generation/target、duration、success 和 completion。driver/Lua secret/error、raw memory bytes、bulk output、registers、hits、items 和大字段仅保存 omitted/size 摘要。它仍是明文文件；会话 DPAPI 保护不会把审计升级为密文或不可篡改日志。

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
       -> privileged: bounded broker submission
            -> lua_execute + policy enabled: approved immediately
            -> otherwise: wait for GUI decision
            -> durable one-shot consume -> execute -> outcome audit
  -> validated Response {ok, completion, result|error}
Cancel/deadline/invalidation/Stop -> same cooperative cancellation context
```

`timeout_ms` 默认 30 秒、最大 5 分钟，接收 Request 时转换成 server `steady_clock` absolute deadline。client 不能在 payload 中声明 capability；dispatcher registry 决定 method 需要 Observe、TargetSelection、TargetMutation 或 HostExecution。handshake 只 grant Observe；缺失 privileged capability 的 method 仅在 dispatcher 明确支持 approval submission 时进入 worker，否则在 dispatch 前拒绝。每个连接最多接纳 1024 个 unique request id；id 终身不复用，达到上限后返回 Error 并断开要求重连。Cancel 只接受 `{}`，不单独返回成功 ack；active request 的最终 Response 给出 completion。

完整 catalog 与内置 Agent 同为 24 个 canonical name：12 Observe、1 TargetSelection、9 TargetMutation、2 HostExecution。`MemJsonTools` 是 AI 和 IPC 共用的参数/结果 adapter；IPC 调用不直接读 `AppContext` 或 socket。`IpcMemServiceDispatcher` 在构造时固定 connection/target baseline，并在执行前后及 reader 每 250 ms 复核。generation、PID、handle 或 process revision 变化会发 session-level Error、取消 active context 并关闭 session。没有 broker/session binding 的 dispatcher 对 privileged method 仍返回 `approval_required`；runtime binding 后，request worker 只提交 method/client/session/target/deadline，原 params 留在 active request。reader 可继续处理 Cancel。`lua_execute` 在共享开关开启时创建 immediately-approved policy record，关闭时创建 pending record；broker 从接口层拒绝任何非 Lua 的 policy auto-approval。两条路径随后都消费 durable one-shot grant，再让 11 个 privileged method 进入 `MemJsonTools`/`MemService`；`lua_execute` 则进入注入的 host executor，并复用 `mem/LuaJsonTool`。

直接测试 `NamedPipeServer::start()` 时的调用链是：

```text
NativePipeSecurity -> protected current-user/SYSTEM read-write DACL
  -> CreateNamedPipeW(\\.\pipe\AMem.NativeAgent.v1,
       FIRST_PIPE_INSTANCE, REJECT_REMOTE_CLIENTS, max instances = 1)
  -> owned thread: overlapped ConnectNamedPipe -> serial handler
  -> DisconnectNamedPipe -> reuse the same server handle
Stop -> signal stop event -> CancelIoEx(active pipe) -> join
```

`NamedPipeServer::snapshot()` 可观察 lifecycle state、accepted count、pipe name 和 last error。`NativeAgentRuntime` 为每个 accepted handle 串起 framed connection -> handshake -> `IpcMemServiceDispatcher` -> request session。成功 Hello 获得 server 单调 session id；stop/start 清诊断计数但不复用 id。snapshot 提供 phase、session count/id、活动 client identity/capability、最后状态和 request/response/cancel 计数，不保存 params/result。system owner 保证 audit、broker 和 host executor 都晚于 runtime 析构；窗口提供显式启停和共享 Lua 策略开关。编译与运行默认关闭，`main.cpp` 只在设备断连前 shutdown。handler 与 dispatch worker 都受管并 join。Hello 与 GUI 显示明确为 `Observe + 逐请求审批；Lua 可自动授权`，不会授予长期 privileged capability。

privileged broker 已接入完整的 submission -> policy/manual decision -> consume -> execution 产品链。worker 用 `{sessionId, requestId}` 提交 metadata 并保持 active request；Cancel/Stop/session 关闭产生精确终止状态。共享 `IpcApprovalAuditLog` 写 `native_ipc_approval_audit.jsonl`，轮转 `.1`，更新最近 100 条及成功/失败计数。schema 2 把 broker 转换记为 `approval_transition`，把每个 consumed grant 的最终摘要记为 `execution_outcome`，并以 `auto_approved` 区分 policy/manual；旧 schema 1 仍可加载。日志是明文，含 client/method/session/request、authorized/observed generation/target、success、completion 和 error code，但不含 params、result JSON 或 error message。GUI“特权安全审计”显示最近 20 条。

broker `consume()` 在锁内复核 session/request/deadline/generation/target 并烧毁 record，再在锁外审计；只有 durable success 返回 grant，失败不可重试复用。dispatcher 随后校验 grant metadata，在 adapter/send 前再次检查 Cancel/deadline，并在返回 response 前同步记录一次 outcome。consume 审计失败可阻止执行；outcome 审计发生在可能已发送的 mutation 之后，因此失败只进入可见 health，不覆盖真实 completion。deny、expire 或 consume 前 Cancel 没有 execution outcome。

`process_open` 是特殊的 `Selection` 路径：dispatcher 在 mutex 下暂时允许预期中的目标切换，调用 adapter 后从结构化结果取出新 snapshot，并与 service 当前 snapshot 精确比对；只有 connection generation 和完整 target 都一致时才推进 session baseline。其余方法仍绑定旧 baseline。Native completion 还新增 `timed_out_before_start`、`timed_out`、`completed_after_deadline`，确保“截止时间后收到设备确认”不会被错误表述为未执行。

### 9.2 Python MCP 与 legacy HTTP 已删除

FastMCP package、配置、安装入口和旧 loopback HTTP server 已从 `NativeAgent` 删除。产品不再提供端口式 HTTP 控制面，也没有恢复它的 CMake 选项或 compile macro。`tools/protocol_reference/amem_client.py` 只用于直接排查 Android 二进制协议，不是 GUI IPC client，也不参与 Agent 运行。

### 9.3 Native IPC 与内置 Agent

| 维度 | 内置 Agent | Native IPC |
|------|------------|------------|
| 授权 | 非 Lua write 与 Lua 分开设置；Lua 默认自动批准 | Hello 仅 Observe；每个 privileged request 都用 one-shot grant，Lua 默认 policy-approved |
| 业务 adapter | `MemJsonTools` / `LuaJsonTool` | 同一套 `MemJsonTools` / `LuaJsonTool` |
| 地址字符串 `"1234"` | 拒绝，必须显式 `0x` | 拒绝，必须显式 `0x` |
| 生命周期 | run context + cancellation + owned tool worker | session/request id + cancellation + owned pipe/worker |
| 工具集合 | LuaJIT: 24；无 LuaJIT: 23 | 24-name catalog；Lua host 不可用时返回 feature error |

两条路径共享业务解析和目标语义，但授权主体、wire envelope 和审计日志不同。新增能力时更新 `IpcMethodCatalog` 和静态 capability gate，不复制第二套参数解析或结果 schema。

### 9.4 Native IPC 安全与响应

Native IPC 编译和运行均默认关闭。显式启用后，Named Pipe 使用当前用户/SYSTEM DACL、拒绝 remote client、单实例、严格 Hello、payload 上限、绝对 deadline、request id 和 Cancel。Hello 不授予长期 privileged capability；目标选择、目标修改和 host execution 都必须逐请求生成并消费 durable one-shot grant。除 `lua_execute` 可按默认开启的共享策略省略人工决策外，其余 privileged request 仍等待 GUI；关闭 Lua 策略也会恢复其 GUI 审批。

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
| provider 报 context 太长 | trace 中 input/budget/reserve、当前 endpoint/model context override 是否真实；本地估算不是精确 tokenizer |
| 切会话后历史变短 | 当前 `AiSettings` token limit 会在 load 成功后立即裁剪历史组 |
| API key 配置未加载 | `ai_config.json` 的 load status/log；损坏文件应保留而不是被默认值覆盖 |
| Forget API key 后又出现 | provider mutation 是否原子安装成功；失败时草稿应保留，成功后 store/live provider 都应为空 |
| Native IPC client 收不到完整帧 | header/payload 上限、deadline、partial close 和 terminal Error drain |
| Native IPC privileged 请求被拒绝 | GUI 是否批准、grant 是否因 target/generation/deadline/session 失效 |
| 精确 `module_resolve` 报 ambiguous | `module_list` 是否存在不同完整路径的同 basename；同路径多分段会自动折叠，真正跨路径歧义需收窄查询 |
| 退出长时间停顿 | provider HTTP 静默 read 是否仍在等待配置 timeout；所有 worker 最终必须 join |

## 12. 建议的自动测试起点

当前 `native_agent_mem_service` 的 33 个测试组覆盖既有 service/Agent 边界、connection generation、批量读取、冻结 completion、真实 LuaJIT 脚本的受控目标推进与失败后 run target 同步、ToolExecutor schema 矩阵、AgentRunner 审批组合、tool history 配对与 JSON 结构限制；`native_app_context_state` 覆盖目标发布、cache invalidation 与 disconnect。Native IPC 另有 6 组 security-audit、12 组 approval-broker、5 组 protocol、5 组 transport、8 组 framed-I/O、8 组 handshake、7 组 request-contract、9 组 request-session、4 组 method-catalog、16 组 dispatcher 和 10 组 runtime 测试。socket client 的 4 组与 multi-port manager 的 6 组覆盖真实 Winsock loopback、partial I/O、timeout/EOF poison、三端口回滚、request/disconnect exclusion 和 reconnect generation；provider/SSE 有 19 组，本机 HTTPS/full-response 有 6 组，context budget 有 5 组，provider trust 有 3 组，persistence recovery/limits/session protection/global-settings/设置草稿/原子安装/JSON 结构有 13 组，HTTP lifecycle/response limit 有 5 组。九个静态 gate 加入 GUI/Lua/main service boundary、context dispatch、endpoint trust 和 session protection integration 后，当前共 30 项 CTest；隔离 mandatory-feature + Native IPC Release 证据记录在重构计划。剩余验收集中在真实环境：

1. 用真实 Android 设备记录三端口 timeout/reconnect 与 driver/process/scan/breakpoint 恢复结果。
2. 用真实 GUI approval click 与 Android device 覆盖 privileged adapter/send-boundary；persistent security audit、fail-closed consume、execution outcome、session/request cancel 与 GUI status gate 已有无设备测试。
3. 在不同 Windows 用户/session 与 remote client 环境做身份负向测试。
4. 完成 GUI 连接、进程切换、批量刷新、冻结和 Lua target-switch smoke。
5. 选做真实外部 provider 互操作；本机 HTTPS transport/full-response 已自动化。

2026-07-13 已用内置 Agent 完成一次真实 `Kernel` execute breakpoint set/hits/suspend/resume/remove：目标 `rf_native_add` 的指令和 PC/寄存器已校验，暂停/恢复的命中时间序列符合预期，移除后命中归零，连接未 poisoned。该记录关闭“硬件后端完全未验证”的缺口；同 ELF 多 mapping 的 A-23 已补原生回归，硬件断点本身尚未成为自动 fixture。

## 13. 一页调用链

```text
Startup
  config/settings -> live providers/tool/http
  session index -> active session -> load history with live global prompt/token

Send
  user message -> getMessagesForRequest
  -> AgentController -> provider -> HttpClient(owned joinable worker)
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
  Native IPC cancel/join + HTTP cancel/stop/join + tool worker join + device disconnect
  HTTP join may wait for the configured I/O timeout, but no worker remains on return

Socket timeout
  -> poison session + advance generation
  -> reject reuse until explicit reconnect

Provider 2xx
  -> require a provider terminal event
  -> otherwise partial error, never tool execution
```
