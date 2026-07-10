# Agent 业务代码审计与问题清单

审计日期：2026-07-10
适用分支：`NativeAgent`（基线来自 `AIChat`）
审计范围：`gui/ai/`、`ipc/IpcServer.*`、`mcp/amem_mcp/`、`socket/` 中被 Agent/IPC 调用的命令层、`gui/AppContext.*`、`main.cpp`。

本文记录当前工作区中仍存在的问题，以及已经落地的修复摘要。目标架构和分阶段关闭方案见 [`native_agent_refactor_plan.md`](./native_agent_refactor_plan.md)。搜索/扫描协议自身的问题不在本文审计范围内。

本次结论来自静态代码审阅和调用链核对，没有连接 Android 设备，也没有执行 provider、MCP 或退出阶段的端到端压力测试。优先级含义：

- **P0**：可能造成未授权调用、错误目标写入、协议串包、并发数据竞争、启动数据破坏或退出期悬空访问，应优先处理。
- **P1**：明显影响可靠性、审计完整性、资源边界或敏感数据安全。
- **P2**：语义漂移、跨前端不一致或可恢复的 UX/维护性问题。
- **P3**：低影响可观测性问题。

## 结论摘要

| ID | 优先级 | 状态 | 问题 |
|----|--------|------|------|
| A-01 | P0 | 未修复 | IPC 无鉴权且允许任意 CORS，回环监听不是完整安全边界 |
| A-02 | P0 | 未修复 | Agent run 和审批没有绑定目标进程 revision |
| A-03 | P0 | 未修复 | 工具执行存在未登记的第二层 detached 线程 |
| A-04 | P0 | 未修复 | 配置/索引损坏可导致启动异常或覆盖原文件 |
| A-05 | P1 | 未修复 | HTTP worker 在完成回调前就从 in-flight 计数移除 |
| A-06 | P1 | 未修复 | IPC client handler 不排空，响应也未处理 partial send |
| A-07 | P1 | 未修复 | Stop 只停止编排，不停止已经开始的工具 |
| A-08 | P1 | 未修复 | 复合工具和进程切换不是事务，可被其他前端插入 |
| A-09 | P1 | 未修复 | HTTP、模型内容、工具输出和会话载入缺少总量上限 |
| A-10 | P1 | 待安全决策 | “OpenAI” provider 默认指向第三方兼容网关 |
| A-11 | P1 | 未修复 | 会话明文保存敏感工具参数、结果和脚本 |
| A-12 | P2 | 未修复 | 全局 prompt/token 设置会被会话文件反向覆盖 |
| A-13 | P2 | 未修复 | `symbol_*` 安全分类、共享状态语义和默认 prompt 不一致 |
| A-14 | P2 | 未修复 | 内置 Agent 与 IPC/MCP 的地址字符串进制语义不同 |
| A-15 | P2 | 未修复 | 设置草稿不完整，且无法从 UI 删除 provider key |
| A-16 | P3 | 未修复 | `approvalDecision` 在快照中几乎不可观测 |
| A-17 | P2 | 未修复 | 内置 Agent、IPC 与 MCP 的能力面和结果契约未统一 |
| A-18 | P1 | 未修复 | 三类 provider 都会把缺少终止事件的截断 SSE 当成功 |
| A-19 | P0 | 未修复 | socket I/O 超时后复用连接，迟到响应可污染下一请求 |
| A-20 | P0 | 未修复 | connect/disconnect 可与在途请求并发关闭或替换 socket |
| A-21 | P2 | 未修复 | provider context 能力未参与 token 裁剪和输出预留 |
| A-22 | P2 | 部分修复 | 已有首批 MemService CTest，其余 Agent/provider/IPC/MCP 核心路径仍缺回归测试 |

## 当前未解决问题

### A-01：IPC 无鉴权且允许任意 CORS

**证据**

