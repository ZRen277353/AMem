# Agent 业务代码审计与问题清单

审计日期：2026-07-13
适用分支：`NativeAgent`（基线来自 `AIChat`）
审计范围：`gui/`、`lua/`、`mem/`、`gui/ai/`、native `ipc/`、`socket/` 中被产品调用的命令层、`gui/AppContext.*`、`main.cpp` 和对应 CTest/static gate。

本文记录当前工作区中仍存在的问题，以及已经落地的修复摘要。目标架构和分阶段关闭方案见 [`native_agent_refactor_plan.md`](./native_agent_refactor_plan.md)。搜索/扫描协议自身的问题不在本文审计范围内。

本次结论来自静态代码审阅、调用链核对、provider SSE parser/terminal state machine、本机自签名 HTTPS full-response、native framing/framed I/O/Hello/request-session/Named Pipe transport 无设备测试，以及 2026-07-13 的真实 Android/外部 OpenAI-compatible provider 验收。尚未执行不同 Windows 用户/remote IPC、Native IPC 真实 privileged request、三端口故障恢复或应用退出阶段的端到端压力测试。优先级含义：

- **P0**：可能造成未授权调用、错误目标写入、协议串包、并发数据竞争、启动数据破坏或退出期悬空访问，应优先处理。
- **P1**：明显影响可靠性、审计完整性、资源边界或敏感数据安全。
- **P2**：语义漂移、跨前端不一致或可恢复的 UX/维护性问题。
- **P3**：低影响可观测性问题。

## 结论摘要

| ID | 优先级 | 状态 | 问题 |
|----|--------|------|------|
| A-01 | P0 | 已修复 | legacy HTTP/CORS 控制面已删除；Native IPC 使用 DACL、Observe-only Hello 与逐请求审批 |
| A-02 | P0 | 已修复 | 当前内置工具均在 service/host send 边界消费 run target；退役 executor 已删除 |
| A-03 | P0 | 已修复 | 工具执行由单个 joinable worker 所有，shutdown 有 join 测试 |
| A-04 | P0 | 已修复 | 配置/会话事务式加载；损坏索引可保留并扫描重建，失败绑定不会写回 |
| A-05 | P1 | 已修复 | HTTP request thread 受管且 completion callback 返回后才可回收；shutdown 全量 join |
| A-06 | P1 | 已修复 | legacy detached handler 已删除；Native IPC exact I/O、取消与 join 已覆盖 |
| A-07 | P1 | 已修复 | Stop 保留真实取消语义；mutation 晚到回执在 UI callback 前进入独立审计 |
| A-08 | P1 | 已修复 | pointer/scan/symbol/breakpoint/process selection 均有 service transaction/revision/epoch owner |
| A-09 | P1 | 已修复 | HTTP/SSE、provider 累积、工具输出和会话载入均有分层硬上限与回归 |
| A-10 | P1 | 已修复 | OpenAI 默认使用官方 endpoint；自定义 endpoint 需显式、URL-bound trust |
| A-11 | P1 | 已修复 | 可用 session/index 使用当前用户 DPAPI；有效旧明文事务迁移，无效文件不绑定 |
| A-12 | P2 | 已修复 | `AiSettings` 独占全局 prompt/token；session v2 只保存历史，v1 字段迁移时忽略 |
| A-13 | P2 | 已修复 | `symbol_*` 按当前二元安全模型统一分类、事务语义和默认 prompt |
| A-14 | P2 | 已修复 | 内置 Agent 与 Native IPC 共用 `MemJsonTools`，地址均要求显式 `0x` |
| A-15 | P2 | 已修复 | 设置弹窗使用统一可丢弃草稿，并支持事务式 Forget provider key |
| A-16 | P3 | 已修复 | 删除瞬时 `approvalDecision`；trace/tool message/audit 是审批事实来源 |
| A-17 | P2 | 已修复 | Python MCP/legacy HTTP 已删除；Agent 与 Native IPC 共用 24-name catalog 和 adapter |
| A-18 | P1 | 已修复 | 三类 provider 统一验证 start/terminal/malformed；截断 partial 只作为错误展示 |
| A-19 | P0 | 已修复 | partial I/O/timeout/EOF 会 poison+close；旧 endpoint 字节不能跨 reconnect generation |
| A-20 | P0 | 已修复 | 三端口 manager 与 request/lifecycle lease 已统一，并有真实 loopback 和故障压力测试 |
| A-21 | P2 | 已修复 | dispatch 预算统一 provider/model 上限、工具 schema、输出预留与保守 UTF-8 估算 |
| A-22 | P2 | 部分修复 | 已覆盖 MemService、target、连接、工具 worker、provider stream 状态机和 Native IPC；真实环境仍缺回归测试 |
| A-23 | P2 | 已修复 | 同一 ELF 的多个映射分段按完整路径折叠，并以最低 mapping base 作为 canonical module base |

## 当前未解决问题

### A-02：Agent 目标绑定已覆盖当前所有实际 send 边界（已修复）

**证据**

