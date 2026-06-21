# AMem AI Agent 解读文档(代码走读与设计原理)

适用分支：`AIChat`　|　最后更新：2026-06-21

本文是 [`agent_architecture.md`](./agent_architecture.md) 的配套**解读**:架构文档讲「有哪些部件、各自是什么」,本文讲「**一次对话到底怎么跑起来的、为什么这么设计、改的时候要小心什么**」。建议先读架构文档第 2、4、5 节,再读本文。

所有代码定位形如 `文件:行号`,以当前分支为准(行号可能随后续提交漂移,以函数名为准更稳)。

---

## 1. 一次对话回合的完整生命周期

下面跟踪「用户发一条消息 → 模型调用一个**写类**工具 → 用户批准 → 工具执行 → 结果回喂模型 → 模型给出最终文本」的全过程。这是理解整个子系统最快的路径。

### 阶段 A — 发起请求(主线程)

1. 用户在 `ChatWindow` 输入并发送。`ChatWindow` 把用户消息写入 `ChatSession`,调用 `agentController_.resetForNewRun()` 生成新的 `run id`,清空 `streamingContent_`。
2. `ChatWindow::dispatchAgentRequest()`(`gui/ai/ChatWindow.cpp:1671`)取 `session_.getMessagesForRequest()` 作为消息序列,构造 `AgentController::ModelRequest`,刷新 `cancelToken_`,调用 `agentController_.dispatchModelRequest(request, cancelToken_)`。
3. `AgentController::dispatchModelRequest()`(`gui/ai/AgentController.cpp:89`):
   - 查 `ProviderRegistry` 拿到当前 provider;`validateProviderConfig()` 校验 API key / `https://` 端点 / model 非空;
   - 组 `CompletionRequest`:`messages` 来自上一步,`tools = ToolExecutor::getInstance().getToolDefinitions()`,`runId = run_.id`;
   - 调 `provider->sendCompletion(completion, cancelToken)`,`markWaitingModel()`(状态 → `WaitingModel`)。
4. 派发成功后,`dispatchAgentRequest` 在 `gui/ai/ChatWindow.cpp:1696` 设 `activeDispatchRunId_ = agentController_.runId()`,`state_ = WaitingResponse`。**这一步是后续过滤陈旧响应的关键**。

### 阶段 B — 后台 HTTP 与流式(worker 线程)

5. provider 的 `sendCompletion`(如 `ClaudeProvider.cpp`)在调用线程把 `ChatMessage[]` 转成各家 JSON 请求体,然后交给 `HttpClient::postAsync`(`gui/ai/HttpClient.cpp:224`),后者 **detach 一个 worker 线程** 跑 HTTPS。
6. worker 收到 SSE 数据块 → `SSEParser` 按行解析 → provider 的解析回调累积文本 / 组装 tool_call:
   - 每段增量文本 → `onToken` → `UIMessageQueue::push({Token, runId, 文本})`;
   - 流结束 → 组装出 `CompletionResponse`(可能含 `toolCalls`)→ `UIMessageQueue::push({Completion, runId, response})`;
   - 出错 / 取消 → `Completion` 带 `error` 或 `Error`。

> worker 线程**全程不碰 ImGui**,只 push 到队列。

### 阶段 C — 主线程消费(`pollMessages`)

7. 每帧 `ChatWindow::pollMessages()`(`gui/ai/ChatWindow.cpp:1149`)`tryPop` 队列。先做 **runId 过滤**(`:1152`):工具结果比对 `activeToolRunId_`,其余比对 `activeDispatchRunId_`,不匹配直接丢弃(防止上一轮被取消后迟到的响应污染当前轮)。
8. `Token`:追加到 `streamingContent_`。
9. `Completion`(`:1165`):
   - 若 `resp.error`(`category != None` 即真)→ `displayErrorForCategory()`(取消会显示「Request cancelled」,鉴权失败会顺手打开设置面板等);
   - 否则提交 assistant 消息,经 `validateAndNormalizeToolCalls()` 规整工具调用;
   - **有 tool_call** → `processToolCalls(calls)` 进入工具循环(保留 `requestStartMs_`,因为这仍是同一个「AI 回合」);
   - **无 tool_call** → `finishCompleted()`,`state_ = Idle`,回合结束。