- `IpcServer::Start()` 绑定 `127.0.0.1:28100`，但没有认证 token、客户端身份或方法级 capability。
- `IpcServer::HandleClient()` 接受浏览器 `OPTIONS` 预检。
- `IpcServer::BuildHttpResponse()` 返回 `Access-Control-Allow-Origin: *`。
- 同一入口暴露 `write_memory`、断点、扫描、`execute_lua`、进程切换等有副作用的方法。

**影响**

回环地址只阻止远端主机直接连接，并不阻止本机浏览器页面或其他本地进程访问。当前 CORS 配置还主动允许网页脚本读取响应。恶意网页可尝试从浏览器调用本地 AMem；浏览器的 Private Network Access 策略在不同版本中可能额外阻拦，但不能作为服务端鉴权。

**建议**

1. MCP 不需要浏览器调用时，移除 CORS 和 `OPTIONS` 支持。
2. 每次 GUI 启动生成高熵 bearer token，通过受控配置/环境传给 MCP 进程，并在所有请求上校验。
3. 校验 `Origin`/`Host`，但不要把它们当作 token 的替代品。
4. 对写内存、进程切换、断点和 Lua 增加 capability 或 GUI 审批策略。

### A-02：Agent run 和审批未绑定目标进程

**证据**

- `AppContext` 已维护 `selectedPid`、`processHandle` 和 `processRevision`。
- `AgentRun`、`CompletionRequest`、`ToolCall`、`AgentRunner` 均未保存目标快照。
- `ChatWindow::drawToolConfirmationModal()` 只展示工具名和参数，不展示 PID、handle 或 revision。
- executor 最终通过共享 `AppContext`/socket 当前状态执行；GUI、内置 Agent 和 MCP 都能在 run 期间切换进程。

**影响**

模型针对进程 A 生成的地址，在等待审批期间若目标切到进程 B，用户批准后可能把写操作施加到 B。runId 只能隔离迟到的 UI 消息，不能校验设备目标。

**建议**

- 在 run 创建时保存 `{pid, handle, processRevision}`。
- 所有 process-bound 工具在执行前比较 revision；不一致时拒绝执行并要求模型重新获取上下文。
- 审批框明确显示 PID、进程名和 revision；批准动作绑定该快照。
- `open_process` 是显式改变目标的工具，应更新 run context，而不是绕过校验。

### A-03：工具执行存在未登记的第二层 detached 线程

**证据**

- `ChatWindow::startToolExecution()` 启动第一层 detached worker，并用 `ToolExecutor::beginToolWorker()`/`endToolWorker()` 计数。
- `ToolExecutor::execute()` 又通过 `runExecutorAsync()` 启动第二层 detached executor。
- 只读工具超时后，第一层 worker 返回并减少计数，但第二层 executor 仍可能在 socket I/O 或等待端口锁。
- `ToolExecutor::shutdown()` 只等待第一层计数，且最多等待 3 秒。

**影响**

shutdown 可能误判工具已排空；未登记 executor 仍会访问 socket、`AppContext` 或其他静态对象，存在退出期悬空访问窗口。`SocketIoTimeout::ScopedTimeout` 只给单次 I/O 设置期限，不提供线程所有权、join 或真正取消。

**建议**

- 去掉双层 detach，改为可 join 的任务队列/`jthread`，一个工具只对应一个受管任务。
- 将任务句柄、取消状态和最终结果纳入生命周期管理。
- 若保留超时语义，超时后仍应跟踪 executor 直到退出；shutdown 不应把“等待 3 秒”描述为已排空。

### A-04：配置/索引损坏可导致启动异常或覆盖原文件

**证据**