- `AgentRunContext` 在 `resetForNewRun()` 时保存 connection generation 和 target snapshot，并绑定当前 cancellation token。
- `ToolRegistration` 已声明 `None`、`Bound` 或 `Selection`；`AgentController` 在等待审批前、批准出队前和成功结果接收前复核快照。
- 审批框显示预期 connection generation、PID 和 process revision。
- 23 个非 Lua canonical 工具的 adapter 均消费显式 `OperationContext` 并在 `MemService` 边界复核；`process_open` 在 send 前再次比较旧 selection，并只在返回快照与当前状态一致时推进 run target。
- `lua_execute` 在 host 执行前复核 target/generation，并把审批 `OperationContext` 绑定到完整 Lua 调用；脚本内部 API 沿用原 target/generation/deadline/cancellation。`ToolDefinitions.cpp` 已删除全部 33 个 hidden alias 和 direct-socket executor，并由 `native_agent_catalog` CTest 阻止重新依赖 `client_singleton.h`/`AppContext.h`。
- GUI `BreakpointWindow` 的 set/remove/enable/suspend/resume 与 hit refresh 已显式注入 `IMemService`，每次调用捕获 target context；命中 batch 保留完整 GPR/FPSIMD，按最新 50,000 条有界。
- GUI `ScanWindow` 的 start/refine/results/clear/remove 已显式注入 `IMemService`，每次调用捕获 target context 并携带最新 scan epoch；Stop 通过共享 cancellation token 进入同一 service/backend 调用。
- `Gui.cpp` 向连接、进程、模块、版本、扫描、Memory Viewer、断点和 Lua 窗口显式注入同一 `IMemService`；GUI/Lua/main 静态门禁拒绝 direct protocol 和非 `AppContext` 目标写入。
- 无设备测试覆盖审批期间切进程/重连、批准后 send 前切进程、同批 `process_open -> memory_read` 和晚到成功结果拒绝。

**影响**

当前注册表只包含 canonical 工具。所有 process-bound 工具在 Controller 校验后，还会在实际 service/host 边界再次验证 snapshot；scan/symbol session 绑定 target+epoch，breakpoint/driver 保留确认/未知回执。退役名称不再可执行，旧会话中的调用组只会在 provider 边界降级为不可执行的 assistant 历史文本。因此原先“legacy executor 在校验后读取新共享状态并向错误目标发送”的路径已关闭。

**建议**

- 保持 `native_agent_catalog` 的名称和依赖检查，不重新引入第二套模型可见名称或 direct-socket executor。
- 将 process name 加入审批显示，并把 effect、资源域和规范化参数写入持久审计。
- 所有 target selection/mutation 的 fake backend 测试必须覆盖“校验后、send 前切换”以及 completion unknown。

### A-04：配置/索引损坏可导致启动异常或覆盖原文件（已修复）

**关闭证据**

- `ApiKeyStore`、`AiSettings`、`SessionManager`、`ChatSession` 统一返回 `Loaded`、`Missing`、`Recovered`、`Invalid`、`IoError`；JSON 解析、字段类型和范围检查全部在临时状态完成，只有成功才一次性提交。
- 启动路径不再放任字段类型异常逃出。只有真正 `Missing` 的配置/设置文件会创建默认值；`Invalid`/`IoError` 保留原文件和原内存状态。
- 损坏会话索引先保留为不覆盖已有备份的 `.corrupt*`，再扫描合法会话 JSON 重建 metadata。原索引无法保留时自动写回被禁止，避免以恢复名义覆盖证据。
- `ChatSession` 写入统一使用 `utils::installTempFile()`；`saveBound()` 只写成功加载或明确新建的绑定。损坏活动会话保持原路径不动，UI 创建新的可写会话。
- `native_persistence_recovery` 当前 13 组覆盖损坏索引恢复、API key/settings 的事务式加载、会话加载、原子安装绑定与 fallback 安装故障恢复；完整 Debug 套件为 30/30 CTest。

**剩余边界**

A-04 只关闭“损坏加载覆盖原件/异常逃逸”问题。持久化文件、单消息和会话载入的总量上限由 A-09 关闭；全局 prompt/token 所有权和 v1 session 迁移由 A-12 关闭。

### A-05：HTTP worker 的完成回调未纳入排空计数（已修复）

**关闭证据**

- `HttpClient` 为每个请求保存 cancellation token、best-effort `Client::stop()` hook 和 joinable thread；旧的 detached、`inFlight_`、3 秒 bounded wait 与 callback 前移除逻辑均已删除。
- scope guard 只在 `completeSafely()` 及 provider callback 返回后标记请求完成。正常完成记录由下次 dispatch 回收；进程 shutdown 会禁止新请求、设置全部 token、尝试中断 transport，并 join 所有 active/complete worker。
- `shutdown()` 返回是否真正排空；从自身 worker callback 调用时返回 `false` 防止 self-join，`main.cpp` 检查并记录该异常路径。
- `native_http_client_lifecycle` 的 4 组本机 HTTP 测试覆盖 streaming cancellation、self-join rejection、callback-inclusive shutdown 和 shutdown 后拒绝新请求，连续 100/100 通过；完整套件增至 21/21。

**剩余边界**

`Client::stop()` 只是 transport interruption hint，静默 read 不保证立即返回；shutdown 会继续等待配置的 I/O timeout 并最终 join，而不是提前遗留线程。HTTP/SSE 累计输入上限现由 A-09 关闭；本机自签名 HTTPS 已覆盖真实 transport、证书校验和 full-response，并已完成一个外部 OpenAI-compatible endpoint 的互操作验收。其他 endpoint/model、限流和认证错误仍不是自动化兼容承诺。

### A-07：Stop 后已发送操作的最终状态独立可见（已修复）

**证据**

`ChatWindow::cancelRequest()` 同时设置 run token 并调用 `AgentTaskExecutor::cancelRun()`。排队任务成为 `cancelled_before_start`，迁移到 `MemService` 的 active 操作观察 cancellation；canonical 长扫描请求 DEBUG stop，memory/breakpoint/driver/Lua mutation 保留 confirmed-after-cancel/deadline 或 `completion_unknown`。活动 mutation 的 Stop notice 明确显示 cancel requested。Clear、New、session switch/delete 和窗口析构也统一调用 active-run cancellation。

