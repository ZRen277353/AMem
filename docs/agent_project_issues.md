# Agent 业务代码审计与问题清单

审计日期：2026-07-13
适用分支：`NativeAgent`（基线来自 `AIChat`）
审计范围：`gui/ai/`、`mem/MemJsonTools.*`、`ipc/IpcProtocol.*`、`ipc/IpcFramedConnection.*`、`ipc/IpcHandshakeSession.*`、`ipc/IpcRequestProtocol.*`、`ipc/IpcRequestSession.*`、`ipc/IpcMethodCatalog.*`、`ipc/IpcMemServiceDispatcher.*`、`ipc/NamedPipeServer.*`、`ipc/NativeAgentRuntime.*`、`ipc/NativePipeSecurity.*`、`ipc/IpcServer.*`、`socket/` 中被 Agent/IPC 调用的命令层、`gui/AppContext.*`、`main.cpp`。

本文记录当前工作区中仍存在的问题，以及已经落地的修复摘要。目标架构和分阶段关闭方案见 [`native_agent_refactor_plan.md`](./native_agent_refactor_plan.md)。搜索/扫描协议自身的问题不在本文审计范围内。

本次结论来自静态代码审阅、调用链核对、native framing/framed I/O/Hello/request-session/本机 Named Pipe transport 无设备测试，没有连接 Android 设备，也没有执行 provider、不同用户/remote IPC 或应用退出阶段的端到端压力测试。优先级含义：

- **P0**：可能造成未授权调用、错误目标写入、协议串包、并发数据竞争、启动数据破坏或退出期悬空访问，应优先处理。
- **P1**：明显影响可靠性、审计完整性、资源边界或敏感数据安全。
- **P2**：语义漂移、跨前端不一致或可恢复的 UX/维护性问题。
- **P3**：低影响可观测性问题。

## 结论摘要

| ID | 优先级 | 状态 | 问题 |
|----|--------|------|------|
| A-01 | P0 | 部分修复 | 默认构建不再监听；opt-in IPC 仍无鉴权且允许任意 CORS |
| A-02 | P0 | 已修复 | 当前内置工具均在 service/host send 边界消费 run target；退役 executor 已删除 |
| A-03 | P0 | 已修复 | 工具执行由单个 joinable worker 所有，shutdown 有 join 测试 |
| A-04 | P0 | 未修复 | 配置/索引损坏可导致启动异常或覆盖原文件 |
| A-05 | P1 | 未修复 | HTTP worker 在完成回调前就从 in-flight 计数移除 |
| A-06 | P1 | 部分修复 | 默认构建不含 handler；opt-in IPC 仍不排空且未处理 partial send |
| A-07 | P1 | 已修复 | Stop 保留真实取消语义；mutation 晚到回执在 UI callback 前进入独立审计 |
| A-08 | P1 | 未修复 | 复合工具和进程切换不是事务，可被其他前端插入 |
| A-09 | P1 | 未修复 | HTTP、模型内容、工具输出和会话载入缺少总量上限 |
| A-10 | P1 | 待安全决策 | “OpenAI” provider 默认指向第三方兼容网关 |
| A-11 | P1 | 部分修复 | driver card 已脱敏；其他敏感工具参数、结果和脚本仍明文保存 |
| A-12 | P2 | 未修复 | 全局 prompt/token 设置会被会话文件反向覆盖 |
| A-13 | P2 | 已修复 | `symbol_*` 按当前二元安全模型统一分类、事务语义和默认 prompt |
| A-14 | P2 | 部分修复 | 默认构建只有严格 Agent；opt-in HTTP IPC 仍接受无前缀十进制地址 |
| A-15 | P2 | 未修复 | 设置草稿不完整，且无法从 UI 删除 provider key |
| A-16 | P3 | 未修复 | `approvalDecision` 在快照中几乎不可观测 |
| A-17 | P2 | 部分修复 | Python MCP 已删除；内置 Agent 与 opt-in HTTP IPC 的契约仍未统一 |
| A-18 | P1 | 未修复 | 三类 provider 都会把缺少终止事件的截断 SSE 当成功 |
| A-19 | P0 | 部分修复 | I/O 失败会 poison 并拒绝复用；仍缺 fake transport 迟到响应测试 |
| A-20 | P0 | 部分修复 | 请求/lifecycle lease 已落地；仍缺真实 client 并发压力测试 |
| A-21 | P2 | 未修复 | provider context 能力未参与 token 裁剪和输出预留 |
| A-22 | P2 | 部分修复 | 已覆盖 MemService、target、连接和工具 worker；provider/IPC 等路径仍缺回归测试 |