- `ApiKeyStore::loadFromFile()` 先清空 `configs_`；解析失败返回 `false`。`ChatWindow` 忽略返回值，随后立即 `seedDefaultsIfEmpty()` 和 `saveToFile()`，可把损坏的 `ai_config.json` 覆盖为空配置。
- `ApiKeyStore`、`AiSettings`、`SessionManager` 的多个 `json::value()` 位于 parse catch 之外。合法 JSON 中字段类型错误会抛 `type_error`，可逃出启动路径。
- `AiSettings::loadOrDefault()` 不区分“文件不存在”和“文件损坏”，两者都会写默认值。
- `SessionManager::init()` 忽略 `loadIndexUnlocked()` 失败；损坏索引会被空索引覆盖，使已有 `ai_sessions/*.json` 成为孤儿。
- `ChatSession::load()` 失败后仍绑定原路径；下一条消息可能覆盖损坏会话。
- `ChatSession::saveUnlocked()` 没有复用 `installTempFile()`；Windows rename 失败后退化为 `copy_file(...overwrite_existing)`，不能保证崩溃时原文件完整。

**影响**

可能丢失 provider 密钥配置、全局设置和会话索引，也可能因类型异常导致 AI Chat 初始化失败或应用退出。

**建议**

- 先解析、校验到临时对象，全部成功后再替换内存状态。
- 区分 `Missing`、`Invalid`、`IoError`，只有 `Missing` 才自动写默认文件。
- 损坏文件保留原件或改名为带时间戳的 `.corrupt`，UI/日志明确提示。
- 索引损坏时扫描会话目录重建元数据。
- `ChatSession` 统一使用经过 Windows 覆盖场景验证的原子安装助手。

### A-05：HTTP worker 的完成回调未纳入排空计数

**证据**

`HttpClient::postAsync()` 的所有结束路径都先调用 `removeFromActive()`，减少 `inFlight_`，再调用 `completeSafely()`。完成回调会继续运行 provider 逻辑，并向 `UIMessageQueue` 投递消息。

**影响**

`HttpClient::shutdown()` 可能在 `inFlight_ == 0` 时返回，但最后一个回调仍未结束。加上 3 秒有界等待，退出阶段仍可能在 UI/AI 单例开始析构后投递消息。

**建议**

- 把计数减少放到完成回调之后，用 scope guard 保证所有返回路径一致。
- 最好保留可 join 的 HTTP worker；若必须 detach，生命周期对象必须独立于静态析构顺序。
- shutdown 返回值应表明是否真正排空，超时要记录并采取明确降级策略。

### A-06：IPC client handler 不排空，响应未处理 partial send

**证据**

- `IpcServer::ServerThread()` 为每个客户端创建捕获 `this` 的 detached 线程。
- `IpcServer::Stop()` 只关闭 listen socket 并 join accept 线程，不等待已接受请求。
- `HandleClient()` 的多个响应路径只调用一次 `::send()`，没有循环发送剩余字节，也没有设置 `SO_SNDTIMEO`。
- Python `IpcClient` 对部分读方法在 timeout/`URLError` 后默认重试 2 次，但客户端 timeout 不会取消旧 C++ handler。

**影响**

handler 可在 `Stop()` 返回后继续访问 `handlers_`、socket 和共享应用状态；静态析构期存在悬空访问窗口。大型 JSON 响应还可能被截断，MCP 端表现为无效 JSON；慢客户端可能长期占住 detached handler。一次 MCP timeout 最多还会留下旧 handler 并发起新 handler，放大端口排队和资源占用。

**建议**

- 使用受管 client 线程池/任务组，在 Stop 时停止接收、关闭活动 client socket 并 join。
- 实现 `sendAll()`，处理 short write、`WSAEINTR`/错误和总发送期限。
- 为响应体设上限或分页，避免一次构建和发送超大 JSON。
- 在服务端支持 request id/cancellation 前，不要对 timeout 后仍可能运行的方法自动重试；至少把旧请求状态暴露给调用方。

### A-07：Stop 只停止编排，不停止已开始的工具

**证据**

`ChatWindow::cancelRequest()` 设置 HTTP cancellation token、清空 active run id 并把 UI 恢复到 Idle。工具 executor 没有接收该 token；迟到的 `ToolResult` 仅因 runId 不匹配而被丢弃。