`AgentTaskExecutor::deliver()` 在任何 completion callback 前将 write-classified 和 symbol-session effect 写入 `AgentMutationAuditLog`。因此即使 callback 抛异常、原 run 已清空或 `UIMessageQueue` 被 run-id gate 丢弃，最终状态仍保存在 `ai_mutation_audit.jsonl` 并出现在 Audit 表。manual deny 与 queue rejection 也有记录。

**影响**

已发送副作用仍不能撤回，迟到结果也不会重新写入已结束的聊天 session；这是刻意的隔离。最终 completion、approval、effect/resource domain、run/tool-call id 和预期 target 改由独立审计承担。当前 service/host executor 都消费 context，但阻塞中的设备或 Lua 调用仍可能只在检查点、socket deadline 或返回时响应取消；worker ownership 和最终 outcome 不会丢失。

**建议**

剩余约束：只有协议能安全中断且不会留下半包时，才把 cancellation 进一步传入 socket 层。独立日志是 4 MiB active + 一个轮转备份的明文摘要，不是不可篡改或远程合规审计。

### A-08：复合操作和共享设备状态不是事务（已修复）

**证据**

- 端口锁仍只覆盖单个 request-response，但每个产品复合操作都有更高层 owner。
- pointer 持有 read transaction 到最后一次解引用；scan/symbol 使用 domain mutex、monotonic epoch 和 MAIN transaction；breakpoint mutation/cleanup 把设备确认与本地 tracker 放在同一 MAIN transaction。
- `AppContext::selectProcess()` 与 `SetCurrentPid()` 已删除。`SystemMemBackend::openProcess()` 取得 connection request lease 后创建 `TargetMutation`，依次执行 DEBUG cleanup 和 MAIN cleanup/close/open；只有非零 handle 且 generation 仍当前才发布。
- `TargetMutation` 用奇数 revision 隔离进行中的切换，用偶数 revision 发布稳定 PID/handle/name，并同步失效 presentation cache。只有 `AppContext` 能写这些字段。
- GUI、Lua、内置 Agent 和显式启用的 Native IPC 全部经 `IMemService` 进入这些 transaction；静态 gate 禁止重新引入 direct protocol 步骤。

**影响**

旧 target 的在途端口命令会先完成；mutation 开始后，新 context capture 等待稳定发布，已捕获的旧 context 在 send gate 因 revision/generation 不匹配而失败。当前产品前端不能在 cleanup/open 或 scan/symbol/pointer/breakpoint 复合序列中插入裸协议命令。

**建议**

- 保持 connection -> process -> domain -> port 锁顺序；持 port gate 时不得反向获取 process-state mutex。
- 新复合能力必须进入 `IMemService` owner 并增加 transaction/revision/epoch，或下沉为服务端单命令。
- 保持 `native_gui_mem_service_boundary` 对 target publication 和 process-switch markers 的回归检查。

### A-09：响应、工具输出和会话载入缺少总量上限（已修复）

**关闭证据**

- `AiLimits.h` 统一硬边界：HTTP 16 MiB，SSE line/event 1/2 MiB，assistant content 4 MiB，普通消息 8 MiB，64 个 tool calls，id/name 256/64 bytes，arguments 512 KiB/调用与 4 MiB/消息，tool result JSON 4 MiB。
- `HttpClient` 在 append 前拒绝超限 chunk，`SSEParser::fail()` 清理 line/event buffer；三个 provider 把网络、流式和非流式超限统一归类为 `InvalidResponse`。非流式解析在复制超长字段或物化第 65 个 tool call 前返回。
- `ToolExecutor` 丢弃并释放超限结果。mutation 已可能发出，因此返回 `completion_unknown`；`AgentRunner` 把 tool audit 收缩到 8 MiB。assistant/tool outcome 无法进入会话时，`ChatWindow` fail closed，不执行未记录工具，也不发送缺失结果的 follow-up。
- 所有持久化 JSON 在 parse 前受 32 MiB 文件限制；`ChatSession` 写盘也检查最终转义后的 32 MiB。磁盘消息最多 10,000 条，只 reserve/物化最后 1,000 条；单消息/tool 字段逐项校验，retained payload 上限 16 MiB。
- runtime `addMessage()` 预检受保护的最新用户回合，按完整对话组裁剪；拒绝时不先删除旧历史。所有不可信 JSON 在 DOM 前还限制深度、节点、单容器元素、单字符串与累计字符串，并对分配失败 fail closed。`native_persistence_recovery` 的 13 组、`native_provider_stream_terminal` 的 19 组、`native_provider_http_integration` 的 6 组、`native_http_client_lifecycle` 的 5 组和 `native_agent_mem_service` 的 31 组均通过；完整 CTest 当前为 30/30。

**剩余边界**

字节与结构上限仍不等于 allocator 精确峰值：nlohmann JSON DOM、TLS/cpp-httplib 和容器对象有额外开销。本机真实 cpp-httplib/OpenSSL transport、证书拒绝、三家 full-response 与结构过密响应已自动化；一个外部 OpenAI-compatible endpoint 已完成人工互操作。大型业务列表仍应在 service 层分页。A-21 已补 request context 预算，但它不替代这些 allocator/transport 硬边界。

### A-10：provider endpoint 身份与信任边界（已修复）

**关闭证据**