## 当前未解决问题

### A-01：IPC 无鉴权且允许任意 CORS

**证据**

- `ENABLE_LEGACY_HTTP_IPC` 默认 OFF；默认构建不加入 `IpcServer.cpp`，`main.cpp` 的 include/start/stop 也受 `HAVE_LEGACY_HTTP_IPC` 约束。
- 显式启用时，`IpcServer::Start()` 绑定 `127.0.0.1:28100`，但没有认证 token、客户端身份或方法级 capability；CMake 会打印未鉴权端口警告。
- `IpcServer::HandleClient()` 接受浏览器 `OPTIONS` 预检。
- `IpcServer::BuildHttpResponse()` 返回 `Access-Control-Allow-Origin: *`。
- 同一入口暴露 `write_memory`、断点、扫描、`execute_lua`、进程切换等有副作用的方法。
- Native transport 已有 protected 当前用户/SYSTEM read-write DACL、`PIPE_REJECT_REMOTE_CLIENTS`、单实例所有权、有界 framed I/O、Observe-only Hello、严格 request session、完整 24-name catalog、12 个 Observe `MemService` adapter、connection/target baseline invalidation、owned runtime composition 和显式 GUI status/control。`ENABLE_NATIVE_IPC` 默认 OFF，编译后 runtime 也默认 stopped；main 不自动启动，只在设备断连前 shutdown。privileged broker core 已接 system owner 与 bounded GUI decision，Stop 会 cancel-all；但尚无 product submission、session/dispatcher consume 或 persistent audit。

**影响**

默认发行构建不再暴露 legacy 监听面，也不创建 native pipe。legacy 风险只在显式迁移构建中存在，但一旦启用，回环地址仍只阻止远端主机直接连接，并不阻止本机浏览器页面或其他本地进程访问。当前 CORS 配置还主动允许网页脚本读取响应；浏览器的 Private Network Access 策略不能作为服务端鉴权。Native DACL/remote rejection 只建立主体边界，尚不能替代 capability 与危险操作审批。

**建议**

1. 保持 default-off gate，不把迁移选项暴露为普通用户设置；若继续保留 opt-in，先移除 CORS 和 `OPTIONS` 支持。
2. 若确认需要外部自动化，保持已落地 GUI control 的 compile/runtime 双重默认关闭，不恢复 Python MCP。
3. 将所有 target selection/mutation 与 Lua 请求接入已落地 broker management surface；session cancel、consume/send-time target recheck 和持久化 audit 必须一起完成。
4. 若没有明确外部调用方，删除整个 IPC source 和 CMake wiring。

### A-02：Agent 目标绑定已覆盖当前所有实际 send 边界（已修复）

**证据**

- `AgentRunContext` 在 `resetForNewRun()` 时保存 connection generation 和 target snapshot，并绑定当前 cancellation token。
- `ToolRegistration` 已声明 `None`、`Bound` 或 `Selection`；`AgentController` 在等待审批前、批准出队前和成功结果接收前复核快照。
- 审批框显示预期 connection generation、PID 和 process revision。
- 23 个非 Lua canonical 工具的 adapter 均消费显式 `OperationContext` 并在 `MemService` 边界复核；`process_open` 在 send 前再次比较旧 selection，并只在返回快照与当前状态一致时推进 run target。
- `lua_execute` 在 host 执行前复核 target/generation；`ToolDefinitions.cpp` 已删除全部 33 个 hidden alias 和 direct-socket executor，并由 `native_agent_catalog` CTest 阻止重新依赖 `client_singleton.h`/`AppContext.h`。
- GUI `BreakpointWindow` 的 set/remove/enable/suspend/resume 与 hit refresh 已显式注入 `IMemService`，每次调用捕获 target context；命中 batch 保留完整 GPR/FPSIMD，按最新 50,000 条有界。
- GUI `ScanWindow` 的 start/refine/results/clear/remove 已显式注入 `IMemService`，每次调用捕获 target context 并携带最新 scan epoch；Stop 通过共享 cancellation token 进入同一 service/backend 调用。
- 无设备测试覆盖审批期间切进程/重连、批准后 send 前切进程、同批 `process_open -> memory_read` 和晚到成功结果拒绝。