**影响**

已批准的写操作仍可能完成，但会话 history/trace 不再记录最终成功或失败。用户可以立即开始新 run，旧工具仍可能占用端口或在之后生效。

**建议**

- 区分“停止生成”“请求取消工具”和“工具不可取消但仍在收尾”。
- 对已经发出的写命令，UI 在拿到确定结果前保持可见的 pending 状态，禁止把它当作已取消。
- 即使原 run 已结束，也应把晚到结果写入独立审计日志。
- 只在协议能安全中断且不会留下半包时，才把 cancellation 传入 socket 层。

### A-08：复合操作和共享设备状态不是事务

**证据**

- 端口锁只覆盖单个 request-response。
- `scan_value`/部分 fuzzy、hex 流程由 `ScanSetRange` 再调用扫描命令组成。
- `symbol_list(module_base)` 先 `SymbolInit` 再 `SymbolGetList`。
- `AppContext::selectProcess()` 包含旧目标清理、open、`SetCurrentPid`、缓存失效等多步。
- GUI、内置 Agent、IPC/MCP 共用进程、扫描结果和服务端 active symbol table。

**影响**

另一个前端可在两步之间插入请求，导致范围、符号表或当前进程被替换。每条 socket 命令都正确加锁并不能保证业务序列原子。

**建议**

- 为进程切换、扫描会话和符号会话建立更高层事务锁/epoch。
- 最可靠的方式是让服务端提供单命令复合操作或显式 session id。
- 工具执行前后校验 process/scan/symbol revision；冲突时失败而不是继续使用混合状态。

### A-09：响应、工具输出和会话载入缺少总量上限

**已有防护**

工具调用数量、工具名/id/arguments 以及多个 socket count/name length 已有局部上限。

**仍缺失**

- `SSEParser::currentLine_`、`eventBuffer_` 和 `HttpClient` 的 `accumulated` 没有总字节上限。
- provider 会持续拼接 assistant content 和 tool argument fragments。
- `ToolResult::resultJson` 与进程/符号/断点等列表的最终 JSON 没有统一输出上限。
- `ChatSession::loadUnlocked()` 在裁剪到 1000 条前先 `reserve(arr.size())`，也没有文件大小、单消息大小或总内容上限。
- 最新用户回合按设计会保留，即使单条消息已经超过 token 预算。

**影响**

异常 provider、异常设备响应或手工篡改的会话文件可造成内存暴涨、长时间 JSON 解析和 UI 卡顿。

**建议**

- 在 HTTP 原始字节、单个 SSE 事件、累计 assistant/tool arguments、工具结果和持久化文件各层设置独立硬上限。
- 大列表强制分页，UI/模型只接收摘要和有限样本。
- 会话加载先检查文件大小，再流式/限量解析；裁剪前不要按不可信数组长度 reserve。

### A-10：“OpenAI” provider 默认指向第三方兼容网关

**证据**

`OpenAIProvider::getDefaultBaseUrl()` 返回 `https://ai.ikik.net/v1`，代码注释也明确它是 OpenAI-compatible gateway。设置 UI 只校验 `https://`，不会提示域名归属或首次确认。

**影响**

用户可能根据 provider 名称误以为密钥和对话发往 OpenAI 自有域名。实际 API key、系统提示、聊天内容和工具结果会发送给该第三方主机。

**建议**

- 默认使用用户明确选择的端点；第三方网关应显示品牌、数据去向和信任提示。
- 首次向非预期域名发送密钥前要求确认。
- 在 UI 和部署文档中说明 endpoint 是安全边界，`https://` 只保证传输加密，不代表接收方可信。

### A-11：会话明文保存敏感 Agent 数据

**证据**

DPAPI 只保护 `ai_config.json` 中的 provider API key。`ChatSession` 会明文保存 assistant tool calls 和 tool role 审计；`AgentRunner::makeToolMessage()` 把 arguments、result/details 完整写入消息。