### 阶段 D — 工具循环(主线程编排 + 工具线程执行)

10. `processToolCalls`(`:1592`)→ `agentController_.beginToolCalls(calls, makeAgentConfig())` → `AgentRunner::beginToolCalls`(`gui/ai/AgentRunner.cpp:41`):`stepCount++`(超 `maxAgentSteps` 直接 `Stopped`),按 `maxToolCallsPerTurn` 截断,然后 `runUntilBlocked()`。
11. `runUntilBlocked`(`AgentRunner.cpp:218`)从当前下标扫工具:
    - 遇**写类**且未 `autoApproveWrites` → `awaitingConfirmation_=true`,产出 `NeedsConfirmation` + `pendingToolCall`;
    - 读类 / 已批准 → 产出 `NeedsExecution` + `toolCallToExecute`。
12. `ChatWindow::handleAgentOutcome`(`:1615`)按 outcome 分派:
    - `NeedsConfirmation` → `state_ = ToolConfirmation`,UI 弹审批框;
    - 用户**批准** → `approvePendingTool` → `resumeApproved` → `NeedsExecution`;
    - 用户**拒绝** → `denyPendingTool` → `resumeDenied`:本批剩余工具全部标记跳过,产出 `ReadyForFollowUp`。
13. `NeedsExecution` → `ChatWindow::startToolExecution()`(`:1597`)设 `activeToolRunId_`,**detach 一个工具线程**跑 `ToolExecutor::getInstance().execute(call)`,完成后 `UIMessageQueue::push({ToolResult, runId, result, durationMs})`。
14. `ToolExecutor::execute`(`gui/ai/ToolExecutor.cpp:322`):查表 → 解析参数 → **schema 校验** → `runExecutorAsync`(`:276`,又一层 detach + `wait_for(timeout)`)。executor 内部调 `client_singleton` 设备命令(经端口互斥锁串行化)。读类超时即报错;写类等待真实结果。
15. `ToolResult` 回到 `pollMessages`(`:1266`)→ `completeToolExecution`(`AgentRunner.cpp:141`):校验结果与当前待执行工具匹配(`sameToolCall`,防乱序),写入审计消息;成功且还有下个工具 → 继续 `runUntilBlocked`;失败 → 跳过本批剩余;本批结束 → `ReadyForFollowUp`。

### 阶段 E — 回喂模型 / 收尾

16. `ReadyForFollowUp` → `sendFollowUpAfterTools()`(`:1657`)→ 再次 `dispatchAgentRequest(session_.getMessagesForRequest(), …)`。注意此时历史里已经有 assistant(tool_use)+ 若干 tool(tool_result)消息,**回到阶段 A 第 2 步**,开始新一轮 model 调用。
17. 如此 model→tool→model 往返,直到:模型返回**纯文本**(阶段 C 第 9 步「无 tool_call」)→ `finishCompleted()`,或触达 `maxAgentSteps` → `Stopped` 收尾。run 状态回 `Idle`,UI 可发下一条。

---

## 2. 关键设计决策与原理

### 2.1 为什么用 `UIMessageQueue` 而不是回调直接改 UI
ImGui 是即时模式、单线程渲染;在 worker 线程触碰 ImGui 状态会数据竞争甚至崩溃。把后台结果统一塞进一个加锁队列、由主线程每帧消费,是最简单且无锁竞争的边界。**推论:任何新的后台产物(新消息类型)都要走 `UIMessageType` + `pollMessages`,不要新开回调直通 UI。**

### 2.2 为什么 `AgentRunner` 不含任何 ImGui
`AgentRunner` 是纯状态机(`beginToolCalls`/`resumeApproved`/`resumeDenied`/`completeToolExecution` 返回 `Outcome`)。把「循环逻辑」与「呈现 / 审批 UI」解耦,使循环可单测、可推理,也让 `ChatWindow` 专注 UI。`AgentController` 居中:负责 provider 查找、请求构造、run 状态与 trace 累积。