**影响**

当前注册表只包含 canonical 工具。所有 process-bound 工具在 Controller 校验后，还会在实际 service/host 边界再次验证 snapshot；scan/symbol session 绑定 target+epoch，breakpoint/driver 保留确认/未知回执。退役名称不再可执行，旧会话中的调用组只会在 provider 边界降级为不可执行的 assistant 历史文本。因此原先“legacy executor 在校验后读取新共享状态并向错误目标发送”的路径已关闭。

**建议**

- 保持 `native_agent_catalog` 的名称和依赖检查，不重新引入第二套模型可见名称或 direct-socket executor。
- 将 process name 加入审批显示，并把 effect、资源域和规范化参数写入持久审计。
- 所有 target selection/mutation 的 fake backend 测试必须覆盖“校验后、send 前切换”以及 completion unknown。

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

- 默认构建不编译 `IpcServer.cpp`，以下问题只存在于 `ENABLE_LEGACY_HTTP_IPC=ON` 的迁移构建。
- `IpcServer::ServerThread()` 为每个客户端创建捕获 `this` 的 detached 线程。
- `IpcServer::Stop()` 只关闭 listen socket 并 join accept 线程，不等待已接受请求。
- `HandleClient()` 的多个响应路径只调用一次 `::send()`，没有循环发送剩余字节，也没有设置 `SO_SNDTIMEO`。
- 新 `NativeAgentRuntime` 不使用 detached handler：`NamedPipeServer` 的 accept 与串行 handler 在一个 owned thread 上，请求 worker 由 session 持有；stop event + `CancelIoEx` 会取消握手/请求并 join 两层 worker。显式 GUI Stop 和 main 退出都会走该边界，但它尚未替代 legacy HTTP。

**影响**

默认发行构建不具备该退出风险。显式启用后，handler 仍可在 `Stop()` 返回后继续访问 `handlers_`、socket 和共享应用状态；静态析构期存在悬空访问窗口。大型 JSON 响应还可能被截断，任意客户端都会看到无效 JSON；慢客户端可能长期占住 detached handler。调用方 timeout 不会取消旧 handler，若自行重试还会放大端口排队和资源占用。

**建议**

- 使用受管 client 线程池/任务组，在 Stop 时停止接收、关闭活动 client socket 并 join。
- 实现 `sendAll()`，处理 short write、`WSAEINTR`/错误和总发送期限。
- 为响应体设上限或分页，避免一次构建和发送超大 JSON。
- 在服务端支持 request id/cancellation 前，不要对 timeout 后仍可能运行的方法自动重试；至少把旧请求状态暴露给调用方。

### A-07：Stop 后已发送操作的最终状态独立可见（已修复）

**证据**

`ChatWindow::cancelRequest()` 同时设置 run token 并调用 `AgentTaskExecutor::cancelRun()`。排队任务成为 `cancelled_before_start`，迁移到 `MemService` 的 active 操作观察 cancellation；canonical 长扫描请求 DEBUG stop，memory/breakpoint/driver/Lua mutation 保留 confirmed-after-cancel/deadline 或 `completion_unknown`。活动 mutation 的 Stop notice 明确显示 cancel requested。Clear、New、session switch/delete 和窗口析构也统一调用 active-run cancellation。

`AgentTaskExecutor::deliver()` 在任何 completion callback 前将 write-classified 和 symbol-session effect 写入 `AgentMutationAuditLog`。因此即使 callback 抛异常、原 run 已清空或 `UIMessageQueue` 被 run-id gate 丢弃，最终状态仍保存在 `ai_mutation_audit.jsonl` 并出现在 Audit 表。manual deny 与 queue rejection 也有记录。

**影响**

已发送副作用仍不能撤回，迟到结果也不会重新写入已结束的聊天 session；这是刻意的隔离。最终 completion、approval、effect/resource domain、run/tool-call id 和预期 target 改由独立审计承担。当前 service/host executor 都消费 context，但阻塞中的设备或 Lua 调用仍可能只在检查点、socket deadline 或返回时响应取消；worker ownership 和最终 outcome 不会丢失。

