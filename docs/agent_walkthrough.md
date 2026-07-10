# AMem AI Agent 代码走读

适用分支：`NativeAgent`（基线来自 `AIChat`）
最后更新：2026-07-10

本文按实际调用顺序解释内置 AI Chat 如何启动、请求模型、审批并执行工具、回喂结果、取消和退出。组件清单见 [`agent_architecture.md`](./agent_architecture.md)，当前问题编号见 [`agent_project_issues.md`](./agent_project_issues.md)，目标重构步骤见 [`native_agent_refactor_plan.md`](./native_agent_refactor_plan.md)。

代码定位以函数名为主。行号会随提交变化，不应作为维护文档的稳定锚点。

## 1. 启动阶段：第一条消息之前发生什么

入口是 `ChatWindow::ChatWindow()`。

### 1.1 注册组件

构造函数调用幂等初始化：

- `ProviderRegistry::initBuiltinProviders()`
- `ToolExecutor::initBuiltinTools()`

当前 provider 为 Claude、OpenAI-compatible、DeepSeek。工具注册表包含 39 个可执行名称，其中 7 个为隐藏兼容 alias，provider 实际收到 32 个定义。

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

`getMessagesForRequest()` 是 provider 协议正确性的关键边界。它只输出完整匹配的 assistant tool calls 和 tool results，过滤孤儿/重复 id 和运行时 system notice。

### 2.2 构造 provider 请求

`AgentController::dispatchModelRequest()`：

1. 从 `ProviderRegistry` 找当前 provider。
2. 校验 API key、model 和 endpoint。
3. 取 `ToolExecutor::getToolDefinitions()`。
4. 构造 `CompletionRequest`。
5. 调 `provider->sendCompletion()`。
6. 把 run 标为 `WaitingModel`。

endpoint 校验目前只要求 `https://`。它不会验证域名归属。特别是 OpenAI provider 的默认值是第三方 `https://ai.ikik.net/v1`，发送前要把 endpoint 当作显式信任决策。

`CompletionRequest` 带 run id，但不带 PID、handle 或 `processRevision`。模型请求和后续工具审批没有绑定目标进程。

provider 的 `getCapabilities().maxContextTokens` 当前没有参与这里的请求构造。会话 token limit 只按消息字节数/4裁剪，也没有计入当前 32 个广告工具 schema 和输出预留，所以 UI 显示“未超限”不代表实际 provider context 一定可接受。

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
- Approve/Deny

它不展示或冻结：

- 当前 PID/进程名
- process handle/revision
- scan/symbol session

因此存在典型 TOCTOU：

```text
模型基于进程 A 生成 write_value(0x...)
  -> 等待用户审批
  -> GUI 或 MCP 切换到进程 B
  -> 用户批准
  -> executor 使用当前进程 B
```

新增 process-bound 工具时，必须先解决或显式处理这个目标绑定问题，不能假设审批 arguments 已经包含完整执行上下文。

### 5.3 两层工具线程

批准或只读工具进入 `ChatWindow::startToolExecution()`：

```text
outer detached worker
  -> beginToolWorker() has already incremented tracked count
  -> ToolExecutor::execute()
       -> parse JSON
       -> schema validation
       -> runExecutorAsync()
            -> inner detached executor
            -> SocketIoTimeout::ScopedTimeout
            -> actual tool/socket command
       -> wait_for(timeout)
  -> UIMessageQueue(ToolResult)
  -> endToolWorker()
```

外层 worker 被生命周期计数覆盖，内层 executor 没有登记。

只读超时路径：

```text
outer wait_for expires
  -> return ToolResult(timeout)
  -> outer posts result and decrements tracked count
  -> inner executor may still be running
```

写类超时路径会继续 `future.get()`，所以外层保持登记直到真实结果返回。这个区别避免“报告超时但写操作稍后成功”的错误推理，却也意味着 Stop 无法结束正在执行的写工具。

### 5.4 socket 层

executor 调 `client_singleton.h` 中的命令。现代命令应使用 `SocketCommand::execute*`：

```text
connection check
  -> EnsureOpenHandle (when required)
  -> acquire per-port mutex
  -> DrainPending
  -> send/receive one command
```

锁只覆盖单命令。以下序列不是事务：

- `ScanSetRange` -> scan command
- `SymbolInit` -> `SymbolGetList`
- `AppContext::selectProcess()` 的多步清理/open/set PID

另一个 GUI/Agent/MCP caller 可在两条命令之间插入。

### 5.5 工具结果

executor 返回 JSON 字符串。`ToolExecutor::extractToolError()` 会识别：

- 顶层非空 `error`
- `success: false`