- `OpenAIProvider::getDefaultBaseUrl()` 改为官方 `https://api.openai.com/v1`；源码和静态 gate 禁止旧第三方默认值回归。
- 自定义 OpenAI-compatible endpoint 必须显式勾选 `Trust this endpoint`。信任绑定到 `canonicalEndpointForTrust()` 生成的完整 URL：scheme/authority 归一化且去除末尾斜杠，path 大小写和内容保持身份语义；host/path 变化立即失效。
- `trustedBaseUrl` 进入 `ai_config.json` v3。旧第三方配置可以继续显示和编辑，但没有匹配 trust 时不会发送，也不会被启动流程静默改成官方地址。
- `ChatWindow` 在创建 run 前校验，`AgentController` 在唯一 dispatch 边界再次校验；任一检查失败都发生在 API key、消息和工具结果进入 HTTP 请求之前。
- `native_provider_trust` 的 3 组覆盖 endpoint canonicalization、默认 endpoint implicit trust 和自定义 endpoint 精确绑定；`native_provider_trust_gate` 固定官方默认、双发送边界与 UI/persistence integration。完整 Debug 与 fresh `ENABLE_NATIVE_IPC=ON` mandatory-feature Release 均为 28/28。

**剩余边界**

显式 trust 只记录用户对该 URL 的决定，不验证自定义服务的组织身份、数据保留政策或模型能力；HTTPS 也只保护传输。新增内置 provider 或改变默认 endpoint 时仍必须重新进行产品级数据去向审查，不能复用现有自定义信任。

### A-11：会话明文保存敏感 Agent 数据（已修复）

**关闭证据**

- `ProtectedPersistence` 定义固定 `AMEMAIP1` 二进制信封，逐字段验证版本、purpose、reserved bytes 与 ciphertext length；session 和 index 使用不同 optional entropy，DPAPI 解密失败或密文篡改都返回 `Invalid`。
- `ChatSession` 和 `SessionManager` 不再直接读写会话 JSON。会话历史、tool calls/results、标题、active id 全部先序列化为最多 32 MiB JSON，再以当前 Windows 用户 DPAPI 保护，外层文件限制 33 MiB，临时文件成功后才原子安装。
- 启动时会验证并迁移未打开的有效旧会话；active session、index 和 `ai_session.json` 同样在完整业务 schema 校验后才替换为信封。迁移/写盘失败不提交内存或 binding；无效、损坏或超限旧文件保持原字节且永不进入 provider history。
- driver card 仍在进入会话前字段级脱敏，避免可复用 credential 出现在解密后的历史。Lua、地址、内存、符号、寄存器和普通工具结果可保持模型连续性，但磁盘文件不再可直接读取。
- `native_persistence_recovery` 当前 13 组，除同用户 round-trip、磁盘无 plaintext/schema、有效旧明文迁移、无效原件保留、index/title 保护、purpose mismatch 和 ciphertext tamper 外，还覆盖原子安装故障恢复和持久化 JSON 结构边界；`native_session_protection_gate` 禁止 session/index 绕回普通 JSON loader。当前 Debug 为 30/30。

**剩余边界**

DPAPI 绑定 Windows 用户，不防御已能以同一用户运行并调用 DPAPI 的恶意进程，也不保护运行时内存、剪贴板、pagefile 或 crash dump。`ai_settings.json`、mutation audit 和 Native IPC approval audit 仍是明文；两类 audit 通过字段省略降低敏感度。发送请求时会话必须解密，system prompt、消息、工具参数和结果仍会交给用户配置的 provider，因此 endpoint trust 与远端数据政策仍是独立边界。无效旧文件为满足事务式损坏恢复保持原字节，但应用不会绑定、展示或发送它们。

### A-12：全局 prompt/token 设置被会话文件覆盖（已修复）

**关闭证据**

- `AiSettings` 明确成为 system prompt/token limit 的唯一持久化所有者；`ChatSession` 只持有请求构造和裁剪所需的实时副本。
- session format v2 只保存 `version` 和消息历史，不再序列化 `systemPrompt`/`tokenLimit`。loader 仅接受 v1/v2，拒绝未来未知版本。
- v1 文件中的旧 prompt/token 字段无论值类型是否有效都被忽略，不会覆盖全局设置，也不会阻止历史消息迁移。
- startup、session switch 和 new session 都保留当前全局值；成功 load 后立即用 live global token limit 按完整消息组裁剪。
- `native_persistence_recovery` 的 global-settings ownership 组覆盖 v1 忽略、v2 输出/重载和 load-time 全局预算裁剪；A-15 后该 executable 当前共 10 组，完整 CTest 28/28。

**剩余边界**

所有权冲突已经关闭。A-21 后 `tokenLimit` 仍是全局用户/会话保留上限，但 dispatch 会另取 provider/model context、显式 endpoint override、工具 schema、协议 framing 与输出预留形成更小的实际请求预算；两层不能再次合并为 session 持久化字段。

### A-13：`symbol_*` 分类、共享状态和默认 prompt 漂移（已按当前二元模型解决）

当前二元 `ToolSafety` 明确定义为：`Write` 表示需要审批的目标/主机 mutation。规范 `symbol_resolve`/`symbol_list` 不修改目标内存，保持 ReadOnly；active table 的内部 session mutation 由 MAIN transaction 和 symbol epoch 约束。`symbol_init`、`symbol_find`、`resolve_symbol` 已从注册表删除，默认 prompt 只列规范名称且不再把 symbol list 写成需要写审批。

剩余改进是引入设计文档中的 richer effect/resource metadata，把 symbol 操作标为 `SessionMutation` 并记录资源域审计。该扩展不能重新引入模型可见的 init 前置步骤，也不能把“可安全自动重试”与 ReadOnly 自动等同。