**建议**

剩余约束：只有协议能安全中断且不会留下半包时，才把 cancellation 进一步传入 socket 层。独立日志是 4 MiB active + 一个轮转备份的明文摘要，不是不可篡改或远程合规审计。

### A-08：复合操作和共享设备状态不是事务

**证据**

- 端口锁只覆盖单个 request-response。
- 每条 `SocketCommand` 现会经过可重入 per-port transaction gate；canonical pointer、scan 与 symbol 已长期持有相应 transaction，process selection 尚未完成同等级的业务事务/revision。
- canonical `scan_start` 已合并 range+scan，结果/refine/clear 绑定 monotonic epoch；GUI start/refine/results/clear/remove 已迁入同一 `IMemService` session。旧 IPC 仍可能分步调用，但会推进 epoch 并使 native/GUI session 失效。
- GUI symbol cache 已用 `loadSymbolTable` 在一个 transaction 内完成一次 init 与全表读取；旧 IPC 仍会分开调用 `SymbolInit` 和 `SymbolGetList`。canonical `symbol_list` 用 epoch 约束续页。
- `AppContext::selectProcess()` 包含旧目标清理、open、`SetCurrentPid`、缓存失效等多步。
- GUI、内置 Agent，以及显式启用时的 HTTP IPC 共用进程、扫描结果和服务端 active symbol table。

**影响**

默认构建已移除 HTTP caller，但 GUI/Agent 并发和 process-selection 多步流程仍不是完整事务。显式启用 IPC 后，另一个前端还可在 legacy scan/symbol 或 process-selection 两步之间插入请求。canonical scan/symbol 会检测冲突，但旧调用自身仍没有同等级的 completion/session 契约。

**建议**

- 继续把旧 IPC scan/symbol 调用迁入 service，并为进程切换建立明确 revision；持 gate 时不得反向获取 process-state mutex。
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

driver card 已通过 `ToolCallSecurity` 从审批显示、tool audit、`ai_sessions/*.json` 和 mutation audit 脱敏，原值只为执行和当前 tool-call 连续性暂存在进程内。独立 audit 也省略 Lua code/error/output、raw memory data、register/hit/items 和大字段，但它仍是明文。Lua 代码、地址、内存内容、符号和断点寄存器仍可能进入会话文件，并在后续回合发送给远端 provider，因此本项仍未关闭。

**建议**

- 把现有 driver card 字段级 redaction 扩展为 catalog 元数据，覆盖后续所有 card/key/token，而不是继续维护工具名特判。
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

### A-13：`symbol_*` 分类、共享状态和默认 prompt 漂移（已按当前二元模型解决）

当前二元 `ToolSafety` 明确定义为：`Write` 表示需要审批的目标/主机 mutation。规范 `symbol_resolve`/`symbol_list` 不修改目标内存，保持 ReadOnly；active table 的内部 session mutation 由 MAIN transaction 和 symbol epoch 约束。`symbol_init`、`symbol_find`、`resolve_symbol` 已从注册表删除，默认 prompt 只列规范名称且不再把 symbol list 写成需要写审批。

剩余改进是引入设计文档中的 richer effect/resource metadata，把 symbol 操作标为 `SessionMutation` 并记录资源域审计。该扩展不能重新引入模型可见的 init 前置步骤，也不能把“可安全自动重试”与 ReadOnly 自动等同。

### A-14：地址字符串进制语义跨前端不一致

**证据**

- 内置 Agent 的 raw/typed memory、scan ranges、disassembly、pointer offsets 和 breakpoint 地址均要求显式 `0x`；退役名称不可执行。
- 显式启用的 IPC `ParseAddress()` 对 `"1234"` 按十进制解析，只有 `0x1234` 才是十六进制。

**影响**

默认构建没有第二个解析入口。显式启用 HTTP IPC 后，无前缀字符串在内置 Agent 中被拒绝，在 IPC 中却会被接受为十进制；调用方跨入口复用参数时会得到不同结果，写内存和断点操作尤其危险。

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

### A-17：内置 Agent 与 opt-in HTTP IPC 的能力面和结果契约不统一

当前静态提取结果：