**影响**

`init_driver` 卡密、Lua 代码、地址、内存内容、符号和断点寄存器可能进入 `ai_sessions/*.json`。这些内容还会在后续回合发送给远端 provider。

**建议**

- 为敏感参数定义字段级 redaction，例如 card/key/token 永不进入 history。
- 提供“不持久化工具结果”或加密会话选项，并在 UI 说明远端 provider 数据边界。
- 对内存/寄存器结果保存摘要或用户明确选择的片段。

### A-12：全局 prompt/token 设置被会话文件覆盖

**证据**

- `ChatWindow` 构造时先把 `ai_settings.json` 的 `systemPrompt`/`tokenLimit` 写入 `session_`，随后调用 `session_.load()`。
- `ChatSession::loadUnlocked()` 又从每个会话文件读取同名字段。
- 切换会话同样直接 load；创建新会话的 `resetInMemory()` 只清消息，会继承刚才会话的 prompt/token。

**影响**

文档和设置 UI 把它们表现为全局设置，实际行为却是隐式的每会话设置。用户切换会话后，模型上下文和 token 裁剪策略可能悄然变化。

**建议**

明确选择一种模型：

- 若为全局设置：会话文件停止保存/加载这两个字段，load 后重新应用 `AiSettings`。
- 若为每会话设置：UI 显示并编辑当前会话值，索引/迁移文档也要明确。

### A-13：`symbol_*` 分类、共享状态和默认 prompt 漂移

**当前事实**

- `symbol_init`、`symbol_list` 当前注册为 `ToolSafety::ReadOnly`。
- `DefaultSystemPrompt.h` 仍把两者列为 write-classified 操作。
- 两者不修改目标内存，但会初始化/切换服务端共享 active symbol table；`symbol_list` 还可隐式执行 init。

**影响**

模型收到的安全说明与运行时审批行为不一致；同时 `ReadOnly` 容易被误解为“无共享状态副作用”。

**建议**

先定义分类语义。如果 `Write` 只表示“修改目标设备/进程”，保留当前分类但新增 `StatefulRead`/资源域元数据，并修正 prompt；如果所有共享状态变化都应审批，则重新分类。无论选择哪种，工具注册、默认 prompt、架构文档和 MCP retry 策略必须同步。

### A-14：地址字符串进制语义跨前端不一致

**证据**

- 规范 `memory_read` 已要求显式 `0x`；隐藏 `read_memory` 和尚未迁移的内置工具仍会把 `"1234"` 按十六进制解析。
- IPC `ParseAddress()` 和 MCP `helpers.parse_int()` 对 `"1234"` 按十进制解析，只有 `0x1234` 才是十六进制。

**影响**

同一个工具参数在两条 Agent 路径中可能指向不同地址，尤其危险于写内存和断点操作。

**建议**

统一为一种严格格式。迁移期间所有文档、schema 描述和模型输出都应强制地址字符串使用 `0x` 前缀；无前缀字符串应拒绝而不是猜测。

### A-15：设置草稿和 provider 删除语义不完整

**证据**

Provider、prompt 和数值设置使用可重置的 edit buffer；`proxyEnabled_`、`proxyHost_`、`proxyPort_` 是 `ChatWindow` 成员。Cancel 或标题栏关闭只清前一类 buffer，不从 `AiSettings` 重新载入代理值。

另一个独立分支是 provider 删除：用户清空已保存 API key 时，validation 会要求同时清空 endpoint；即使两者都清空并通过校验，Save 循环也会因 key 为空而 `continue`。`ApiKeyStore::removeConfig()` 没有 UI 调用路径，因此用户无法从设置面板真正删除已保存密钥。

**影响**

未保存的代理编辑会在下次打开设置时重新出现，并可能在随后一次 Save 中被意外提交。运行中的 `HttpClient` 在首次 Cancel 时尚未改变，因此 UI 和实际配置也会暂时不一致。用户以为已清除的 provider key 仍会保留在 DPAPI 配置中。