### A-15：设置草稿和 provider 删除语义不完整（已修复）

**关闭证据**

- `ChatSettingsDraft` 统一持有 provider、prompt、数值和 proxy 编辑状态。每次打开弹窗从 `ApiKeyStore`/`AiSettings` 新快照初始化；Save 之外不会修改 live provider、`HttpClient` 或持久化设置，Cancel、标题栏关闭和成功 Save 都丢弃完整草稿。
- 已配置 provider 显示明确的 `Forget saved key`。删除先在草稿中暂存并可 Undo；Save 通过 `ApiKeyStore::applyChangesAndSave()` 在加密临时 snapshot 中合并全部 update/remove，原子安装成功后才替换内存 map。写盘失败保留原文件、原 live provider 和草稿供重试。
- Forget 成功后立即以空配置重新配置 live provider；`ProviderConfig` 覆盖、移动和析构都会主动擦除旧 `apiKey`。`ProviderSettingsDraft`/`ChatSettingsDraft` 在 Cancel/X/Save/析构时以 volatile write 清零 API key、prompt 和相关字符 buffer。
- `native_persistence_recovery` 新增 provider update+remove 原子提交、不可写目标失败回滚和 settings draft secure clear 两组，当前 10 组全部通过；相关 ChatWindow/provider 产品翻译单元编译通过，完整 CTest 为 28/28。

**剩余边界**

显式清零只覆盖本进程拥有且可定位的 C++ buffer，不证明操作系统、ImGui 内部编辑状态、剪贴板、allocator 或崩溃转储没有副本。运行中的 HTTP 请求已经复制到 header 的 key 也不能由 Forget 撤回；Forget 只保证后续请求和持久化不再使用该配置。

### A-16：`approvalDecision` 在运行快照中几乎不可观测（已修复）

`AgentApprovalDecision`、`AgentRun::approvalDecision` 和 snapshot 同名字段已删除，Controller 不再写入后立即重置一个误导性状态。`pendingApproval` 只表达当前是否等待决策；批准、拒绝和自动批准事实由 `AgentTraceType::Approved`/`Denied`/`AutoApproved`、生成的 tool message 以及 mutation audit 表达，生命周期明确且 UI 已消费。源码 gate 搜索确认运行代码不再含该字段，31 组 Agent/MemService 测试中的审批矩阵覆盖 read-only、manual approve/deny、auto-approve、auto-approved failure、后续 skipped、tool pairing 与 trace/batch 顺序。

快照不是永久审计存储；trace 仍按 128 条有界。需要跨运行长期追踪 write decision 时读取独立 mutation audit，而不是重新给 run snapshot 添加无期限“最近一次”字段。

### A-18：截断 SSE 被当作成功响应（已修复）

**证据**

- Claude 在 `message_stop` 时设置 `StreamState::completed`，完成回调从不检查该字段。
- DeepSeek 在 `[DONE]`/`finish_reason` 时设置 `finished`，完成回调也不检查。
- OpenAI streaming parser 不记录 `finish_reason`；公共 `SSEParser` 还会过滤 `[DONE]`，provider 无法用它确认结束。
- 三者在 HTTP 2xx 时都会把已累计文本/tool fragments 作为成功 `Completion`。

**影响**

服务端、代理或中间网络若正常结束 HTTP/chunked body，却没有发 provider 终止事件，部分文本会被持久化为完整回答；更危险的是半截 tool arguments 可能进入校验/执行路径。即使参数最终因 JSON 无效被拒绝，错误也会被误归为模型工具参数问题，而不是截断流。

**关闭证据**

- `SSEParser` 已从 `HttpClient.cpp` 提取为纯组件，不再过滤 `[DONE]`，并覆盖分片、多行 `data:`、EOF flush 与 callback exception。
- `StreamTerminalTracker` 统一记录合法 start、terminal 和首个 malformed/schema error。Claude 要求 `message_start`/`message_stop`；OpenAI/DeepSeek 要求合法 `choices` 起始，并接受非空 `finish_reason` 或 `[DONE]`。
- 三个 provider 在 HTTP 2xx 完成时强制验证 tracker；失败响应保留 partial message/tool calls，但 `CompletionResponse.error` 使 `ChatWindow` 在工具校验与执行前退出。
- `native_provider_stream_terminal` 当前 19 组覆盖完整、截断、重复终止、空 `finish_reason`、malformed、terminal-without-start、非 SSE 2xx、partial tool-call retention 和 provider JSON 结构边界；`native_provider_http_integration` 另以 6 组本机 HTTPS 测试覆盖三家 full-response、HTTP error、结构过密响应和证书信任拒绝。当前 Debug 完整 30/30 CTest 通过。

### A-21：provider context 能力没有参与请求预算（已修复）

**关闭证据**

- `AgentController` 在唯一 provider dispatch 边界读取 `getCapabilities()`，以全局用户 `tokenLimit` 和当前 provider/model context 的较小值作为总窗口。内置 endpoint 的显式 context 只能收紧 provider 声明；自定义 endpoint 可以用持久化的 `contextWindowTokens` 替换 provider 默认，但仍受 1,000,000 用户上限约束。
- `ContextBudget` 对 ASCII 使用保守 3 bytes/token，对 2/3-byte UTF-8 code point 至少计 1 token、4-byte code point 计 2 token；消息/工具调用 framing、provider envelope 和全部广告工具 name/description/schema 都计入 input。
- output reserve 默认来自 provider capability 的 4096 tokens，小窗口按最多四分之一收缩但不少于 256；预算结果写入 `CompletionRequest.maxOutputTokens`。Claude/DeepSeek 发 `max_tokens`，OpenAI-compatible 发 `max_completion_tokens`，因此预留和请求上限一致。
- 预算只裁剪 `getMessagesForRequest()` 的清洗副本，按完整旧 user/tool conversation group 删除，不破坏磁盘历史。最新 conversation 连同 system/tools 仍超限时本地 fail closed，不发送已知超限请求；trace 记录 input/budget/output reserve 与 dropped message 数。
- `native_context_budget` 的 5 组覆盖 UTF-8 估算、provider/用户/自定义 endpoint 上限、工具 schema 与输出预留、完整 tool group 裁剪和最新组拒绝。`native_context_budget_gate` 固定预算先于 send、三 provider 输出参数和 UI/persistence override；完整 CTest 为 28/28。