- 内置 Agent（LuaJIT）：24 个 canonical 名称，24 个可执行、24 个向 provider 广告、0 个 hidden alias；无 LuaJIT 时为 23/23/0。
- Opt-in IPC：29 个方法；默认构建为 0。
- Python MCP package、`.mcp.json`、安装元数据和 IDE 配置已经删除，不再形成第三套可调用面。
- 内置 Agent 独有 canonical `disassemble`、`symbol_resolve` 等；IPC 独有 `read_batch`，并继续使用 legacy method 名称。
- 无 LuaJIT 时，内置 `lua_execute` 不注册；IPC 也不注册对应方法，但 availability/error 契约不同。

内置 executor 返回 JSON 字符串，再由 `ToolExecutor` 解释顶层 `error`；IPC 返回 HTTP JSON。两条路径的名称、错误字段、duration、分页、截断、审批、target binding 和 feature availability 仍不同。

`status`、`driver_initialize`、`process_list`、`process_open`、module/pointer/disassembly/symbol resolution、canonical scan/breakpoint、raw/typed memory read/write 已统一经 `MemService` 返回结构化错误和 meta。scan/symbol 返回 epoch 和分页；driver/scan/breakpoint mutation 返回明确 completion。GUI scan 也使用相同 session contract：范围+start、count+page、count-confirmed remove 与 token-driven Stop 不再直接拼装 socket 命令。breakpoint hits 使用无 cursor 的最新批次，Agent 上限 100 并报告 `available/dropped`；module 匹配拒绝歧义，typed value 由 `ValueCodec` 统一范围、字节序和精度文本。`lua_execute` 具有 feature gate、host target 校验和 deadline 回执。Python MCP 和内置 legacy 工具均已删除，HTTP IPC 默认关闭但 opt-in 实现尚未迁移，因此本问题仍未关闭。

建议建立机器可读 capability registry，校验内置工具与后续受限 transport 的暴露面；同时定义共享结果契约：`success`、`result`、`error`、`duration_ms`、`truncated`、`next_cursor`、`unavailable_reason`。不要为临时 HTTP IPC继续扩展第二套 schema。

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

- `WindowsSocketClient::Send()`/`Receive()` 的任意 I/O 错误、EOF 或 partial failure 都会先 `MarkPoisoned()` 再关闭当前 client。
- 首次 poison 推进 connection generation，`DeviceSession::AcquireRequest()` 随后拒绝新请求；旧的待处理字节清理恢复路径已删除。
- Debug/Release 的无设备测试验证 poison 使在途 lease 过期且拒绝后续 lease。

**影响**

旧连接现在不会继续承载下一请求，原串包路径已被结构性阻断。剩余风险是缺少可注入 socket transport，尚未自动验证真实 partial send、迟到响应、三端口中单端口失败和重连后的协议恢复。

**建议**

- 增加 fake transport/loopback fixture，证明 timeout 后旧字节不能进入新 generation。
- 明确重连后的 driver/process/scan 恢复策略；breakpoint cleanup tracker 现会随 disconnect 清空，但远端状态仍不承诺自动恢复。
- 长期给协议增加长度、request id 或 session generation。

### A-20：连接关闭/重连与在途请求没有互斥

**证据**

- `SocketCommand::execute*()`、open/close handle 都持有 shared request lease；connect/disconnect 持有 exclusive lifecycle lease 后才替换或关闭三个 client。
- 同线程嵌套命令复用同一个 shared lease，进程切换保持 connection -> process -> port 锁顺序。
- 应用退出在静态析构前显式 `DisconnectMultiPort()`；无设备测试验证 lifecycle 必须等待 request lease 释放。

**影响**

已知命令入口现在受 lifecycle gate 保护。仍需对真实三个 `WindowsSocketClient` 做并发 connect/disconnect/poison 压力测试，并继续收窄公开 `GetClient()`，防止后续代码绕过 lease。

**建议**

- 增加真实/fake client 压力测试以及锁顺序断言。
- 将 `GetClient()` 和端口 mutex 收窄到协议层，业务调用只能经过 session-aware command/service。
- 为退出、自动重连和 poison 后手动重连记录可重复 smoke 结果。

### A-21：provider context 能力没有参与请求预算

**证据**