### 2.3 `runId` 过滤:陈旧响应隔离
取消一个请求只是 `cancelToken_->store(true)`——但 worker 可能已经在路上,迟到的 `Completion` 不该污染新回合。`dispatchAgentRequest` 成功后记录 `activeDispatchRunId_`,`pollMessages` 只认当前 id(`ChatWindow.cpp:1152`)。删除会话 / 清空历史 / 发新消息都会刷新 run,旧响应自然被丢。

### 2.4 `getMessagesForRequest()`:把配对正确性收敛到一处(最重要的不变量)
Anthropic / OpenAI 都要求:assistant 的每个 `tool_use` 必须有紧随其后、id 匹配的 `tool_result`,反之亦然,否则**硬 400**。provider 端是按「位置相邻」拼装的,看似脆弱;但所有请求都先过 `ChatSession::getMessagesForRequest()`(`gui/ai/ChatSession.cpp:301`),它在那里集中保证:

- 跳过运行时 `System` 通知(取消 / 错误提示不发给模型,只有配置的系统提示发);
- 剔除空 / 重复 id 的 tool_call;
- 仅当一组 tool_call 的**全部**结果齐备时,才连续输出 `assistant + tool_results`;不完整组降级为纯文本;
- 丢弃没有前驱 tool_use 的孤儿 tool_result。

**因此 provider 收到的恒是干净配对**——这是为什么「provider 按位置拼装」是安全的。改历史截断 / 消息持久化逻辑时,务必保持这条:破坏它就会在某些会话上触发 400。

### 2.5 审批门与 `autoApproveWrites`
模型可能「幻觉」出危险写操作(改内存、设断点、跑 Lua)。每个工具静态分 `ReadOnly`/`Write`;写类默认弹审批(`AgentRunner` 的 `NeedsConfirmation`)。`autoApproveWrites`(YOLO)默认关。**新增任何会改变设备 / 进程 / 扫描状态的工具,必须注册为 `Write`**,否则绕过这道唯一的安全网。

### 2.6 预算:防失控
`maxAgentSteps`(model↔tool 往返)与 `maxToolCallsPerTurn`(单轮工具数)双重 clamp 到 [1,64]。模型若陷入「调工具→看结果→再调」的循环,步数上限会终止它并给出 `StepLimitReached` 提示。

### 2.7 工具执行:读 / 写超时语义不同
`ToolExecutor::execute`(`ToolExecutor.cpp:389`)对**只读**工具超时即返回错误(反正可重试、无副作用);对**写 / 有状态**工具则**等待真实结果**——否则会出现「报了超时,但设备其实已经写入」的歧义目标态,后续推理会基于错误前提。

### 2.8 取消与退出排空
取消是协作式的(`cancelToken`),`HttpClient` 在每个数据块检查并中止。进程退出时 detached worker 不能 join,故 `HttpClient::shutdown()`(`main.cpp` 拆除阶段调用)取消全部在途请求并**有界等待**收尾,避免 worker 在 `HttpClient`/`UIMessageQueue` 单例析构后访问已释放内存。

### 2.9 密钥加密与原子写
- API Key 用 **DPAPI** 加密(`ApiKeyStore`),按 Windows 用户隔离,从不内置 / 不进仓库;解密失败**保留密文**仅提示重输,避免瞬时故障导致永久丢 key。
- 三处配置 / 会话写盘统一走 `utils/AtomicFileWrite.h::installTempFile`:写 `.tmp`→rename;失败先把原文件挪 `.bak` 再装新文件、失败回滚,**绝不让唯一副本消失**。

---

## 3. 如何扩展

### 3.1 新增一个工具(让 Agent 能调用一个新设备能力)
1. **协议层**:在 `socket/*Commands.cpp` 实现命令,并在 `socket/client_singleton.h` 声明(用 `SocketCommand::execute*` 模板,勿手写 send/recv)。
2. **Agent 工具**:在 `gui/ai/ToolDefinitions.cpp` 写一个 `execXxx(argsJson)` executor(解析 / 校验 / 调命令 / 返回 JSON 字符串,异常用 try-catch 转 `makeError`),并在 `initBuiltinTools()` 里 `registerTool(name, desc, schema, safety, exec)`——**正确选 `ToolSafety`**。
3.(可选)**外部 MCP 也要用**:在 `ipc/IpcServer.cpp::RegisterBuiltinMethods()` 加方法,再在 `mcp/amem_mcp/tools/` 包装;注意与 `ToolDefinitions.cpp` 的参数校验保持一致。
4. 若涉及扫描 flag / 数据类型 / 内存区枚举,同步 `mcp/amem_mcp/constants.py`。