`AgentRunner::completeToolExecution()` 校验结果是否匹配当前 pending call，然后：

- 成功：写 tool audit，继续下一工具。
- 失败：写失败 audit，本批剩余工具标 skipped。
- 批次结束：`ReadyForFollowUp`。

`AgentRunner::makeToolMessage()` 会把 arguments 和 result/details 完整写入会话。敏感 card、Lua 和内存数据因此会明文落盘并可能在下一次模型请求中发送到 provider。

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
2. 清 active dispatch/tool run id。
3. 保存部分流式内容。
4. 记录 cancelled notice/trace。
5. 把 UI 和 run 恢复到 Idle。

它不会给 `ToolExecutor` 或 socket executor 发送取消。迟到工具结果会被 runId 过滤，但操作本身仍可能完成。

排查“点 Stop 后设备还是变化”时，这是当前预期实现限制，不是 runId 过滤失效。

### 7.2 只读工具 timeout

UI/模型收到 timeout 只表示等待者不再等待。内层 executor 是否退出要看 socket timeout、锁等待和具体命令。

不要立即把同一端口可用性视为已恢复；旧 executor 可能仍占锁或处理响应。

更具体地说，`WindowsSocketClient` 遇 `WSAETIMEDOUT` 后保留连接。下一请求调用的 `DrainPending()` 只清除当时已经到达的字节：

```text
old recv times out
  -> next request acquires lock
  -> DrainPending sees 0 bytes
  -> next command is sent
  -> old response arrives
  -> next request reads old response
```

partial send/receive 更无法通过 drain 恢复。timeout 后应把连接视为 poisoned，关闭并以新 generation 重连。

### 7.3 应用退出

主退出顺序：

```text
IpcServer::Stop()
  -> HttpClient::shutdown()
  -> ToolExecutor::shutdown()
  -> ImGui teardown
```

当前边界：

- IPC 只 join accept thread，不 join client handler。
- HTTP 最后 callback 不在 `inFlight_` 计数内。
- ToolExecutor 不统计内层 executor。
- HTTP/工具只等待 3 秒。

因此 shutdown 是 best effort。若调试退出崩溃、静态析构异常或偶发 socket 访问，必须同时检查三类 detached task。

此外，GUI 的 connect/disconnect/auto-reconnect 不通过统一 connection lifecycle lock。它可以在 Agent/IPC 正在 send/recv 时直接 `Close()`/替换 client；`sock_`/`connected_` 也是普通字段。排查连接按钮触发的随机失败时，要同时检查跨线程 Close 和旧 process handle 跨 connection generation 复用。

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

### 8.3 加载边界

`ChatSession::loadUnlocked()` 先完整解析 JSON，再按 `arr.size()` reserve，最后才裁剪到 1000 条。外部编辑或异常大文件可在裁剪前消耗大量内存。

配置管理器的字段类型校验和损坏恢复也不一致。实现修复时应统一为：

```text
read -> parse temporary -> validate all fields -> commit memory
                               |
                               +-- failure: preserve original and report
```

## 9. 外部 MCP/IPC 路径

外部调用不进入内置 Agent 循环：

```text
MCP client
  -> Python FastMCP tool
  -> IpcClient.call()
  -> HTTP POST 127.0.0.1:28100
  -> IpcServer::DispatchRequest()
  -> registered C++ handler
  -> client_singleton command
```

### 9.1 与内置 Agent 的差异

| 维度 | 内置 Agent | MCP/IPC |
|------|------------|---------|
| 写审批 | `AgentRunner` + UI | AMem 内无统一审批 |
| 错误 | `ToolResult` JSON audit | IPC `success/error`，Python 常转异常 |
| 地址字符串 `"1234"` | 规范 `memory_read` 拒绝；未迁移/隐藏旧工具仍按 hex | decimal |
| 生命周期 | UI runId/cancel token | Python HTTP timeout + detached IPC handler |
| 工具集合 | 32 个广告定义 / 39 个可执行名称 | 独立 MCP tool 集合 |

跨前端测试必须使用同一组语义样例，特别是地址、扫描 flags、错误和分页。

静态提取显示 IPC 有 29 个方法、MCP 有 30 个工具。MCP typed read/write 是 wrapper；IPC 的 `read_batch` 没有 MCP 工具，内置 Agent 的 `read_disassembly`/`resolve_symbol` 也没有同名 IPC 方法。无 LuaJIT 时三层还会以“返回 unavailable / 未注册 / 仍展示工具”三种方式表现。不要再用“暴露全部 C++ 能力”描述 MCP。

### 9.2 IPC 安全

IPC 监听 loopback，但当前：