**剩余边界**

本地估算不是对应模型的 tokenizer，provider 可能计算不同的 Unicode、JSON 或 tool grammar 开销；显式自定义 context 也依赖用户填写真实能力。真实 provider 400/usage 回执尚未自动校准本地估算，后续可以在不放宽 fail-closed 预算的前提下加入 model-specific tokenizer/profile。

### A-22：核心路径缺少自动回归测试

仓库已有 `native_agent_mem_service` 的 31 个测试组和独立 `native_app_context_state`。覆盖连接 generation、批量读取完整性/失败、冻结 completion receipt、奇偶 revision、stale mutation、cache invalidation、disconnect publication、ToolExecutor schema 矩阵、AgentRunner 审批矩阵、tool history pairing 和 JSON 结构限制。Native IPC 有 6 组 security-audit、12 组 approval-broker、5 protocol、5 transport、8 framed-I/O、8 handshake、7 request-contract、9 request-session、4 catalog、15 dispatcher 和 10 runtime 测试。socket client 有 4 组，multi-port manager 有 6 组；provider/SSE 有 19 组；本机 HTTPS/full-response 有 6 组；context budget 有 5 组；provider trust 有 3 组；persistence recovery/limits/session protection/global-settings/设置草稿/原子安装/JSON 结构有 13 组；HTTP lifecycle/response limit 有 5 组。九个静态 gate 包含完整 GUI/Lua/main service boundary、context dispatch、endpoint trust 和 session protection integration，当前共 30 项 CTest；fresh mandatory-feature + Native IPC Release 的产品链接证据记录在重构计划。

**2026-07-13 真实验收证据**

- Android 后端使用 `android_mem_engine/bin/socket_server`，本地与设备部署文件 SHA-256 都是 `FC7AEEC89102AF0370F26E72D088A1E6A1D34CC6EDA3C630E98623DA76F5377D`；GUI 三端口连接到 ADB 转发 `52738`，握手为 `CHEATENGINE v2.0`。
- 自定义 OpenAI-compatible endpoint 以精确 URL trust 和 DPAPI key 配置运行，磁盘 key 字段不是 `sk-` 明文。首次请求发现 `HttpClient` 重复发送 `Content-Type`，服务返回 HTTP 400；修复为只发送一个 `application/json`，并在 `native_provider_http_integration` 增加 header-count 断言。随后 `gpt-5.4` 固定文本、合法 SSE terminal、tool call、结果回喂均通过。
- 内置 Agent 完成 `status`、进程查询/切换、模块分页、raw/typed memory read、人工批准写入/读回/恢复、scan start/results/refine/clear、Lua deny/approve 和自动批准路径。fixture 最终快照保持 marker `0x1122334455667788`、scan `0x13572468` 及两侧 guard 原值。
- 当前 Android scan backend 只纳入完整落在 `[start,end]` 内的 mapping。fixture 子区间扫描返回 0；覆盖完整 `rw-p 0x5B87432000..0x5B87434000` 后，epoch `7` 精确返回 `0x5B87432FC0`，refine 后 epoch `8` 仍为 1，clear 后 epoch 为 `9`。调用方不能把 mapping 内部窄范围的 0 结果解释为值不存在。
- 普通 `SysCall` 后端在已验证的 `rf_native_add` ARM64 地址上拒绝 execute breakpoint：`protocol_error`、`retryable=false`、`completion=completed`。随后 `status` 仍为 connected、未 poisoned，PID/handle/revision 未改变。
- `SysCall`/`Kernel` 是 Android 后端内部的读写与断点实现选择，不是 target identity，也不要求推进 connection generation 或 process revision。
- Kernel 验收时，`module_list(filter="librfhooktarget.so")` 返回同一路径的两个映射分段：`0x6F4D607000`/flag `13` 与 `0x6F4D60B000`/flag `9`；旧解析器把它们误判为 ambiguous。当前 A-23 修复按规范化完整路径折叠同一 ELF 的分段，并选择最低 base；不同完整路径的同 basename 仍拒绝为真实歧义。
- `Kernel` execute breakpoint 完整成功链已通过：set 返回 `confirmed=true/completion=completed`；hits 返回最新 10 条且 `truncated=true`，每条 `pc=0x6F4D607710`、`x1=0x7`，`x0` 随 fixture tick 增长；suspend 后最新 `hit_time` 固定为 `1783959312`，resume 后推进到 `1783959590`；remove 返回 confirmed，十余秒后的 hits 为 `count=0/hits=[]`。最终 `status` 仍为 connected、未 poisoned、Kernel、PID `31372`、revision `10`、generation `1`。`process_open`、set/suspend/resume/remove 均以 `auto_approved`、`success=true`、`completion=completed` 写入独立 mutation audit。