**建议**

把代理也放入统一的 edit model；打开弹窗时从持久化快照初始化，Save 才提交，Cancel/X 丢弃整个快照。为已配置 provider 提供明确的 Remove/Forget 操作，调用 `removeConfig()`，并清零包含明文 key 的临时 buffer。

### A-16：`approvalDecision` 在运行快照中几乎不可观测

`AgentController::approvePendingTool()`/`denyPendingTool()` 先设置 `Approved`/`Denied`，随后的 `updateRunFromToolOutcome()` 立即重置为 `Pending`。trace 中仍能看到批准/拒绝事件，因此功能不受影响，但 `AgentRunSnapshot.approvalDecision` 很难被 UI 或诊断代码观察到。

建议删除该瞬时字段，或定义明确的“最近一次决策”生命周期。

### A-17：内置 Agent、IPC 与 MCP 的能力面和结果契约不统一

当前静态提取结果：

- 内置 Agent：39 个可执行名称，其中 7 个隐藏兼容 alias，向 provider 广告 32 个定义。
- IPC：29 个方法。
- MCP：30 个工具，typed read/write 由 Python 映射到 IPC 的 `read_memory`/`write_memory`。
- 内置 Agent 独有 `read_disassembly`、`resolve_symbol` 等；IPC 独有 `read_batch`，但 MCP 未暴露。
- 无 LuaJIT 时，内置 `execute_lua` 仍存在并返回 unavailable；IPC 不注册该方法，而 MCP 仍对模型暴露工具。
- 名称别名包括 `get_server_version`/`get_version`、`read_breakpoint_info`/`read_bp_info`。

内置 executor 返回 JSON 字符串，再由 `ToolExecutor` 解释顶层 `error`；MCP Python 层通常把 IPC `success=false` 转成异常。三条路径的错误字段、duration、分页、截断和 feature availability 仍不同。`mcp/README.md` 原先声称暴露“全部 C++ 能力”，与实际集合不符。

首批 `status`、`process_list`、`process_open`、`memory_read` 已统一经 `MemService` 返回结构化错误和 meta，但其余工具、IPC 与 MCP 尚未迁移，因此本问题仍未关闭。

建议建立机器可读 capability registry，由内置工具、IPC 和 MCP wrapper 生成或校验各自暴露面；同时定义共享结果契约：`success`、`result`、`error`、`duration_ms`、`truncated`、`next_cursor`、`unavailable_reason`。

### A-18：截断 SSE 被当作成功响应

**证据**

- Claude 在 `message_stop` 时设置 `StreamState::completed`，完成回调从不检查该字段。
- DeepSeek 在 `[DONE]`/`finish_reason` 时设置 `finished`，完成回调也不检查。
- OpenAI streaming parser 不记录 `finish_reason`；公共 `SSEParser` 还会过滤 `[DONE]`，provider 无法用它确认结束。
- 三者在 HTTP 2xx 时都会把已累计文本/tool fragments 作为成功 `Completion`。

**影响**

服务端、代理或中间网络若正常结束 HTTP/chunked body，却没有发 provider 终止事件，部分文本会被持久化为完整回答；更危险的是半截 tool arguments 可能进入校验/执行路径。即使参数最终因 JSON 无效被拒绝，错误也会被误归为模型工具参数问题，而不是截断流。

**建议**

- 每个 streaming provider 明确记录“看见合法开始”和“看见合法终止”。
- HTTP 2xx 但缺终止事件时返回 `InvalidResponse`，保留 partial content 仅供 UI 显示，不进入工具执行。
- 公共 SSE parser 不应吞掉 provider 需要的终止语义，或应单独暴露 terminal callback。
- 增加完整、截断、重复终止、malformed event、非 SSE 2xx body 的单测。

### A-19：socket I/O 超时后复用连接可造成协议串包

**证据**