### 3.2 新增一个 Provider
实现 `AIProvider`(`getName`/`getDefaultBaseUrl`/`getCapabilities`/`configure`/`sendCompletion`),在 `ProviderRegistry::initBuiltinProviders()`(`gui/ai/ProviderRegistry.cpp`)注册。`sendCompletion` 必须:在后台经 `HttpClient::postAsync` 跑 I/O,token/完成经 `UIMessageQueue` 回投,并正确做 `tool_use`/`tool_result` 的请求拼装与响应解析。

### 3.3 调整系统提示 / 预算
- 默认系统提示在 `gui/ai/DefaultSystemPrompt.h`(首次运行写入 `ai_settings.json`);
- 预算 / 超时在设置面板或直接改 `ai_settings.json`(`maxAgentSteps` / `maxToolCallsPerTurn` / `executionTimeout` / `tokenLimit`,均会被 clamp)。

---

## 4. 常见陷阱

| 陷阱 | 后果 | 正确做法 |
|------|------|----------|
| 在 worker / 工具线程里碰 ImGui 或 run 状态 | 数据竞争 / 崩溃 | 只经 `UIMessageQueue` 回投主线程 |
| 手写 socket send/recv 不加端口锁 | 并发响应串包 | 用 `SocketCommand::execute*` |
| 把写 / 有状态工具标成 `ReadOnly` | 绕过审批门,模型可无确认地改设备 | 状态变更类一律 `Write` |
| 绕过 `getMessagesForRequest` 直接把历史发给 provider | `tool_use`/`tool_result` 失配 → API 400 | 始终经 `getMessagesForRequest` |
| 派发后忘了设 `activeDispatchRunId_` | 响应被 runId 过滤丢弃,回合卡死 | 见 `ChatWindow.cpp:1696` |
| 新增持久化用 remove-then-rename | 锁定 / 失败时丢唯一副本 | 用 `installTempFile` |

---

## 5. 调试与可观测性

- **执行轨迹**:`AgentRun.trace`(`AgentTraceEvent` 列表,≤128)记录每一步(派发 / 工具开始 / 成功 / 失败 / 跳过 / 审批 / 完成 / 错误),UI 有对应面板。排查「Agent 为什么停了」先看 trace 末尾(`StepLimitReached` / `ProviderError` / `Stopped`)。
- **日志**:`Gui::log(...)` 贯穿编排与 IPC,LogWindow 可见。
- **运行态**:`AgentRunState`(`agentRunStateLabel()` 给出可读名)。卡在 `WaitingModel` 多半是网络 / provider 错误;卡在 `WaitingApproval` 是在等用户点审批。
- **provider 错误分类**:`ProviderError.category`(`Network`/`Authentication`/`RateLimit`/`Timeout`/`InvalidResponse`/`Cancelled`/`Unknown`)决定 UI 文案与是否打开设置面板。
- **已知遗留项**:见 [`agent_project_issues.md`](./agent_project_issues.md) 与 [`scan_protocol_issues.md`](./scan_protocol_issues.md)。

---

## 6. 一页速查

```
发送 → dispatchAgentRequest → provider.sendCompletion → HttpClient.postAsync(worker)
   worker: SSE → UIMessageQueue(Token/Completion) → 主线程 pollMessages
       Completion 有 tool_call → AgentRunner(预算/审批) → startToolExecution(工具线程)
           → ToolExecutor.execute → client_singleton 设备命令
           → UIMessageQueue(ToolResult) → completeToolExecution
               → ReadyForFollowUp → sendFollowUpAfterTools(回到 dispatch)
       Completion 纯文本 → finishCompleted → Idle
不变量:UI 线程隔离 · socket 串行 · getMessagesForRequest 配对 · 写类必审批 · runId 过滤
```