- Native IPC GUI approval click 与真实设备 privileged operation。
- 不同 Windows 用户/session 与真实 remote client 的 native transport 负向测试。
- 真实 Android 三端口 timeout/reconnect 和远端状态恢复。
- 真实 GUI 连接、进程切换、批量刷新、冻结和 Lua target-switch 交互 smoke。
- 更多 provider/endpoint/model 的互操作、限流与认证错误；当前仅验证一个显式信任的 OpenAI-compatible endpoint。

建议先建立不依赖 Android 设备的单元/契约测试，再保留少量真实设备 smoke test。否则当前文档中的安全不变量无法在后续重构中自动守住。

### A-23：精确模块名因同一 ELF 的多个映射分段而歧义（已修复）

**关闭证据**

- `module_list` 继续保留服务端返回的原始 mapping，避免改变 GUI 和协议展示语义。
- `findResolvedModule()` 在完整名、basename 或唯一子串的当前优先级内，以大小写归一化后的完整路径作为模块身份。同一路径的多个 mapping 只形成一个候选，并选择最低地址 mapping 作为 canonical module base。
- 两个不同完整路径即使 basename 相同仍返回 `module_name is ambiguous`，不会把真正的同名库静默合并或取第一项。
- 该 helper 同时服务 `module_resolve`、`pointer_resolve` 和 symbol module lookup，因此三条路径使用相同语义。
- `native_agent_mem_service` 新增乱序同路径分段、exact full path、不同路径同 basename 和 `MemJsonTools` adapter 回归；针对性测试通过。

**剩余边界**

当前 Android wire entry 没有 file offset、inode 或 load-instance id。相同规范路径如果被真正加载为多个独立实例，客户端无法仅凭现有字段区分；需要该能力时应先扩展服务端协议，而不是在客户端引入地址距离猜测。

## 已修复摘要

以下旧问题已在当前代码中落地，不再把旧的“现状/影响”保留为待修复描述：

| 项目 | 当前实现 |
|------|----------|
| A-04 配置/索引损坏覆盖原件 | 四类 loader 临时解析后提交并区分五种状态；只有缺失才写默认值，损坏索引保留后扫描重建，失败会话不绑定写回，统一使用原子安装助手 |
| A-05 HTTP callback 脱离排空计数 | 每请求 owned/joinable worker；completion callback 返回后才完成，shutdown 取消/stop 并 join 全部线程，self-join 显式失败 |
| A-09 响应/输出/会话无总量上限 | HTTP/SSE/provider/tool/session/persistence 分层硬上限；超限 mutation 保留 completion unknown，会话裁剪按完整组且失败事务式，四个相关 executable 连续 20/20 |
| A-11 会话明文保存敏感数据 | session/index 使用 purpose-bound 当前用户 DPAPI 信封；有效 legacy 明文在完整 schema 校验后原子迁移，无效输入保持原字节且不绑定 |
| A-12 全局 prompt/token 被 session 覆盖 | `AiSettings` 是唯一持久化所有者；session v2 只保存历史，v1 同名字段忽略，load 后按当前全局预算裁剪 |
| Python MCP 复制工具 schema、常量和 retry 语义 | FastMCP package、安装入口、IDE 配置和 `.mcp.json` 已删除；仅保留不参与产品运行的标准库协议排障脚本 |
| A-01 legacy HTTP 无鉴权/CORS 控制面 | `ipc/IpcServer.*`、端口启动、CMake 选项和 compile macro 已删除；静态 gate 阻止恢复旧 HTTP server |
| A-06 legacy detached handler/partial send | legacy handler 已随 HTTP server 删除；Native IPC 使用 owned/joinable handler、overlapped exact I/O、Stop event 与 `CancelIoEx` |
| A-14 地址进制跨前端不一致 | Agent 与 Native IPC 共用 `MemJsonTools`，所有地址字段拒绝无前缀字符串并要求显式 `0x` |
| A-17 Agent/外部控制契约分裂 | Python MCP 和 legacy HTTP 已删除；Native IPC 与 Agent 对齐 24-name catalog、共享 adapter、target policy 与 feature gate |
| A-18 截断 SSE 被当作成功 | 公共 parser 暴露 `[DONE]`，三个 provider 统一验证 start/terminal/malformed；partial error 不进入工具执行，14 组纯测试覆盖完整与失败语义 |
| A-19 timeout 后复用旧流导致串包 | `IWindowsSocketOps` 注入测试固定 partial send/receive、`WSAETIMEDOUT`/EOF poison+close、旧 lease 失效；迟到旧字节保留在旧 endpoint，新 generation 只读取新 endpoint |
| A-20 connect/disconnect 与在途请求缺少互斥 | `MultiPortClientManager` 在一个 exclusive lifecycle lease 内管理三端口、状态清理和失败回滚；真实三连接 loopback、活动 request 阻塞 disconnect、单端口 poison、全端口重连与 50 轮 generation 已覆盖 |
| Native IPC wire contract 未固定 | `IpcProtocol` 使用显式 24-byte little-endian header、精确版本与 request-id 规则、UTF-8 和 payload 硬上限；跨 polling timeout 的 partial frame 会有界保留并继续读取，partial close 仍是 protocol error；终止 Error 后做 100 ms 可取消 drain |
| Native IPC 身份与 handler 生命周期没有基础边界 | protected DACL 只允许当前进程用户和 SYSTEM read/write；拒绝 remote client，单实例 handle 持续占有名称；accept/handler 同属一个 joinable thread，stop event + `CancelIoEx` 后 join；产品仍不启动 |
| Native IPC 协议阶段缺少统一 runtime owner | `NativeAgentRuntime` 持有 server，串起 framed connection/Hello/Observe dispatcher/request session；Stop 取消并 join handler/worker，线程安全 snapshot 不保存请求参数或结果；server session id 跨 restart 单调且退出时精确取消绑定审批；产品默认 stopped，仅允许用户显式启用 Observe |
| Native IPC privileged approval 没有可验证状态机 | broker management、session/request cancel、worker submission、bounded persistent security audit、fail-closed consume、dispatcher/send-boundary 与最终 outcome 已接产品链；consume 写盘失败烧毁授权且不返回 grant，post-effect outcome 写盘失败保持真实回执并进入可见 health |
| Agent breakpoint 直连 socket 且回执/本地 tracker 分离 | 五个规范工具经 `MemService` 绑定 target；mutation 区分未发送/拒绝/完成未知/确认完成，设备确认与 cleanup tracker 在同一 transaction 更新，hits 使用有界最新批次且不伪造 cursor |
| Agent symbol 依赖 active table 前置状态 | `symbol_resolve`/`symbol_list` 在一个事务内完成 module resolve + init + find/page；续页绑定 epoch，旧前端 init 会使其失效 |
| Agent scan 依赖 set-range 前置状态且跨前端不可检测 | `scan_start` 一次提交完整请求；refine/results/clear 绑定 epoch，所有旧 scan mutation 也推进 epoch；sent-without-terminal 返回 `completion_unknown` |
| pointer chain 的 module/read 可被其他前端插入 | `pointer_resolve` 持有 read transaction 到最后一次 read；GUI/Lua/Agent/Native IPC 均从 service 进入，低层 helper 不再是业务入口 |
| module 列表/解析直连 socket 且子串取首项 | `MemService::listModules/resolveModule` 统一分页、target 校验和完整名/basename/唯一子串匹配；同完整路径 mapping 以最低 base 折叠，不同路径歧义与异常范围仍失败 |
| typed read/write 重复解析和直连 socket | `ValueCodec` + `MemService::readValue/writeValue` 统一 scalar 范围/字节序；规范工具要求 `0x`，旧名称已退役且不可执行 |
| 工具路径两层 detached worker | `AgentTaskExecutor` 独占 joinable worker；`ToolExecutor` 同步执行，shutdown 测试证明 active/queued task 排空后 join |
| executor 返回顶层 `{"error": ...}` 却标成功 | `ToolExecutor::extractToolError()` 会转为 `success=false`，`AgentRunner` 保留 details |
| Claude 结构化错误丢失 | `ChatWindow::pollMessages()` 能统一处理 `CompletionResponse.error` |
| 多个 socket 可变长度响应缺少边界 | 进程、模块、符号、冻结、断点和扫描结果的主要读取点已增加 count/name 上限 |
| `FetchProcessList()` 半截读取仍返回成功 | 条目或名称读取失败会返回失败 |
| `InitDriver()` 长度未校验 | card 和返回消息已有非空/长度检查 |
| `OpenProcessHandle()` 接受 handle 0 | 底层与 `SystemMemBackend::openProcess()` 都拒绝 handle 0，失败 mutation 发布 detached 稳定状态 |