- `WindowsSocketClient::Send()`/`Receive()` 遇 `WSAETIMEDOUT` 时返回 false，但只在 reset/aborted 时 `Close()`。
- 下一次命令在持有端口锁后调用一次 `DrainPending()`，只读取当时 `FIONREAD` 已可见的数据。
- 协议没有 request id 或帧边界，多个命令共享同一字节流。

**影响**

旧响应若在 `DrainPending()` 之后、新命令发送之后才到，会被新请求当作自己的响应。超时若发生在部分 send/receive 之后，服务端和客户端对消息边界的理解已不同，瞬时 drain 无法证明重新同步。后续 read/write/scan 可能连续返回错误，甚至把错误字节解释为 count/handle/result。

**建议**

- 任意可能发生 partial send/receive 的 timeout 后，把该连接标记为 poisoned 并关闭，不再复用。
- 在受管连接层重连并重新建立当前进程/driver 状态；失败时向所有前端暴露“连接需要恢复”。
- 长期给协议增加长度、request id 或 session generation，不能靠“清空当前可读字节”恢复无帧字节流。

### A-20：连接关闭/重连与在途请求没有互斥

**证据**

- `ConnectMultiPort()`/`DisconnectMultiPort()` 直接 `Connect()`/`Close()` 三个 client，没有获取端口请求锁或全局 connection lifecycle lock。
- detached Agent executor 和 IPC handler 可同时持有/等待端口锁。
- `WindowsSocketClient::sock_` 和 `connected_` 是普通字段；`IsConnected()`、`Send()`、`Receive()`、`Close()` 可跨线程并发访问。

**影响**

用户点击断开、自动重连或退出时，可能在另一个线程的 send/recv 中关闭同一 socket，产生 C++ 数据竞争和未定义行为。即使没有崩溃，也会让请求使用旧连接 generation、旧 process handle 或新 socket 上的旧状态。

**建议**

- 为整个三端口连接建立共享/独占 lifecycle gate：请求持 shared lease，connect/disconnect/reconnect 持 exclusive lease。
- `Close()` 前取消并排空使用者；连接对象状态由同一 mutex 保护，而不是只把 bool 改为 atomic。
- 每次连接生成 generation，命令和 `AppContext` handle 绑定 generation；跨 generation 立即失败。

当前部分缓解：socket manager 已生成单调 generation，首批 `MemService` 操作在调用前后校验它。由于请求没有 shared lease、关闭没有 exclusive gate，generation 只能拒绝已观察到的陈旧结果，不能阻止并发 `Close()` 数据竞争。

### A-21：provider context 能力没有参与请求预算

**证据**

- provider 声明 `maxContextTokens`：OpenAI 128k、Claude 200k、DeepSeek 64k。
- 除声明外没有代码读取 `getCapabilities()`。
- `AiSettings` 允许 `tokenLimit` 到 1,000,000；`AiSettings.h` 注释仍写 200,000。
- `ChatSession::estimateTokenCount()` 仅用消息 UTF-8 字节数/4，未计当前 32 个广告工具 schema、provider JSON 开销或输出 token 预留。

**影响**

用户选择 DeepSeek/自定义模型后仍可保留远超上下文的历史，导致 provider 400 或反复失败。中文、tool audit 和大型 schema 下估算误差更明显。

**建议**

- 请求预算取用户上限、当前 provider/model 上限和 endpoint 配置的最小值。
- 预留输出 token 与工具 schema/序列化开销；至少使用分语言更保守估算，理想情况下使用对应 tokenizer。
- 自定义兼容 endpoint 允许用户显式配置 model context，而不是沿用 provider 类硬编码。
- 清理 `AiSettings.h` 与实现的 clamp 漂移。

### A-22：核心路径缺少自动回归测试

仓库已有 `NativeAgentMemTests`/`native_agent_mem_service`，覆盖首批地址、分页、结果契约、target/generation、取消/期限和隐藏 alias。以下纯逻辑/协议边界仍缺自动化：