- provider 声明 `maxContextTokens`：OpenAI 128k、Claude 200k、DeepSeek 64k。
- 除声明外没有代码读取 `getCapabilities()`。
- `AiSettings` 允许 `tokenLimit` 到 1,000,000；`AiSettings.h` 注释仍写 200,000。
- `ChatSession::estimateTokenCount()` 仅用消息 UTF-8 字节数/4，未计当前 24 个广告工具 schema、provider JSON 开销或输出 token 预留。

**影响**

用户选择 DeepSeek/自定义模型后仍可保留远超上下文的历史，导致 provider 400 或反复失败。中文、tool audit 和大型 schema 下估算误差更明显。

**建议**

- 请求预算取用户上限、当前 provider/model 上限和 endpoint 配置的最小值。
- 预留输出 token 与工具 schema/序列化开销；至少使用分语言更保守估算，理想情况下使用对应 tokenizer。
- 自定义兼容 endpoint 允许用户显式配置 model context，而不是沿用 provider 类硬编码。
- 清理 `AiSettings.h` 与实现的 clamp 漂移。

### A-22：核心路径缺少自动回归测试

仓库已有 `NativeAgentMemTests`/`native_agent_mem_service` 的 23 个测试组，覆盖地址/scalar codec、driver receipt/card redaction、进程与模块分页/解析、事务化 pointer resolution、disassembly、scan/symbol session/full-table transaction、breakpoint receipt/rich hit batch、scan 取消/完成未知、mutation audit 脱敏/轮转/晚到 callback 前写盘、target/generation、raw/typed write 完成语义、连接 lease/poison、审批失效、同批 target 推进、非目标工具、排队取消/timeout、active cancellation、shutdown join、晚到结果拒绝和 33 个退役工具的历史降级。Native IPC 有 7 组 approval-broker、5 组 protocol、5 组 transport、7 组 framed-I/O、8 组 handshake、6 组 request-contract、9 组 request-session、4 组 method-catalog、5 组 MemService-dispatcher 和 7 组 runtime 测试。broker 新增 pending+approved cancel-all；native gate 固定 GUI decision 不读取 params/results、Stop 取消审批且 Hello 仍 Observe-only。Debug/Release 当前各有 15 项 CTest。以下纯逻辑/协议边界仍缺自动化：

- 三类 provider 的 SSE/full-response parser 和终止语义。
- `ChatSession::getMessagesForRequest()` 的通用 tool call/result 配对和预算裁剪。
- config/index 损坏与错误字段类型。
- ToolExecutor 完整 schema、预算上限和 auto-approve/denial 组合。
- Legacy IPC HTTP parser/partial send，以及 Native IPC broker submission/session/dispatcher/persistent-audit integration 与 GUI click smoke。
- 不同 Windows 用户/session 与真实 remote client 的 native transport 负向测试。
- fake transport 上的 partial I/O、timeout、迟到响应和三端口重连。
- C++ Agent/IPC capability、结果和 feature gate 对齐。

建议先建立不依赖 Android 设备的单元/契约测试，再保留少量真实设备 smoke test。否则当前文档中的安全不变量无法在后续重构中自动守住。

## 已修复摘要

以下旧问题已在当前代码中落地，不再把旧的“现状/影响”保留为待修复描述：