## 不应误认为“已闭环”的缓解

- `runId` 过滤只防止迟到消息污染当前 UI，不会停止网络或工具副作用。
- `SocketIoTimeout` 现在消费 task absolute deadline，但不提供事务回滚或撤回已经发送的写命令。
- `AgentTaskExecutor` 与 `HttpClient` shutdown 都会 join；HTTP transport stop 不保证静默 read 立即结束，因此退出可能等待配置的 I/O timeout。
- DPAPI 保护 provider API key 与可用 session/index 的静态文件，但不保护同用户恶意进程、运行时内存、明文审计或远端 provider 数据。
- Native DACL/remote rejection 提供身份边界，Hello 只授予 Observe，privileged operation 通过逐请求审批与 durable one-shot grant 授权；尚未完成跨用户/session 与真实 remote client 负向验证。
- execution outcome audit 是同步 best-effort 的事后记录：它覆盖正常返回路径，但进程在设备 effect 与日志 flush 之间崩溃时仍可能缺失 outcome；不能把它描述为设备事务日志。
- 四类持久化 loader 已事务化且 `ChatSession` 已统一使用 `installTempFile()`；A-09 的文件/消息/session 上限与 A-12 的全局设置所有权已落地，但 JSON DOM allocator 开销仍不等于源字节。
- `DeviceSession` 已删除待处理字节清理恢复路径；client 与 multi-port manager 测试证明 partial I/O、timeout/EOF、迟到字节隔离和 lifecycle 互斥，但 Android 端 driver/process/scan/breakpoint 状态恢复仍没有自动承诺。
- provider-aware 预算是保守估算而非真实 tokenizer；自定义 endpoint 的显式 context 配错时仍可能得到 provider 400，但不会绕过用户上限、工具 schema 或输出预留。

## 建议修复顺序

1. 保持 Native IPC compile/runtime default-off；补 GUI click 和真实设备验证。不得把逐请求 grant 扩大成 Hello 级长期 privileged capability。
2. 为已落地的 poison/lifecycle gate 增加真实 Android 设备压力与恢复记录。
3. 为 Stop、写工具晚到结果和复合设备操作继续保持明确状态/事务边界。
4. 继续为大型列表增加 service 分页；provider-aware context 预算已由 A-21 固定。
5. endpoint 身份与显式信任由 A-10 固定，可用会话静态保护由 A-11 固定；选做真实外部 provider 互操作验证。
6. 保持安全分类、能力矩阵和跨前端结果契约同步；全局/会话设置所有权已由 A-12 固定。