- 三类 provider 的 SSE/full-response parser 和终止语义。
- `ChatSession::getMessagesForRequest()` 的 tool call/result 配对。
- config/index 损坏与错误字段类型。
- ToolExecutor 完整 schema、错误提取、预算/审批状态机（当前只覆盖隐藏 alias 注册语义）。
- IPC HTTP parser、partial send、auth 和 capability。
- 假 socket 上的 timeout、迟到响应、重连 generation。
- C++/IPC/MCP capability 和常量对齐。

建议先建立不依赖 Android 设备的单元/契约测试，再保留少量真实设备 smoke test。否则当前文档中的安全不变量无法在后续重构中自动守住。

## 已修复摘要

以下旧问题已在当前代码中落地，不再把旧的“现状/影响”保留为待修复描述：

| 项目 | 当前实现 |
|------|----------|
| executor 返回顶层 `{"error": ...}` 却标成功 | `ToolExecutor::extractToolError()` 会转为 `success=false`，`AgentRunner` 保留 details |
| Claude 结构化错误丢失 | `ChatWindow::pollMessages()` 能统一处理 `CompletionResponse.error` |
| 多个 socket 可变长度响应缺少边界 | 进程、模块、符号、冻结、断点和扫描结果的主要读取点已增加 count/name 上限 |
| `FetchProcessList()` 半截读取仍返回成功 | 条目或名称读取失败会返回失败 |
| `InitDriver()` 长度未校验 | card 和返回消息已有非空/长度检查 |
| `OpenProcessHandle()` 接受 handle 0 | 底层和 `AppContext` 会把 handle 0 视为失败 |
| IPC 状态行固定写 `OK` | `BuildHttpResponse()` 已按状态码输出 reason phrase |
| IPC 请求 method/path/Content-Length 过于宽松 | 已校验 `POST`/`OPTIONS`、根路径、长度和 1 MiB 请求上限 |
| IPC `open_process` 重复打开/保留旧 handle | 已统一经 `AppContext::selectProcess()` 并读取最终状态 |

## 不应误认为“已闭环”的缓解

- `runId` 过滤只防止迟到消息污染当前 UI，不会停止网络或工具副作用。
- `SocketIoTimeout` 限制 I/O 等待时间，不提供任务 join、事务回滚或写操作取消。
- 3 秒 `shutdown()` 等待是 best effort，不是“所有 detached worker 已退出”的证明。
- DPAPI 只保护 provider API key，不保护会话、工具参数或结果。
- IPC 绑定 loopback 可缩小暴露面，但在无鉴权且允许 CORS 时不是完整安全边界。
- `installTempFile()` 已用于 `ApiKeyStore`、`AiSettings`、`SessionManager`，但 `ChatSession` 仍有独立且较弱的替换路径。
- `DrainPending()` 只清理调用瞬间已到达的数据，不能证明 timeout 后的无帧 socket 已重新同步。
- `ProviderCapabilities` 当前只是声明，不会自动保护请求不超过模型 context。

## 建议修复顺序

1. 关闭 IPC 的浏览器暴露并加入认证；同时让审批绑定 `{pid, handle, revision}`。
2. timeout 后停止复用 poisoned socket，并为 connect/disconnect/请求建立 generation 和生命周期互斥。
3. 统一重构 HTTP、工具和 IPC handler 为可管理、可 join 的任务生命周期。
4. 修复配置/索引的事务式加载和损坏文件保留，统一会话原子写。
5. 校验 provider 流式终止事件，并建立无需设备的 parser/config/state-machine 回归测试。
6. 为 Stop、写工具晚到结果和复合设备操作建立明确状态/事务边界。
7. 增加端到端资源/context 上限、IPC `sendAll()` 和列表分页。
8. 明确第三方 endpoint、会话明文与 provider key 删除策略。
9. 最后统一全局/会话设置、安全分类、地址格式、能力矩阵和跨前端结果契约。