- 无认证。
- 允许 `Access-Control-Allow-Origin: *`。
- 接受浏览器 OPTIONS。
- 暴露写内存、进程、断点和 Lua。

因此 loopback 不是充分安全边界。新增 IPC 方法前，先处理 A-01，而不是只增加参数校验。

### 9.3 IPC 响应

请求解析已有 1 MiB 上限。响应当前构造完整 JSON 后只调用一次 `send()`；Winsock 允许 short write。MCP 偶发收到截断 JSON 时，应检查服务端发送循环，而不只在 Python 端重试。

`IpcClient` 对部分读方法默认重试两次。Python timeout 不会取消旧 C++ handler，因此重试可能同时留下三条请求。没有 server request id/cancellation 前，自动重试必须同时评估资源放大和共享 symbol/target 状态，而不只看“是否写目标内存”。

## 10. 扩展时的检查步骤

### 10.1 新增工具

1. 在协议层实现命令，使用 `SocketCommand::execute*`。
2. 标出资源域：process、scan、symbol、breakpoint、driver 或全局。
3. 判断是否需要 PID/revision、scan epoch 或 symbol epoch。
4. 设计 schema 和输入上限，地址强制 `0x`。
5. 设计输出上限/分页，避免把完整大列表塞给模型。
6. 选择 `ToolSafety`，并同步 `DefaultSystemPrompt.h`。
7. 在 `ToolDefinitions.cpp` 注册。
8. 如需 MCP，同步 IPC、Python wrapper、constants 和错误契约。
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

### 10.4 修改 IPC/MCP

1. 先确认鉴权/capability。
2. 保持 C++ 与 Python 参数、错误和上限一致。
3. 使用 `sendAll()` 和响应上限。
4. handler 必须可停止和 join。
5. 明确重试是否安全；有共享状态副作用的方法不应自动重试。
6. timeout 不等于服务端取消；没有 request id/cancellation 时避免盲目重试。

## 11. 排障速查

| 现象 | 首先检查 |
|------|----------|
| 一直 `WaitingModel` | endpoint/API key、HTTP timeout、worker 是否仍在、队列 run id |
| 工具 timeout 后后续也卡 | 内层 executor 是否仍占端口锁、socket 是否读乱 |
| timeout 后结果完全不相关 | 旧响应是否在 `DrainPending()` 后迟到，连接是否应重建 |
| 点 Stop 后仍写入 | 已开始工具不会被 `cancelRequest()` 取消 |
| 写到了意外进程 | 审批期间 `processRevision` 是否变化 |
| scan/symbol 结果串台 | 两个前端是否交错执行复合命令 |
| 正常 2xx 却得到半截回答 | provider 是否看见 `message_stop`/`finish_reason` |
| provider 报 context 太长 | 本地估算是否忽略工具 schema、输出预留和 provider 上限 |
| 切会话后 prompt 变了 | 会话文件中的 `systemPrompt`/`tokenLimit` |
| API key 配置消失 | `ai_config.json` 是否损坏后被启动流程覆盖 |
| 清空 API key 后又出现 | Save 跳过空 key，没有调用 `removeConfig()` |
| MCP 返回无效 JSON | IPC 单次 `send()` 是否 short write、响应是否过大 |
| MCP 与内置地址不同 | 无前缀字符串的 hex/decimal 差异，统一改成 `0x...` |
| 退出偶发崩溃 | HTTP callback、内层 tool executor、IPC handler 三类 detached task |

## 12. 建议的自动测试起点

当前已有 `native_agent_mem_service` CTest 覆盖首批原生 service/adapter。其余测试优先从无设备依赖的边界开始：

1. 用固定 SSE corpus 覆盖完整/截断/重复 terminal/malformed/non-SSE 2xx。
2. 用 table tests 覆盖 tool use/result 配对、预算和审批。
3. 用损坏/错误类型 JSON 覆盖三个配置管理器和会话索引。
4. 用 fake socket 构造 timeout 后迟到响应、partial send/recv 和 reconnect generation。
5. 自动提取并比较内置/IPC/MCP capability、常量和 feature gate。

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
  -> optional approval (currently no PID/revision binding)
  -> outer tracked detached worker
  -> inner untracked detached executor
  -> socket command
  -> ToolResult -> follow-up model request

Stop
  cancel HTTP orchestration + discard late UI result
  != cancel already-running tool

Exit
  IPC Stop + HTTP/Tool best-effort waits
  != proof that all detached work has ended

Socket timeout
  -> connection may be protocol-poisoned
  -> DrainPending is not a synchronization proof

Provider 2xx
  -> require a provider terminal event
  -> otherwise partial error, never tool execution
```