| 项目 | 当前实现 |
|------|----------|
| Python MCP 复制工具 schema、常量和 retry 语义 | FastMCP package、安装入口、IDE 配置和 `.mcp.json` 已删除；仅保留不参与产品运行的标准库协议排障脚本 |
| HTTP IPC 默认监听未鉴权端口 | `ENABLE_LEGACY_HTTP_IPC` 默认 OFF；标准构建不编译 `IpcServer.cpp`，main 的 include/start/stop 也受 compile gate 约束 |
| Native IPC wire contract 未固定 | `IpcProtocol` 使用显式 24-byte little-endian header、精确版本与 request-id 规则、UTF-8、request 1 MiB 与其他帧 4 MiB 硬上限；partial frame 不消费输入并有独立 CTest |
| Native IPC 身份与 handler 生命周期没有基础边界 | protected DACL 只允许当前进程用户和 SYSTEM read/write；拒绝 remote client，单实例 handle 持续占有名称；accept/handler 同属一个 joinable thread，stop event + `CancelIoEx` 后 join；产品仍不启动 |
| Native IPC 协议阶段缺少统一 runtime owner | `NativeAgentRuntime` 持有 server，串起 framed connection/Hello/Observe dispatcher/request session；Stop 取消并 join handler/worker，线程安全 snapshot 不保存请求参数或结果；产品默认 stopped，仅允许用户显式启用 Observe |
| Native IPC privileged approval 没有可验证状态机 | `IpcApprovalBroker` 由 catalog 推导 metadata，bounded record 不含 params/results；approval 在 consume 前可失效，一次性 grant 绑定 session/request/generation/target；尚未接产品调用链 |
| Agent breakpoint 直连 socket 且回执/本地 tracker 分离 | 五个规范工具经 `MemService` 绑定 target；mutation 区分未发送/拒绝/完成未知/确认完成，设备确认与 cleanup tracker 在同一 transaction 更新，hits 使用有界最新批次且不伪造 cursor |
| Agent symbol 依赖 active table 前置状态 | `symbol_resolve`/`symbol_list` 在一个事务内完成 module resolve + init + find/page；续页绑定 epoch，旧前端 init 会使其失效 |
| Agent scan 依赖 set-range 前置状态且跨前端不可检测 | `scan_start` 一次提交完整请求；refine/results/clear 绑定 epoch，所有旧 scan mutation 也推进 epoch；sent-without-terminal 返回 `completion_unknown` |
| pointer chain 的 module/read 可被其他前端插入 | 可重入 per-port transaction gate 覆盖所有普通命令；`pointer_resolve` 和旧 GUI/Lua/IPC helper 在 module list 到最后一次 read 期间持续持有 |
| module 列表/解析直连 socket 且子串取首项 | `MemService::listModules/resolveModule` 统一分页、target 校验和完整名/basename/唯一子串匹配；歧义与异常范围会失败 |
| typed read/write 重复解析和直连 socket | `ValueCodec` + `MemService::readValue/writeValue` 统一 scalar 范围/字节序；规范工具要求 `0x`，旧名称已退役且不可执行 |
| 工具路径两层 detached worker | `AgentTaskExecutor` 独占 joinable worker；`ToolExecutor` 同步执行，shutdown 测试证明 active/queued task 排空后 join |
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
- `SocketIoTimeout` 现在消费 task absolute deadline，但不提供事务回滚或撤回已经发送的写命令。
- `AgentTaskExecutor::shutdown()` 会 join；`HttpClient` 的 3 秒 bounded wait 仍不是 HTTP worker 已全部退出的证明。
- DPAPI 只保护 provider API key，不保护会话、工具参数或结果。
- IPC 绑定 loopback 可缩小暴露面，但在无鉴权且允许 CORS 时不是完整安全边界；native DACL/remote rejection 提供身份边界，Observe-only session 提供当前 capability 边界，但仍没有 privileged approval。
- `installTempFile()` 已用于 `ApiKeyStore`、`AiSettings`、`SessionManager`，但 `ChatSession` 仍有独立且较弱的替换路径。
- `DeviceSession` 已删除待处理字节清理恢复路径并 poison 失败连接，但尚缺 fake transport 对迟到字节/partial I/O 的完整证明。
- `ProviderCapabilities` 当前只是声明，不会自动保护请求不超过模型 context。

## 建议修复顺序

1. 保持 HTTP IPC 与 native runtime default-off；把已落地 broker management surface 接入 request submission、session lifecycle、dispatcher consume/send boundary 和持久化审计。整条链完成前不能 grant privileged capability，也不能默认开启旧端口。
2. 为已落地的 poison/lifecycle gate 增加 fake transport 与真实设备压力证明。
3. 删除 legacy HTTP detached handler；native transport 已具备 join 基础，后续 frame/session handler 必须保持同一受管生命周期。
4. 修复配置/索引的事务式加载和损坏文件保留，统一会话原子写。
5. 校验 provider 流式终止事件，并建立无需设备的 parser/config/state-machine 回归测试。
6. 为 Stop、写工具晚到结果和复合设备操作建立明确状态/事务边界。
7. 增加端到端资源/context 上限、IPC `sendAll()` 和列表分页。
8. 明确第三方 endpoint、会话明文与 provider key 删除策略。
9. 最后统一全局/会话设置、安全分类、地址格式、能力矩阵和跨前端结果契约。
