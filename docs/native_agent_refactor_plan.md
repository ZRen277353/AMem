# NativeAgent 原生内存工具重构方案

状态：实施中，24 个 canonical Agent/IPC 名称、共享 `MemJsonTools`/`LuaJsonTool`、原生 driver/module/pointer/disassembly/scan/symbol/breakpoint/raw/typed memory、GUI breakpoint/symbol/scan、Lua host boundary、独立 mutation audit、连接生命周期、run target、受管工具/HTTP worker、端到端 payload 硬上限、全局 prompt/token 所有权、退役 alias 清理、Python MCP 与 legacy HTTP IPC 删除、native IPC framing/Hello/request session/catalog/完整 dispatcher/安全 transport/owned runtime/显式 GUI control/逐请求 privileged approval execution/security outcome audit 已落地
适用分支：`NativeAgent`
分支角色：独立的 Agent 产品分支，目前不以合并回 `dev` 为目标
基线提交：`0bf354f`
最后更新：2026-07-13

本文给出从原 AI Chat + HTTP IPC + Python MCP 基线迁移到“内置原生内存工具 Agent”的实施方案。Python MCP 与 legacy HTTP IPC 已删除；Named Pipe framing/session/transport、owned runtime、显式 GUI control、broker management、server session binding、bounded persistent security audit、fail-closed consume、approved dispatcher/send-boundary execution 与 final outcome summary 已落地。Native IPC 编译和运行默认关闭；Hello 仍只授予 Observe，privileged execution 通过逐请求审批与 durable one-shot grant 完成。

## 0. 当前进度

截至 2026-07-13 已完成四十四个纵向切片：

- 新增 `MemResult`、`TargetSnapshot`、`OperationContext`、`IMemBackend`、`IMemService` 和可注入的 `MemService`。
- `DeviceSession` 统一维护 shared request lease、exclusive lifecycle gate、单调 `connectionGeneration` 和 poison 状态；timeout、EOF 或 partial I/O 失败后旧连接不再复用。
- `WindowsSocketClient` 通过 `IWindowsSocketOps` 保留默认 Winsock adapter 并提供确定性故障注入；4 组测试覆盖真实 loopback、partial send/receive、timeout/EOF poison、旧 lease 失效和迟到字节的 reconnect generation 隔离。
- `MultiPortClientManager` 在一个 exclusive lifecycle lease 内持有 MAIN/DEBUG/ERROR client、目标清理回调、失败回滚和显式重连；6 组测试覆盖真实三连接 loopback、活动 request 排斥、单端口 poison、全端口替换和重复 generation。
- `AppContext` 可生成一致目标快照，进程切换遵循 connection -> process -> port 锁顺序。
- `status`、`driver_initialize`、`process_list`、`process_open`、module/pointer/disassembly/symbol resolution、canonical scan/symbol/breakpoint、raw/typed memory read/write 已通过薄 Agent adapter 调用 `MemService`。
- raw write 保留 request-started/response-received/written-byte 状态，区分发送前取消、`completion_unknown`、部分写和 deadline/cancel 后确认完成。
- `ValueCodec` 统一 byte/word/dword/qword/xor/float/double 的别名、范围、little-endian 编解码和精确文本；typed write 复用 raw write 回执。
- module list 统一分页和大小上限；module resolve 按完整名、basename、唯一子串依次匹配并拒绝歧义。协议入口限制单名 4096 bytes 和累计 16 MiB。
- 每端口新增可重入 transaction gate；普通 socket 命令短暂持有，pointer chain 跨 module list 和全部 8-byte read 持有。`pointer_resolve` 固定 generation/target，逐跳检查取消、deadline 和 uint64 overflow，释放事务后再复核完整 snapshot。
- scan mutation 统一推进单调 epoch；`scan_start` 在一个事务内执行 range+scan，refine/results/clear 要求最新 epoch，results 绑定 count+page，clear 用 count=0 确认。长扫描通过 DEBUG stop 响应 cancellation，sent-without-terminal 保留 `completion_unknown`。
- symbol init 在取得 transaction gate 后推进单调 epoch；`symbol_resolve`/`symbol_list` 在一个 MAIN transaction 内完成 module 唯一匹配、init 与 find/page，续页要求最新 epoch。GUI `loadSymbolTable` 一次 init 并在同一 transaction 读取全表；旧 IPC init 会使 native session 失效。
- GUI `ScanWindow` 显式注入 `IMemService`；start/refine/results/clear/remove 均携带 target context 与最新 epoch。进度和 cancellation 共用一个 callback，GUI Stop 只设置 token；确认完成、Stop 后确认完成和 `completion_unknown` 不再混淆。删除地址去重并在一个 scan transaction 内确认前后计数与新 epoch。
- breakpoint set/remove/suspend/resume 使用统一 tracked receipt，区分未发送、server reject、发送后未知和确认后 cancel/deadline；设备确认与 cleanup tracker 在同一 MAIN transaction 更新，disconnect 清本地 tracker。hits 使用无 cursor 的最新批次；Agent 上限 100 并把 64 位值转为字符串。
- `disassemble` 统一校验 ARM64 固定宽度 `count * 4`、拒绝短读，返回 little-endian encoding 和明确的 `decoded=false`；退役 `read_disassembly` 不再注册。
- `driver_initialize` 在 service/send 边界绑定 connection generation，区分未发送、server reject、`completion_unknown` 与确认后 cancel/deadline；runtime 只接受 `card`，旧历史中的 `card_name` 仍在降级文本中脱敏，原值仅瞬时用于执行/provider 连续性。
- `lua_execute` 是唯一内置 Lua 名称，仅在 `HAVE_LUAJIT` 时注册；执行前复核 target/generation，Lua timeout 不得延长 task absolute deadline，开始后取消只记录回执而不声称硬中止。
- write-classified 与 symbol-session outcome 在 UI callback 前写 `ai_mutation_audit.jsonl`；manual deny/queue rejection 同样记录。日志保存 approval/effect/resource/target/completion 的脱敏摘要，64 KiB/record、4 MiB active + 一个轮转备份，并由独立 Audit 表展示最近 100 条。
- GUI `BreakpointWindow` 显式注入 `IMemService`；set/remove/enable/suspend/resume 与 hit refresh 捕获 target context。hit DTO 保存完整 GPR/FPSIMD，DEBUG 响应按块排空并保留最新 50,000 条。
- `AgentRunContext` 在首轮模型请求前捕获 connection/target，审批、出队和结果回收均按 `None`/`Bound`/`Selection` 策略复核；`process_open` 成功后显式推进 run target。
- 审批框展示预期 connection generation、PID 和 process revision；晚到的旧目标成功结果不会回喂模型。
- `AgentTaskExecutor` 用单个 joinable worker 串行工具队列；`ToolExecutor` 同步执行，不再创建 inner detached future。shutdown 会停止接收、取消 active/queued task 并 join。
- provider SSE parser 已提取为纯组件并保留 `[DONE]`；Claude/OpenAI/DeepSeek 共用 terminal tracker 验证 start/terminal/malformed，截断 partial 只作为 `InvalidResponse` 展示且不执行工具。
- `HttpClient` 为每个请求持有 cancellation、transport stop hook 和 joinable worker；provider completion callback 返回后才完成，shutdown 禁止新请求并全量 join。
- 四类 AI 持久化 loader 统一五态结果并先校验临时状态再提交；损坏索引保留后扫描会话重建，失败会话不绑定写回，所有 session 保存使用统一原子安装助手。
- `AiLimits` 为 HTTP/SSE/provider/tool/session/persistence 建立分层硬上限；超限 mutation 保留 `completion_unknown`，会话按完整组裁剪且拒绝保持事务性，assistant/tool outcome 无法入会话时停止 Agent 链。
- `AiSettings` 是 system prompt/token limit 的唯一持久化所有者；session format v2 只保存历史，v1 同名字段迁移时忽略，load 后使用当前全局预算裁剪。
- NativeAgent 的 AI Chat、Capstone 和 Keystone 改为强制产品依赖；删除 `ENABLE_AI_CHAT` 与缺失依赖时的降级分支，CMake 始终装配源文件、链接库并定义三个 `HAVE_*` 实现宏。
- 33 个旧名称已从注册表和 JSON adapter 删除。LuaJIT 构建为 24 可执行 / 24 广告 / 0 hidden；无 LuaJIT 为 23/23/0。旧会话调用组只会降级为不可执行的 assistant 历史文本。
- Python FastMCP package、`.mcp.json`、安装元数据和 IDE 配置已删除；标准库 wire-protocol 探针迁至 `tools/protocol_reference/`，明确不参与产品运行或 Agent 集成。
- legacy `ipc/IpcServer.*`、CMake option/macro、main 启停和 loopback HTTP listener 已删除；`native_agent_no_legacy_http_ipc` 静态 gate 阻止旧 server/CORS/端口入口回归。
- `IpcProtocol` 固定 24-byte little-endian header、精确 `1.0` 版本、六种 message type 与 request-id 规则；request payload 硬限制 1 MiB，其他帧硬限制 4 MiB，并验证 UTF-8，partial frame 不消费输入。
- `ENABLE_NATIVE_IPC` 默认 OFF；`NativePipeSecurity` 只允许当前进程用户 SID 与 SYSTEM read/write，`NamedPipeServer` 拒绝 remote client、固定 first/single instance 并复用同一 handle。overlapped accept 与串行 handler 共用 joinable thread，stop event + `CancelIoEx` 后 join；状态快照有 lifecycle/count/name/error。
- `IpcFramedConnection` 在 payload 分配前调用 `DecodeHeader()`，用 overlapped exact read/write、绝对 deadline 和 stop event 处理 fragmented input、short write 与取消；跨 polling timeout 的 partial frame 有界保留并继续读取，partial close 是 protocol error。
- `IpcHandshakeSession` 把首帧限制为 16 KiB `Hello` JSON，校验 identity/capability schema，精确匹配 header `1.0`，仅授予请求的 `Observe`，拒绝任何请求的 privileged capability；错误 Hello 返回 structured `Error`，unsupported version/oversized header 直接关闭，拒绝 drain 最长 1 秒。
- `IpcRequestProtocol` 固定 strict `{method, params, timeout_ms?}`、空 object Cancel、30 秒默认/5 分钟最大 timeout、统一 completion/response envelope 和 output validation。
- `IpcRequestSession` 在 Hello 后维持 one-active reader/worker 状态机；request id 连接内终身去重并以 1024 项封顶，dispatcher server metadata 决定 required capability，相对 timeout 固定为 absolute deadline，client Cancel/deadline/Stop 共用 cooperative cancellation，worker owned/joinable。
- 原 `AgentMemTools` 实现提升为 AI/IPC 共享的 `MemJsonTools`；AI alias 保持调用面，地址/scalar/分页/parser/result 只有一份实现。
- `IpcMethodCatalog` 与 Agent 同步固定 24 个 canonical name：12 Observe、1 TargetSelection、9 TargetMutation、2 HostExecution；3 None、20 Bound、1 Selection。只有 Observe 可免审批执行。
- `IpcMemServiceDispatcher` 执行 12 个 Observe method；批准后消费 durable grant，11 个 privileged method 复用 `MemJsonTools`/`MemService`，`lua_execute` 走注入的 host executor。session baseline 绑定 connection/target，前后与 250 ms polling 复核；变化时发 session Error、取消 active context 并 join。deadline/Cancel 复用 service 原子 token；未绑定 broker/session 的 privileged direct call 返回 `approval_required`。
- `NativeAgentRuntime` 持有 `NamedPipeServer`，对每个 serial client 依次装配 framed connection、Hello、`IpcMemServiceDispatcher` 与 request session。Stop 取消并 join handler/dispatch worker；线程安全 snapshot 只保留 lifecycle、活动身份/capability、状态与有界计数，完成后清活动身份且不保存 params/results。
- `SystemNativeAgentRuntime` 延迟构造产品 owner 与 Lua host executor；`NativeAgentIpcWindow` 显示 server/phase/client/session/request 状态并提供显式启停。应用启动仍不监听；main 只在 device disconnect 前 shutdown。GUI 与 gate 明确显示 `Observe + 逐请求审批`。
- `IpcApprovalBroker` 固定 pending/approved/denied/invalidated/expired/cancelled/consumed 单向状态；catalog 决定 capability/target policy，记录不含 params/results，live/history 有界。approved 在一次性 consume 前仍会因 generation/target/deadline/session 失效，audit callback 在 broker 锁外执行。
- system owner 延迟持有 audit、broker、host executor 与 runtime；GUI refresh/decision 只消费 bounded record，Stop/shutdown cancel-all。静态 gate 禁止窗口引用 params/results，固定 Hello Observe-only，并要求 product dispatcher consume 与 grant-bound execution。
- 每次成功 Hello 分配 server-owned 单调 session id，stop/start 不复用 id；runtime 持有同一个 system broker 的非 owning 引用，每个 established session 的关闭、target/session invalidation、异常和 Stop 退出都精确 `cancelSession()`。snapshot 与 GUI 只显示当前/最近 id，不保存请求内容；runtime owner 本身不直接 submit。
- privileged request 由 owned worker 提交 bounded broker metadata，reader 保持可处理 Cancel；client Cancel 精确取消 request。批准后调用 `consume()` 并核对 grant metadata；审计失败或 consume 后 adapter/send 前 Cancel 都不会执行。终止 Error 后做 100 ms 可取消 drain。
- `IpcApprovalAuditLog` 持久化不含 params/results 的 broker transition：16 KiB/record、4 MiB active + `.1`、最近 100 条、bounded tail reload、损坏/超大行跳过和写盘失败状态；GUI 显示最近 20 条与 health。
- broker `consume()` 复核 approval/session/request/deadline/generation/target，先烧毁 record 再做锁外持久化；只有 durable consumed transition 返回 grant，失败不可重试，且与 session Cancel 只有一个 terminal winner。产品 dispatcher 已接 consume。
- `process_open` 在 mutex 保护下执行 controlled selection；只有 adapter 返回 snapshot、当前 service snapshot 与 grant generation/旧 baseline 全部一致时才推进 external session baseline。
- Native IPC completion 新增 `timed_out_before_start`、`timed_out`、`completed_after_deadline`，保留 deadline 后设备已确认完成的成功回执。
- security audit schema 2 在同一有界 JSONL 中区分 `approval_transition` 与 `execution_outcome`；outcome 只含 authorized/observed target、success、completion 与 bounded error code，schema 1 继续可加载。post-effect 写盘失败进入 GUI health，但不覆盖真实设备回执。
- `NativeAgentMemTests` 的 24 个测试组覆盖既有 service/Agent 边界和 tool-result 输出上限；native IPC 有 6 组 security-audit、12 approval-broker、5 protocol、5 transport、8 framed-I/O、8 handshake、6 request-contract、9 request-session、4 catalog、15 dispatcher 和 10 runtime 测试；socket client 有 4 组，multi-port manager 有 6 组；provider/SSE 有 18 组；persistence recovery/limits/global-settings ownership 有 8 组；HTTP lifecycle/response limit 有 5 组。五个静态 gate 加入 mandatory-feature contract 后当前共 22 项 CTest；两个 socket suite 各连续 100 次通过，四个 A-09 相关 executable 连续 20/20，本切片 fresh mandatory-feature Release 为 22/22 并完成产品链接。

尚未完成：Native IPC GUI approval click 与真实 Android privileged operation smoke；不同用户/remote 负向测试；真实 Android 三端口 timeout/reconnect 与远端状态恢复；真实 provider HTTP/TLS/full-response。规范目录、共享 adapter、catalog、完整 dispatch、owned runtime、session/request cancellation、persistent security audit、fail-closed consume、逐请求 approved execution/outcome、Python MCP 与 legacy HTTP 删除、native framing/session/transport、provider stream terminal validation、事务式 persistence recovery、owned HTTP lifecycle、端到端 payload 硬上限和全局 prompt/token 所有权已完成。A-01、A-04、A-05、A-06、A-09、A-12、A-14、A-17、A-18、A-19、A-20 已关闭；A-21 的 provider context 预算仍未关闭。

## 1. 结论

目标不是把 Python MCP 翻译成 C++，而是删除这层重复代理，并在应用内部建立唯一的原生内存业务层：

```text
ImGui Agent -----> Agent 编排/审批 ------+
                                       |
GUI windows ---------------------------+--> MemService
                                       |      -> DeviceSession
可选 Named Pipe IPC -------------------+      -> socket protocol
                                              -> Android server/device
```

核心决定如下：

1. `MemService` 成为进程、模块、内存、扫描、断点和符号能力的唯一业务入口；Agent、GUI 和可选 IPC 不再直接调用 `client_singleton.h`。
2. Python MCP 包已删除，其 wrapper、常量表和重试逻辑不复制到 C++。
3. Agent 只暴露一组稳定的规范工具名；兼容别名先隐藏、再迁移、最后删除。
4. 工具调用绑定 `{pid, processHandle, processRevision, connectionGeneration}`，审批后目标发生变化时必须拒绝执行。
5. detached 工具线程改成一个受管、可 join 的任务队列。取消具有明确状态，不再把“UI 不接收晚到结果”描述为操作已停止。
6. legacy HTTP IPC 已由默认关闭的 Windows Named Pipe control plane 取代；Native IPC 只做共享 `MemService` 的受限 adapter，不保留第二套业务实现。

## 2. 目标与非目标

### 2.1 目标

- AMem 安装后即可使用内置 Agent 的内存调试工具，不依赖 Python、FastMCP 或额外进程。
- 同一业务操作只有一套参数校验、目标校验、错误语义、取消语义和结果格式。
- 模型看到的工具更少、更稳定，避免别名和前置状态工具增加选择成本。
- 目标切换、断线重连、超时、取消和应用退出都有可证明的生命周期边界。
- 核心逻辑可在没有 Android 设备时通过 fake service/transport 自动测试。
- 保留 GUI 现有能力，迁移期间允许旧入口与新服务短期并存，但必须有删除期限。

### 2.2 非目标

- 本轮不修改 Android 服务端二进制协议。
- 不重做聊天界面或 provider 产品形态。
- 不用一个带 `action` 参数的超大工具替代全部内存工具。
- 不让 Agent 直接持有 socket、原始进程句柄或全局单例的可变引用。
- 不承诺已发往设备的写命令可以回滚或硬取消。

## 3. 目标组件与职责

### 3.1 `MemService`

`MemService` 是原生业务门面，负责：

- 统一解析地址、数值类型、扫描类型、内存范围和分页参数。
- 获取并验证目标快照。
- 将多个底层命令组织为一个业务操作，保护扫描集、符号表等共享状态。
- 把底层 `bool`/整数返回值转换为结构化结果和稳定错误码。
- 应用统一的大小、数量、耗时和输出截断限制。
- 记录操作 effect、duration、目标 revision 和 connection generation。

它不负责：

- JSON Schema 或 provider-specific tool 格式。
- ImGui 弹窗、聊天消息和文案。
- Named Pipe framing。
- 直接保存会话。

### 3.2 `DeviceSession`

`DeviceSession` 取代当前“全局三个 client + 分散状态检查”的连接生命周期管理：

- 一个 session 管理 main/debug/error 三个端口和单调递增的 `connectionGeneration`。
- 普通请求持有共享 connection lease；connect/disconnect/reconnect 持有独占 lifecycle gate。
- 每个端口仍可单独串行 request/response，但连接对象状态由 session 内同一同步边界保护。
- 任意可能发生 partial send/receive 的超时将对应连接标记为 poisoned；旧连接不再通过待处理字节清理路径复用。
- 重连生成新 generation，并使旧 process handle、扫描状态、断点跟踪和符号缓存失效。
- 长扫描的取消通过 debug 端口发送 stop；若协议不能确认停止，则结果标为 `cancel_requested` 或 `completion_unknown`，而不是 `cancelled`。

### 3.3 `AgentRunContext`

每次 run 在第一轮模型请求前捕获：

```cpp
struct TargetSnapshot {
    int pid = 0;
    int processHandle = 0;
    uint64_t processRevision = 0;
    uint64_t connectionGeneration = 0;
};

struct AgentRunContext {
    std::string runId;
    OperationContext operation;
};
```

规则：

- 非进程绑定工具可以不带 target。
- 进程绑定工具在审批前、出队执行前和返回结果前都校验 snapshot。
- `process_open` 成功后产生新 snapshot，并显式更新当前 run；不能悄悄沿用旧 snapshot。
- target 改变时，待审批和队列中尚未开始的旧工具全部失败为 `target_changed`。
- 晚到结果可以进入审计记录，但不得作为当前目标上的成功结果回喂模型。

### 3.4 `AgentTaskExecutor`

用一个拥有 joinable `std::thread` 的队列替代当前两层 detached 执行：

```text
AgentRunner -> enqueue ToolTask -> owned worker -> MemService -> ToolOutcome
                    |                                  |
                    +---------- cancellation ----------+
```

- 队列拥有 worker，应用退出时先停止接收、请求取消、排空或标记未完成任务，然后 join。
- executor 本身执行工具，不再为 timeout 创建第二层 detached packaged task。
- deadline 通过 `OperationContext` 传到 service 和 socket I/O。
- 写命令发送前可取消；发送后只能报告 `write_sent`、`completion_unknown` 或已确认结果。
- UI 只消费带 `runId` 和 target snapshot 的 outcome，不接触 worker 生命周期。

### 3.5 `AgentToolCatalog`

工具目录只负责模型接口：

- 规范名称和描述。
- JSON Schema。
- effect、安全等级、feature gate、幂等性和资源域元数据。
- JSON 参数到强类型 request 的转换。
- `MemResult<T>` 到统一 tool result 的转换。

工具实现中不再出现 socket command、`AppContext::Get()` 或重复的十六进制编码逻辑。

## 4. `MemService` 契约草案

C++17 没有 `std::expected`，可使用项目内轻量结果类型；不要用空数组、0 或 JSON 字符串同时表达成功与失败。

```cpp
enum class MemErrorCode {
    InvalidArgument,
    NotConnected,
    NoTarget,
    TargetChanged,
    ConnectionChanged,
    ConnectionPoisoned,
    Timeout,
    CancelRequested,
    CompletionUnknown,
    ProtocolError,
    Unsupported,
    PermissionDenied,
    InternalError,
};

struct MemError {
    MemErrorCode code;
    std::string message;
    bool retryable = false;
};

template <typename T>
struct MemResult {
    std::optional<T> value;
    std::optional<MemError> error;
    uint64_t durationMs = 0;
};

struct Unit {};

struct OperationContext {
    uint64_t connectionGeneration = 0;
    std::optional<TargetSnapshot> target;
    CancellationToken cancellation;
    std::chrono::steady_clock::time_point deadline;
};

struct OpenProcessResult {
    TargetSnapshot target;
    std::string name;
};
```

建议的业务 API 按域分组：

```cpp
class IMemService {
public:
    virtual ~IMemService() = default;

    virtual MemResult<Status> status(const OperationContext&) = 0;
    virtual MemResult<std::vector<ProcessInfo>> listProcesses(
        const OperationContext&, const ListRequest&) = 0;
    virtual MemResult<OpenProcessResult> openProcess(
        const OperationContext&, const OpenProcessRequest&) = 0;

    virtual MemResult<Page<ModuleInfo>> listModules(
        const OperationContext&, const ModuleListRequest&) = 0;
    virtual MemResult<ResolvedAddress> resolveModule(
        const OperationContext&, const ModuleResolveRequest&) = 0;
    virtual MemResult<ResolvedAddress> resolvePointer(
        const OperationContext&, const PointerResolveRequest&) = 0;

    virtual MemResult<MemoryBlock> readMemory(
        const OperationContext&, const MemoryReadRequest&) = 0;
    virtual MemResult<ScalarValue> readValue(
        const OperationContext&, const ValueReadRequest&) = 0;
    virtual MemResult<WriteReceipt> writeMemory(
        const OperationContext&, const MemoryWriteRequest&) = 0;
    virtual MemResult<WriteReceipt> writeValue(
        const OperationContext&, const ValueWriteRequest&) = 0;

    virtual MemResult<ScanSummary> startScan(
        const OperationContext&, const ScanStartRequest&) = 0;
    virtual MemResult<ScanSummary> refineScan(
        const OperationContext&, const ScanRefineRequest&) = 0;
    virtual MemResult<Page<ScanResult>> scanResults(
        const OperationContext&, const PageRequest&) = 0;
    virtual MemResult<Unit> clearScan(const OperationContext&) = 0;

    // breakpoint, symbol, disassembly and optional Lua methods follow
    // the same context/result contract.
};
```

接口约束：

- JSON 中的地址统一为带 `0x` 前缀的字符串，避免进制歧义和 JSON number 的 53-bit 精度问题。
- `MemResult` 必须恰好包含 value 或 error 之一；实现时通过工厂函数维护该不变量。
- 非进程工具仍校验 `OperationContext::connectionGeneration`；target 存在时，两处 generation 必须一致。
- `MemoryBlock` 内部保存 bytes；十六进制、ASCII、typed value 等展示形式由边界 adapter 生成。
- 列表统一返回 `items`、`total`、`nextCursor` 和 `truncated`。
- `ScanStartRequest` 一次包含范围、数据类型、模式和值/字节模式；不再依赖先调用 `scan_set_range`。
- symbol 初始化是 `listSymbols`/`resolveSymbol` 的服务内部细节，不作为 Agent 工具。
- service 的通用上限是安全边界；Agent/IPC 可以设置更小的调用预算，但不能绕过通用上限。

## 5. 规范工具集合

目标目录包含 24 个工具。迁移期曾把旧名称保留为“仅执行、不向模型广告”的 alias；当前 33 个旧名称已经删除。旧会话通过历史文本降级保留可读记录，不再依赖可执行 alias。

| 规范工具 | 替代当前名称 | 说明 |
|---|---|---|
| `status` | `get_status`, `get_server_version`, `get_architecture` | 一次返回连接、版本、架构、目标和 feature availability |
| `driver_initialize` | `init_driver` | 特权初始化，始终审批 |
| `process_list` | `get_process_list`, `list_processes` | 支持 filter/cursor/limit |
| `process_open` | `open_process` | 返回新的 target snapshot，始终审批 |
| `module_list` | `get_module_list`, `list_modules` | 统一分页和过滤 |
| `module_resolve` | `get_module_base` | 返回 base、size 和规范模块名 |
| `pointer_resolve` | `resolve_offset_chain` | 一次完成模块基址和指针链解析 |
| `memory_read` | `memory_read`, `read_memory` | 唯一 raw bytes 读取入口 |
| `memory_read_value` | `read_value` | typed scalar 读取 |
| `memory_write` | `memory_write`, `write_bytes` | 唯一 raw bytes 写入入口 |
| `memory_write_value` | `write_value` | typed scalar 写入 |
| `scan_start` | `scan_set_range` + `scan_value`/`scan_fuzzy`/`scan_hex` | 一次提交完整 scan request |
| `scan_refine` | `scan_next` | 过滤当前 scan session |
| `scan_results` | `get_scan_count`, `get_scan_results` | 结果页包含 total |
| `scan_clear` | `clear_scan` | 清理当前 scan session |
| `disassemble` | `read_disassembly` | 当前返回 bytes、little-endian encoding 和 `decoded=false`；不伪造 mnemonic |
| `breakpoint_set` | `set_breakpoint` | 目标状态变更，始终审批 |
| `breakpoint_remove` | `remove_breakpoint` | 目标状态变更，始终审批 |
| `breakpoint_hits` | `read_breakpoint_info` | 读取命中和寄存器信息 |
| `breakpoint_suspend` | `suspend_breakpoint` | 目标状态变更，始终审批 |
| `breakpoint_resume` | `resume_breakpoint` | 目标状态变更，始终审批 |
| `symbol_resolve` | `resolve_symbol`, `symbol_find` | service 内部处理初始化和 module base |
| `symbol_list` | `symbol_init`, `symbol_list` | 不暴露共享 active table 前置步骤 |
| `lua_execute` | `execute_lua` | 仅 `HAVE_LUAJIT` 时注册，始终审批，可配置关闭 |

不建议新增 `memory(action=...)` 或 `mem_tool(operation=...)`。这种大工具虽然名称少，但 schema 更复杂、审批更模糊、错误更难定位，也会让 provider 更难稳定选择参数。简洁化应来自去别名、合并前置状态和统一返回值，而不是把不同 effect 塞进同一个 action 分支。

## 6. Effect、审批与审计

将当前二元 `ReadOnly`/`Write` 扩展为：

| Effect | 示例 | 默认策略 |
|---|---|---|
| `Observe` | status、list、read、disassemble | 无审批，仍校验 target |
| `SessionMutation` | scan start/refine/clear、symbol cache 初始化 | 默认无需逐次审批，但记录审计并可由设置提升为需审批 |
| `TargetSelection` | process open/switch | 始终审批 |
| `ConnectionMutation` | driver init | 始终审批并绑定 connection generation |
| `TargetMutation` | memory write、breakpoint、freeze | 始终审批并绑定 target snapshot |
| `HostExecution` | Lua | 始终审批，可在构建或设置中彻底禁用 |

每个 tool descriptor 还应声明：

- `targetRequired`
- `idempotency`：safe / conditional / unsafe
- `resourceDomain`：connection / process / scan / symbol / breakpoint / lua
- `cancellation`：before-send / cooperative / unsupported-after-send
- `featureGate`
- 输入和输出预算

审批记录至少保存 tool 名、脱敏后的规范化参数摘要、effect、runId、target snapshot、决定和时间。driver card/key/token 必须永不进入显示或持久审计；内存写入数据可以按安全设置做摘要或脱敏，但不能只记录“用户已同意”而缺少目标。

## 7. IPC 取舍与新协议

### 7.1 默认决定

`NativeAgent` 已删除 legacy loopback HTTP 服务，只保留以下 Native IPC 构建选项：

```cmake
option(ENABLE_NATIVE_IPC "Compile native Named Pipe transport" OFF)
```

- 该选项把 codec/transport/runtime/GUI control 编入产品，但应用启动时仍不监听；只有用户在状态窗口点击“启用”才启动 Observe-only 服务。
- 未编译或用户未显式启用时，架构停在 GUI/Agent -> `MemService`。
- 需要外部脚本或 IDE 自动化时使用 Named Pipe；它只是 `MemService` 的受限 adapter，不拥有业务逻辑。

### 7.2 Named Pipe 设计

- 名称：`\\.\pipe\AMem.NativeAgent.v1`。
- 使用当前交互用户 SID 的 DACL，并允许 SYSTEM；拒绝其他用户和远程 pipe client。
- 服务默认关闭，由 GUI 设置显式开启；状态必须可见。
- 使用显式小端编码的 header，不直接发送 C++ struct 内存：magic、protocol version、message type、request id、payload length。
- payload 使用 UTF-8 JSON；保留 request/response 结构化契约，设置请求和响应总量上限。
- 首条消息完成版本和 capability handshake；不支持的版本立即关闭。
- 支持 request id、deadline 和 cancel 消息；取消状态与 `AgentTaskExecutor` 使用相同语义。
- 默认只开放 `Observe`。`TargetSelection`、`TargetMutation` 和 `HostExecution` 请求进入同一个 GUI approval broker，并绑定 target snapshot。
- 连接断开、目标切换或 generation 变化时，旧的外部审批和 capability 立即失效。

Named Pipe 的同用户 ACL 只能解决访问主体问题，不能替代危险操作审批。不要用“本机进程”作为默认允许任意写内存的理由。

## 8. 并发、事务和取消

### 8.1 资源域

端口 mutex 只保护一条底层 request/response，不能保护复合业务操作。`MemService` 需要按资源域增加业务锁或串行队列：

- `process`：open/close/target cleanup 与所有 target-bound 操作互斥。
- `scan`：set range + start、refine、results、clear 作为同一 scan session。
- `symbol`：init + list/find 作为同一 module symbol session。
- `breakpoint`：远端状态和本地 tracked set 同步更新。
- `connection`：请求 lease 与 reconnect/disconnect 互斥。

锁顺序必须固定为 connection -> process -> domain -> port，禁止 adapter 自己组合锁。

### 8.2 取消状态

统一 outcome：

- `cancelled_before_start`
- `cancelled_before_send`
- `cancel_requested`
- `completed_after_cancel_request`
- `completion_unknown`
- `completed`

用户按 Stop 后立即停止新的 model/tool 调度，并请求取消活动操作。只有 service/协议确认没有副作用时才显示 `cancelled`。写入已经发送、但响应超时时必须显示“完成状态未知”，同时 poison 连接并要求重新连接/重新选择目标。

## 9. 文件级变更地图

建议新增：

| 路径 | 职责 |
|---|---|
| `mem/MemTypes.h` | 强类型 request/result、地址和值类型、分页 |
| `mem/MemResult.h` | 稳定错误码和 `MemResult<T>` |
| `mem/IMemService.h` | 供 Agent/GUI/IPC 和 fake 使用的接口 |
| `mem/MemService.h/.cpp` | 业务操作、校验、事务和结果转换 |
| `socket/DeviceSession.h/.cpp` | connection lease、generation、poison/reconnect |
| `gui/ai/AgentRunContext.h` | run 与 target snapshot |
| `gui/ai/AgentToolCatalog.h/.cpp` | 规范工具目录和 JSON adapter |
| `gui/ai/AgentTaskExecutor.h/.cpp` | joinable worker、队列和取消 |
| `ipc/NamedPipeServer.h/.cpp` | 可选 transport adapter |
| `ipc/NativeAgentRuntime.h/.cpp` | compile-only transport/handshake/Observe session owner 与诊断快照 |
| `ipc/IpcProtocol.h/.cpp` | framing、handshake、request/response DTO |
| `tests/` | fake service、契约、状态机和协议测试 |

建议逐步修改：

- `gui/AppContext.*`：由 `MemService`/target store 管理一致快照，移除前端直接编排 cleanup command。
- `gui/ai/AgentRun.*`、`AgentRunner.*`、`AgentController.*`：携带 run context 和结构化 tool outcome。
- `gui/ai/ToolDefinitions.cpp`：迁移后拆为 catalog/schema 与很薄的 service adapter；最终删除重复 executor。
- `gui/ai/ToolExecutor.*`、`ChatWindow.cpp`：移除 detached worker 和 timeout 内层线程。
- `gui/ai/DefaultSystemPrompt.h`：只描述规范工具，不列 alias 或前置状态调用。
- `socket/client_singleton.*`、`socket/*Commands.cpp`：先作为 `MemService` 的 legacy backend；消费者迁完后收窄为内部协议层。
- `gui/*Window.cpp`：按域迁移到 `IMemService`，不再直接读取原始 handle。
- `main.cpp`、`CMakeLists.txt`：注入 service/session，删除 legacy HTTP IPC，增加可选 Named Pipe 和测试 target。
- `README.md`、`AGENTS.md`、`CLAUDE.md`、`scripts/README.md`：完成每阶段后更新事实描述。

最终删除或移动：

- [x] 删除 `.mcp.json`。
- [x] 删除 `mcp/amem_mcp/`、`mcp/configs/`、`mcp/server.py`、`mcp/pyproject.toml`、`mcp/requirements.txt`、`mcp/README.md` 和 `mcp/.gitignore`。
- [x] 将仍用于协议排障的标准库探针移动到 `tools/protocol_reference/`，并明确它不参与产品运行。
- [x] 删除旧 HTTP `ipc/IpcServer.*`、CMake option/macro、main 启停和端口/CORS 入口。
- [x] 用 `native_agent_no_legacy_http_ipc` gate 阻止旧 server、构建开关、监听地址和 CORS marker 回归。
- [x] 删除 README、IDE 配置和脚本中对 FastMCP、`python -m amem_mcp` 和 MCP 安装的引用。

## 10. 分阶段迁移

### Phase 0：冻结契约并建立测试支点（进行中）

变更：

- 保存迁移前 36/29/30 能力矩阵作为基线；迁移中的可执行/广告名称分开统计。
- 为地址解析、typed value 编解码、scan 参数映射和 tool schema 建立无设备测试。
- 引入 `IMemService` fake，覆盖 AgentRunner 的审批、target changed、取消和错误回喂。
- [x] 为 `WindowsSocketClient` 建立可注入 `IWindowsSocketOps`，覆盖 partial I/O、timeout/EOF poison 与 reconnect generation 隔离。

退出条件：测试可在 CI/CTest 独立运行；尚不改变用户可见工具行为。

### Phase 1：引入 `DeviceSession` 与 `MemService`（部分完成）

变更：

- 先包装 status、process list/open、module list、memory read/write。
- 保持旧函数存在，但只允许 `MemService` 新代码调用；增加直接调用清单防止继续扩散。
- 建立 generation、target snapshot、统一错误和 poison 连接规则。

退出条件：上述能力可通过 fake service 测试；超时后的连接不会被复用。

### Phase 2：迁移 Agent 工具和执行生命周期（已完成）

变更：

- 加入 `AgentRunContext`、effect metadata 和 target-bound approval。
- 使用 joinable `AgentTaskExecutor`，删除工具路径两层 detached。
- 24 个目标规范名称均已落地；23 个非 Lua 工具位于 service 边界，Lua 位于 host boundary/feature gate。
- 迁移期 hidden compatibility entry 已删除；旧会话调用组在 provider 边界转为不可执行的历史文本。
- mutation/session effect 在 completion callback 前写独立脱敏审计，Stop 不再把取消请求描述为副作用已撤回。

退出条件：Agent 工具实现中没有 raw socket command 或 `AppContext::Get()`；Stop 和 teardown 测试通过。

### Phase 3：迁移 GUI 与共享状态

变更：

- Process、Scan、Memory Viewer、Breakpoint、Modules 和 Lua 窗口逐个改用 service。
- 将 module/symbol/scan 状态放到明确的 session owner，不由多个前端共同修改裸全局字段。
- 清理 `SetCurrentPid()`、`EnsureOpenHandle()` 等隐式全局行为。

退出条件：除 `MemService`/协议实现外，仓库没有前端直接包含 `client_singleton.h` 的业务调用。

### Phase 4：以 Native IPC 替换 legacy HTTP（部分完成）

变更：

- [x] 删除 legacy HTTP server、构建 gate、main 启停和 loopback/CORS 控制面。
- [x] 固定显式小端 framing、版本/message/id/长度/UTF-8 校验和 partial decode 契约。
- [x] 实现 compile-only、default-off 的单实例 Named Pipe、当前用户/SYSTEM DACL、remote rejection、受管串行 handler、stop/join 和状态快照。
- [x] 实现有界 framed read/write、payload 分配前 header 校验、绝对 deadline 和 Stop cancellation。
- [x] 实现 exact-version Hello、16 KiB schema 边界与 Observe-only capability negotiation。
- [x] 实现严格 Request/Cancel DTO、bounded response、persistent one-active session、lifetime ID dedupe、deadline/client/Stop cancellation 和 server-owned capability enforcement。
- [x] 固定完整 24-name capability/target catalog，共享 `MemJsonTools`，接入 12 Observe `MemService` method，并使 connection/target baseline 变化失效。
- [x] 实现 compile-only owned runtime composition、线程安全有界 snapshot、顺序 client、Stop/join、invalidation 与 restart 测试。
- [x] 实现 compile-time opt-in、runtime default-stopped 的 GUI enable/status，延迟构造 system owner，并在 device disconnect 前 shutdown。
- [x] 固定不含 params/results 的 approval DTO、有界状态/历史、one-shot consume grant、target/generation/session/deadline invalidation 与 audit sink 接口。
- [x] system owner/GUI 接入 bounded pending snapshot、approve/deny、refresh invalidation 与 Stop/shutdown cancel-all；gate 保证 UI 无 params/results 且 Hello 仍 Observe-only。
- [x] runtime 为成功 Hello 分配跨 restart 单调 session id，并在 session close/invalidation/Stop 时精确取消绑定审批；gate 禁止 runtime 提交审批。
- [x] owned request worker 提交 bounded approval metadata，reader 保持 Cancel；deny/expire/invalidate/cancel 在 adapter 前终止。
- [x] system broker 注入 bounded persistent approval JSONL sink，GUI 显示 recent/health，sink 返回 durability status。
- [x] broker consume 复核 session/request/deadline/generation/target；consumed audit 失败时烧毁授权且不返回 grant，并与 session Cancel 线性化。
- [x] dispatcher 消费 durable one-shot grant，执行 11 个共享 `MemJsonTools`/`MemService` privileged adapter 和注入的 Lua host executor；`process_open` 受控推进 baseline，completion 保留 deadline 后确认语义。
- [x] 对每个 consumed grant 同步持久化无 raw params/results 的 final execution outcome；schema 1 reload、post-effect audit failure 和 GUI health 可见性已有测试/gate。
- [x] 增加 `native_agent_no_legacy_http_ipc` 静态 gate，禁止恢复旧 server、option/macro、监听地址和 CORS marker。
- [ ] 完成 GUI approval click 与真实设备 smoke。

退出条件：legacy HTTP 控制面不存在；Native IPC 不存在无审批的外部 target mutation 路径；GUI approval 与真实设备 smoke 有可重复记录。

### Phase 5：删除 Python MCP（已完成）

变更：

- 按第 9 节清单删除 Python 包、配置和文档。
- 将仍有价值的协议参考工具移出 `mcp/`。
- 更新项目总览、构建依赖和开发说明。

退出条件：构建、运行、测试和产品文档不要求 Python/FastMCP；`rg` 不再找到过期启动命令或 MCP 配置。`tools/protocol_reference/` 的可选标准库脚本不构成产品依赖。

### Phase 6：删除兼容别名（已完成）

变更：

- 迁移默认 prompt 和保存会话中的历史 tool name。
- 对无法迁移的旧 tool call/result 作为历史文本保留，不重新执行。
- 删除 hidden aliases 和旧 JSON adapter。

退出条件：模型只看到并只能新调用第 5 节的规范集合；内置 catalog 由 `ToolDefinitions.cpp` 单一来源和 `native_agent_catalog` CTest 校验。内置 Agent 与后续受限 transport 的完整能力矩阵随 Phase 4 收敛。

### Phase 7：完成 Agent 基础设施加固

变更：

- 修复截断 SSE 成功、provider context 预算、配置损坏覆盖、会话明文策略和设置所有权。（截断 SSE、配置损坏覆盖和 payload 总量上限已完成；其余仍待完成。）
- 将 HTTP provider worker 也改为受管生命周期。（已完成。）
- 为响应、历史和工具输出建立端到端预算。

退出条件：[`agent_project_issues.md`](./agent_project_issues.md) 中 A-04、A-05、A-09 至 A-12、A-15、A-18、A-21 均有回归测试和关闭证据。

## 11. 测试策略

### 11.1 无设备自动测试

- 地址：只接受显式 `0x`、溢出、空值、负数、JSON number 精度边界。
- typed value：整数范围、浮点、endianness、HEX 奇数长度和非法字符。
- schema：必填字段、额外字段、oneOf scan mode、分页和大小上限。
- target：审批期间切进程、断线重连、handle 相同但 revision/generation 不同。
- executor：排队取消、运行中取消、timeout、shutdown join、晚到结果。
- service：scan/symbol 复合操作不被其他 caller 插入，错误不会留下半更新本地状态。
- connection：partial send/receive、timeout/EOF poison、三端口回滚、request/disconnect exclusion 和重连 generation 已覆盖；Android 远端状态恢复仍待设备验证。
- provider：完整/截断/重复终止/malformed SSE，以及 HTTP/SSE/content/tool arguments 硬边界。
- persistence：损坏 JSON、字段类型错误、32 MiB 文件、8 MiB 消息、10,000/1,000 消息载入、16 MiB retained payload、临时文件替换失败和旧会话迁移。
- Native IPC：frame 分片、超大 payload、错误版本、重复 request id、ACL 和危险操作审批。

### 11.2 真实设备 smoke test

每个 release 至少验证：

1. 连接、列进程、打开目标、列模块。
2. 有界 read 和经人工确认的 write/read-back。
3. start/refine/results/clear scan 完整周期及中途 Stop。
4. breakpoint set/hit/read/suspend/resume/remove。
5. 操作中切换目标、断开和退出，不崩溃且不把结果归给新目标。
6. 若启用 Named Pipe，验证只读、审批写、取消和客户端异常退出。

## 12. 问题覆盖关系

| 阶段 | 主要覆盖问题 |
|---|---|
| Phase 0 | A-14、A-17、A-22 |
| Phase 1 | A-08、A-19、A-20 |
| Phase 2 | A-02、A-03、A-07、A-13、A-16、A-17 |
| Phase 3 | A-08、A-20 |
| Phase 4 | A-01、A-06、A-17 |
| Phase 5-6 | A-13、A-14、A-17 |
| Phase 7 | A-04、A-05、A-09、A-10、A-11、A-12、A-15、A-18、A-21 |

## 13. 验收标准

重构完成必须同时满足：

- Python MCP、FastMCP 配置和 legacy HTTP server 已不存在。
- Agent 在无 Python 环境中可以完成进程、模块、内存、扫描、断点和符号工作流。
- GUI、Agent 和可选 IPC 的设备业务调用都经过 `IMemService`。
- Agent 广告的工具只有规范名称，没有当前重复 alias 和 symbol/scan 前置状态工具。
- 每个进程绑定操作和审批都验证 revision + connection generation。
- 工具和 provider 后台线程由 owner join；退出不依赖固定 3 秒 best-effort wait。
- timeout/partial I/O 后旧连接不会继续承载新请求。
- Stop 的 UI、审计和 tool result 能区分“未开始”“已请求取消”“完成未知”。
- capability/feature gate 和结果契约来自单一 registry，Lua 等不可用功能不会继续向模型广告。
- 无设备测试进入 CTest/CI，真实设备 smoke checklist 有可重复记录。

## 14. 已落地的实现切片

第一批代码保持窄范围、可回滚：

1. 新增 `MemResult`、`TargetSnapshot`、`IMemService` 和 fake。
2. 只迁移 `status`、`process_list`、`process_open`、`memory_read` 四个规范工具。
3. 给这四个工具加入 target/generation、结果契约和单元测试。
4. 旧工具仍可运行，但不在这一批删除 MCP、IPC 或 GUI 直连。

第二批补充了：

1. 受管 `DeviceSession`、request/lifecycle lease、poison 和 generation 失效。
2. `ToolTargetPolicy::{None, Bound, Selection}` 与 context-aware executor overload。
3. run 创建时捕获 target，审批/出队/结果三阶段校验，以及 `process_open` 后显式推进快照。
4. 目标切换、重连、审批后执行前切换、同批 open/read、非目标工具和晚到结果测试。

第三批迁移 raw `memory_write`，并将 `write_bytes` 降为 hidden compatibility alias；service 可证明写入未发送、回执确认或 completion unknown，禁止对不确定写入自动重试。

第四批加入 joinable `AgentTaskExecutor`，删除 `ChatWindow` outer detached worker 和 `ToolExecutor::runExecutorAsync()` inner detached executor。队列固定 absolute deadline，Stop 传递 cancellation，shutdown 取消未开始任务并 join active worker；tool audit 记录规范 completion 状态。

第五批加入 `ValueCodec` 和 `MemService::readValue/writeValue`，注册 `memory_read_value`/`memory_write_value`，并将 `read_value`/`write_value` 降为 hidden compatibility aliases。规范 typed 地址要求 `0x`，qword 可用字符串保持完整精度，写操作保留 raw completion 状态。

第六批加入 `MemService::listModules/resolveModule`，注册 `module_list`/`module_resolve`，并隐藏 `get_module_list`、`list_modules`、`get_module_base`。解析不再静默取第一个子串匹配，模块响应也增加单名和累计字节上限。

第七批加入 per-port `SocketCommand::TransactionLease` 和 `MemService::resolvePointer`，注册 `pointer_resolve` 并隐藏 `resolve_offset_chain`。规范 offset 要求 `0x`，module 匹配拒绝歧义，module list 和全部 pointer read 处于同一事务；旧 GUI/Lua/IPC helper 也使用该 gate，避免跨前端插入。

第八批加入 `MemService` scan domain/session 和全局 scan epoch，注册 `scan_start`/`scan_refine`/`scan_results`/`scan_clear`，并隐藏八个旧名称。规范 start 一次携带 range/type/mode/value 或 byte pattern；refine/results/clear 显式携带 epoch，任何旧前端 mutation 都使旧 session 失效。扫描取消会请求 DEBUG stop，终态和完成未知不会混淆。

第九批加入 `MemService` symbol domain/session 和全局 symbol epoch，注册 `symbol_resolve`/`symbol_list`，并隐藏 `resolve_symbol`/`symbol_init`/`symbol_find`。module resolve + init + find/page 位于同一 MAIN transaction，续页携带最新 epoch；当前二元安全模型明确按目标/主机 mutation 审批，内部 symbol session mutation 保持 ReadOnly 且由事务审计语义约束。同时修正 scan/symbol epoch 的推进位置，使其发生在取得 transaction gate 后，保证 epoch 顺序与真实 mutation 顺序一致。

第十批加入 `MemService` breakpoint mutation/hit 读取，注册 `breakpoint_set`/`breakpoint_remove`/`breakpoint_hits`/`breakpoint_suspend`/`breakpoint_resume`，并隐藏五个旧名称。mutation 统一 target snapshot 和 request-started/response-received/applied 回执；设备确认与 cleanup tracker 在同一 transaction 内更新，cleanup 持 gate 避免并发 set 漏项，disconnect 清除本地旧 tracker。第十六批进一步纠正 hits 的无 cursor 协议语义。

第十一批加入 `MemService::disassemble` 与 canonical `disassemble`，隐藏 `read_disassembly`。ARM64 请求固定读取 `count * 4` 字节，短读视为协议错误；当前返回结构化 little-endian encoding 与 `decoded=false`，不在无解码器证据时伪造 mnemonic。

第十二批加入 connection-bound `MemService::initializeDriver` 与 canonical `driver_initialize`，隐藏 `init_driver`。tracked socket receipt 区分 request started、response received、server accepted/rejected 和 completion unknown；`ToolCallSecurity` 为执行参数保留瞬时原值，但审批显示、tool audit 和 session persistence 只使用脱敏副本。

第十三批加入 canonical `lua_execute` 并隐藏 `execute_lua`，两者只在 `HAVE_LUAJIT` 时注册。host 执行前复核 target/generation，实际 Lua hook deadline 取参数 timeout 与 Agent absolute deadline 的较早值；脚本开始后不承诺硬取消，晚到取消/超时按完成回执记录。

第十四批加入 `AgentMutationAuditLog`。executor 在 UI callback 前持久化 write 和 symbol-session outcome；manual deny/queue rejection 由 UI 边界补写。记录冻结 enqueue 时 safety/target policy，保存 approval、effect/resource、run/tool-call id、预期 target 和 completion；driver/Lua/raw-memory/bulk 字段脱敏，JSONL 有单条/总量/轮转上限。Stop、Clear、New、session switch/delete 与析构共享 active-run cancellation，Audit 表独立显示迟到最终状态。

第十五批开始迁移 GUI 消费者。`BreakpointWindow` 由 `CEWindow` 显式注入系统 `IMemService`，五个 mutation 入口不再直接调用 socket singleton；每次操作绑定当前 target/generation，并区分结构化失败与 confirmed success。GUI hit history 仍依赖包含 FPSIMD 的 `HW_HIT_INFO` 和 DEBUG 端口，因此留给后续 DTO/分页切片。

第十六批完成 GUI breakpoint hit 迁移并修正 canonical batch 语义。`BreakpointHit` 补齐 `orig_x0`、syscall、FPSR/FPCR 和 32 个 128-bit vector registers；socket 按块排空无 cursor 响应，只保留最新 tail。GUI 通过 service 读取最多 50,000 条，Agent 读取最多 100 条并返回 `available/dropped`；旧 `offset` 在 backend 前拒绝，不再返回虚假 `next_cursor`。

第十七批完成 GUI symbol cache 迁移。`loadSymbolTable` 在一个 symbol domain/MAIN transaction 内完成 module 唯一匹配、一次 init 和全部 1000-item page，限制 1,000,000 项与 64 MiB 名称；释放 transaction 后复核 target/epoch。`MemoryViewerWindow` 与 `BreakpointWindow` 显式传入 service，cache 安装时在 mutex 内再次复核 target/module，旧结果不能在进程切换 invalidate 后写回。

第十八批完成 GUI scan session 迁移。`ScanWindow` 由 `CEWindow` 显式注入系统 `IMemService`；首次扫描把内存区组合、数据类型、模式和值一次提交，refine/results/clear/remove 都携带最新 epoch 和 target context。结果页在一个 transaction 内绑定 count+page；选中删除先去重，再在一个 transaction 内读取前后 count 并要求 epoch 前进。GUI Stop 仅设置共享 cancellation token，system backend 在进度 callback 中发送一次 DEBUG stop；收到 terminal count 时保留 confirmed-after-cancel，未确认时保留结构化错误。同步旧扫描实现和 GUI 直连扫描命令已删除。

第十九批完成退役工具收敛。`ChatSession::getMessagesForRequest()` 识别 33 个退役名称：只要同一 assistant tool-call 组包含退役调用，就把整组转换为普通 assistant 文本，保留脱敏参数和已记录结果，不再发送 provider tool protocol，也不能重新执行。随后从 `ToolDefinitions.cpp` 删除全部 hidden alias、direct-socket executor、旧 schema/参数解析和 `client_singleton.h`/`AppContext.h` 依赖；`AgentMemTools` 同步删除 legacy mode，只接受 canonical 字段与显式 `0x` 地址。LuaJIT 目录收敛为 24/24/0，无 LuaJIT 为 23/23/0。新增 `native_agent_catalog` CTest 固定名称和 include 边界，原生测试增至 23 组。

第二十批删除 Python MCP 产品运行时。移除 FastMCP package、stdio 入口、pip metadata、`.mcp.json` 和六套 IDE 配置，不再维护第三套 schema、常量、retry 与 feature availability。独立的标准库 Android 协议探针迁至 `tools/protocol_reference/`，说明明确 C++ socket command 才是事实来源，脚本不具备 Agent 审批、target revision 或产品级安全边界。`native_agent_no_python_mcp` CTest 阻止旧目录、配置和产品启动说明回归。

第二十一批关闭 legacy HTTP IPC 默认入口。新增 `ENABLE_LEGACY_HTTP_IPC=OFF`，标准构建不包含 `IpcServer.cpp` 或 main start/stop 路径；显式 ON 时定义 `HAVE_LEGACY_HTTP_IPC`、编译旧实现并打印未鉴权 28100 警告。`native_agent_legacy_ipc_gate` 固定 CMake/main 边界；Debug/Release 默认构建与独立 opt-in 构建均通过。

第二十二批固定 native IPC framing contract。`IpcProtocol` 使用显式 24-byte little-endian header，定义精确 `1.0` 版本、六种 message type、request-id/zero-flags 规则、request 1 MiB 与其他帧 4 MiB 硬上限和严格 UTF-8。decoder 在 payload 分配前拒绝错误 header，partial 输入不消费，连续输入一次只取一帧；5 组 `native_ipc_protocol` 测试固定 wire bytes 和所有边界。该切片没有实现或启用 Named Pipe server、ACL、handshake 状态机、cancel routing 或 approval broker。

第二十三批加入安全 Named Pipe transport 基础。`ENABLE_NATIVE_IPC` 默认 OFF 且只控制编译，main 无 start；protected DACL 只含当前进程用户/SYSTEM read-write ACE，pipe 拒绝 remote、使用 first-instance + max-one 并在连接间复用同一 handle。overlapped accept 与 handler 串行运行在一个 owned thread，stop event + `CancelIoEx` 后 join；snapshot 暴露五态、连接数、名称和错误。5 组 `native_ipc_transport` 测试覆盖 ACL、flags/name、同用户连接、旧客户端未关闭时复用、名称独占和 shutdown；native gate 固定 default-off/no-main-start。Debug/Release 各 7/7 CTest 通过，transport 各连续 50 次无失败，独立 opt-in 配置/编译通过。

第二十四批加入 bounded frame I/O 与 Observe-only Hello 状态机。`IpcFramedConnection` 先读/验 24-byte header 再分配 payload，用 overlapped exact transfer、绝对 deadline 和 stop event 处理碎片、short write、timeout 与 cancellation。`IpcHandshakeSession` 要求首帧为最大 16 KiB 的 request-id 0 `Hello`，校验 identity/capability JSON，只 grant 请求的 `Observe`，拒绝 privileged capability；invalid Hello 返回 structured `Error`，unsupported version/oversized header 直接关闭，拒绝 drain 有 1 秒上限。7 组 framed-I/O 与 8 组 handshake 测试在 Debug/Release 各连续 50 次通过；完整 9/9 CTest 和 fresh `ENABLE_NATIVE_IPC=ON` 编译验证通过。产品仍无 start 路径。

第二十五批固定 Request contract 与 persistent session。`IpcRequestProtocol` 严格解析 `{method, params, timeout_ms?}`，拒绝 client capability/unknown fields，限制 method/timeout，并验证 Cancel、completion/result/error 与 response size。`IpcRequestSession` 使用 caller reader + owned serial worker，同一连接 one-active、1024 unique-id lifetime bound；server dispatcher metadata 在执行前拒绝 unknown/privileged method，relative timeout 变为 absolute deadline，client Cancel/deadline/Stop 发同一 cooperative token，invalid/duplicate/busy/count limit 都有 stable Error。6 组 contract 与 8 组 session 测试落地；Debug/Release 完整 11/11、session 各连续 50 次、fresh `ENABLE_NATIVE_IPC=ON` 静态库编译通过。产品仍无 start 路径或真实业务 dispatch。

第二十六批接入共享 catalog 与 Observe service adapter。原 `AgentMemTools.cpp` 提升为 `mem/MemJsonTools.cpp`，AI type alias 保持兼容，IPC 不再复制 23 个工具的 parser/result。`IpcMethodCatalog` 由 CTest 与 Agent 的 24-name 集合对齐，显式固定 12/1/9/2 capability 和 3/20/1 target policy。`IpcMemServiceDispatcher` 只执行 12 Observe，privileged direct call 返回 `approval_required`；dispatcher baseline 捕获 connection/target，reader 250 ms polling、request 前后和 service 自身三层复核，变化时以 `SessionInvalidated` 取消并 join。该 polling 还发现并修复了早期 wake 被误判为 request deadline 的竞态。新增 4 组 catalog、5 组 dispatcher 和第 9 组 session 测试；Debug/Release 完整 13/13、session 各连续 50 次、fresh opt-in 编译通过。

第二十七批加入 compile-only owned runtime。`NativeAgentRuntime` 持有 `NamedPipeServer`，为每个串行连接依次装配 `IpcFramedConnection`、`IpcHandshakeSession`、`IpcMemServiceDispatcher` 和 `IpcRequestSession`，自身不创建 detached 或额外 worker。Stop 通过 pipe stop event 取消握手/reader/active service context，并在返回前 join server handler 与 session dispatch worker。线程安全 snapshot 暴露 server/phase、accepted/established/completed session、活动 client identity/capability、最后 handshake/session 状态和有界计数；session 完成后清活动身份，结构上不保存 params/result。7 组真实 pipe runtime 测试覆盖 listening、Observe 调用、privileged denial、顺序 client、错误 Hello、target invalidation、握手/active request Stop、restart 和 snapshot 边界；Debug/Release 完整 14/14、runtime 各连续 50 次、fresh `ENABLE_NATIVE_IPC=ON` 静态库编译通过。

第二十八批加入显式 Observe-only 产品控制。`SystemNativeAgentRuntime` 只在打开控制窗口时延迟构造，避免 feature 编译即监听；`NativeAgentIpcWindow` 从主窗口菜单进入，显示 server/runtime phase、固定 Observe/privileged-disabled、pipe、活动 client、session/request/response/cancel 计数与错误，并提供同步启用/停止。`main.cpp` 不调用 start，只在 `DisconnectMultiPort()` 前 shutdown，保证 handler/dispatch worker 先 join。`native_agent_native_ipc_gate` 固定 `ENABLE_NATIVE_IPC=OFF`、用户动作是唯一 start、可见权限状态和 shutdown 顺序。Debug/Release 仍完整 14/14；fresh `ENABLE_NATIVE_IPC=ON` 的 `NativeIpcGuiControl` 静态库及 main/CEWindow 对象编译通过，未链接或覆盖用户产品产物。

第二十九批固定 privileged approval broker core。`IpcApprovalBroker` 只接受 catalog 中需要审批的方法，以 server-assigned `{sessionId, requestId}` 去重并从 catalog 推导 capability/target policy；client identity、finite deadline、connection generation 和 Bound/Selection target 在入队时验证。record 不含 raw params/result，pending 与 retained history 分别有界。decision 单向进入 approved/denied，approved 仍可被 invalidate/expire/cancel，只有 `consume()` 在执行前再次匹配 generation/target 后产生一次性 grant。audit sink 在 broker mutex 外接收每次状态转换，失败不改变 authorization。6 组测试覆盖校验、deny/approve/consume、queue/history、target/generation/session/deadline、audit re-entry 和并发提交；Debug/Release 完整 15/15，broker 压力 100/100，fresh opt-in GUI control 编译通过。grep 与既有 handshake/dispatcher 测试证明 broker 尚未进入产品调用链，privileged 仍 denied。

第三十批接入 privileged broker management surface。system owner 延迟持有 broker，窗口 refresh 先 expire 并按最新 `SystemMemService` context invalidate，再从 record 副本显示 pending client/method/capability/target/remaining deadline；approve/deny 只调用 facade decision，不执行工具。Stop button 与 main shutdown 在停 pipe 前 `cancelAll()` pending/approved。新增第 7 组 broker 测试固定 cancel-all 单向/idempotent；native gate 读取 window/owner/handshake source，要求 decision/approval-aware Stop，禁止 `params`/`resultJson`，并固定 Hello grant 仍只接受 Observe。Debug/Release 完整 15/15，broker 压力 100/100，fresh opt-in GUI control 静态编译通过；尚未执行真实 GUI click smoke。

第三十一批把 broker 绑定到 server session 生命周期。每次成功 Hello 由 runtime 分配单调 `uint64_t` session id；restart 清空本轮诊断，但 id 继续递增，避免旧审批与新连接重合。system owner 保证 broker 先构造、后析构，并把同一实例注入 runtime；每个 established session 的正常关闭、target/session invalidation、handler 异常和 Stop 退出都在清 runtime 状态前按 id `cancelSession()`。snapshot/GUI 增加当前与最近 session id。新增 2 组 runtime 测试，合计 9 组，覆盖 close/invalidation cancellation 与 restart 不复用；native gate 固定 broker 注入和 cancellation，并禁止 runtime 调用 `submit()`。Debug/Release 完整 15/15，runtime 压力 50/50，fresh `ENABLE_NATIVE_IPC=ON` GUI control 编译通过。Hello 仍只 grant Observe，runtime 没有 submission/consume。

第三十二批接入 non-executing privileged request submission。`IpcRequestSession` 仅在 server dispatcher 明确支持时让缺失 capability 的 method 进入 owned worker；worker 以 server session/request id、client identity、method、baseline 和 absolute deadline 提交 broker，raw params 只留在 active task。reader 等待期间继续处理 Cancel，broker 新增 exact `cancelRequest()`/`find()`；deny/expire/invalidate/cancel 返回明确 completion，approve 被转为 cancelled 并返回 `approval_execution_disabled`。静态 gate 要求 submission 并禁止 `consume()`，Hello 仍只 grant Observe。压力测试同时发现并修复两处 native I/O 竞态：partial header 跨 validation timeout 不再丢失；session-level 终止 Error 后有 100 ms 可取消 drain，避免立即 disconnect 截断 payload。测试增至 8 broker、8 framed-I/O、6 dispatcher、10 runtime 组；Debug/Release 15/15，runtime/framed 各 100/100，fresh opt-in GUI control 编译通过。

第三十三批加入独立 `IpcApprovalAuditLog`。system owner 依次持有 audit -> broker -> runtime，broker 所有 transition 在锁外同步写 `native_ipc_approval_audit.jsonl`。记录只含 transition timestamp、client/method/capability/state、session/request、generation/target 和 deadline remaining；单条 16 KiB，active 4 MiB + 一个 `.1`，内存最近 100 条。loader 只扫描每个文件尾部的有界数据并跳过损坏、错误类型和超大行；外部超大 active 在下次 append 时替换。GUI 显示路径、最近 20 条、成功/失败数和 last error。sink 返回 durability status，为下一切片 fail-closed consume 提供接口；当前 decision 不因 audit failure 回滚。新增 4 组 audit 测试，Debug/Release 16/16，audit 50/50、broker 100/100，fresh `ENABLE_NATIVE_IPC=ON` + `ENABLE_AI_CHAT=OFF` GUI 编译通过。

第三十四批实现 fail-closed broker consume。调用者必须同时提供 approval/session/request/current context；broker 在同一 mutex 临界区复核 identity、deadline、generation 和 target，并先把 approved record 置为 consumed。锁外 persistent audit 成功后才构造 one-shot grant；写盘失败返回 `approval_audit_failed`、不返回 grant，record 仍 consumed 且重试得到 `approval_not_approved`。阻塞 sink 与 64 轮双线程测试固定 consume/session Cancel 只有一个 terminal winner。approval-audit 增至 5 组、broker 增至 12 组；Debug/Release 16/16、audit 50/50、broker 100/100，fresh `ENABLE_NATIVE_IPC=ON` + `ENABLE_AI_CHAT=OFF` GUI control 编译通过。静态 gate 仍禁止 dispatcher consume，Hello 仍只 grant Observe。

第三十五批接入 approved privileged execution。`IpcMemServiceDispatcher` 在 worker 等待 approval 后调用 fail-closed `consume()`，校验 one-shot grant 的 session/request/method/capability/target policy，并在 adapter/send 前再次检查 Cancel/deadline。11 个 privileged method 复用 `MemJsonTools`/`MemService`；`lua_execute` 通过注入的 `IIpcHostMethodExecutor` 执行，共享实现提取为 `mem/LuaJsonTool`，AI Chat 同步复用。`process_open` 用 mutex 保护 controlled selection，只在返回 target 与当前 service snapshot 精确一致时推进 session baseline。completion enum 增加 `timed_out_before_start`、`timed_out`、`completed_after_deadline`。dispatcher 增至 14 组、runtime 保持 10 组；Debug/Release 16/16，dispatcher/runtime 各 50/50，fresh `ENABLE_NATIVE_IPC=ON` 在 `ENABLE_AI_CHAT=OFF` 与 `ON` 下均完成清理后全量产品链接。静态 gate 现要求 dispatcher consume/grant-bound execution 与共享 Lua target recheck，Hello 仍只 grant Observe。

第三十六批加入 privileged execution outcome audit。新增 `IpcExecutionAuditRecord`/sink，只允许 approval/session/request、client/method/capability、authorized/observed context、success/completion 和 bounded error code；不含 params、result JSON 或 error message。现有 `IpcApprovalAuditLog` 升级到 schema 2，以 `approval_transition`/`execution_outcome` 共用 16 KiB record、4 MiB active + `.1`、最近 100 条与 failure health，并兼容 schema 1。dispatcher 对每个 consumed grant 在 response 前同步记录一次；deny/expire/consume 前 Cancel 不记录。consume audit 仍 fail closed，post-effect outcome audit 失败不改写真实回执。GUI 改为“特权安全审计”并区分审批/执行事件。security-audit 增至 6 组、dispatcher 增至 15 组、runtime 保持 10 组；Debug/Release 16/16，audit/dispatcher/runtime 各 50/50。隔离 full product link：AI-off `29477888` bytes / `B2B9E2C2753E53ABBDB63A29485D1D150A64D901961D221A30DAAF39DEC1188A`，AI-on `35727360` bytes / `F8A55F89685DD90B2EB6368FEC5A69B6C17F8011BEDB87696C1733D23FBF10B1`。

第三十七批彻底删除 legacy HTTP IPC。删除 `ipc/IpcServer.cpp`/`.h`、`ENABLE_LEGACY_HTTP_IPC`、`HAVE_LEGACY_HTTP_IPC`、main 的端口启停和旧 `IPC_SOURCES`；原迁移 gate 替换为 `native_agent_no_legacy_http_ipc`，静态拒绝旧 server 文件、option/macro、`IpcServer`、loopback 监听地址和 CORS marker 回归。直接连接 Android 二进制协议的 `tools/protocol_reference/amem_client.py` 保留为排障工具，不是 GUI HTTP client。Debug/Release 各 16/16 CTest 通过。隔离 `ENABLE_NATIVE_IPC=ON` full product link：AI-off `29477888` bytes / `3F6CD0307932994E9BE77852A50F19FF9BF78CFC593D4DEB5229A0B59D88B200`，AI-on `35727360` bytes / `8DD0D1DB2595A98ABBFA75DE7AE8A2E229F8E5E1634D8042B650D2F1486092CB`。

第三十八批加入可注入 socket transport 与迟到字节回归。`WindowsSocketClient` 的 public send/receive/connect 形态保持不变，底层系统调用由默认 `SystemWindowsSocketOps` 或测试注入的 `IWindowsSocketOps` 提供。新增 `native_socket_client_transport` 的 4 组测试：真实 Winsock loopback 往返；partial send 继续剩余 offset 后 `WSAETIMEDOUT` poison；partial receive 后 EOF poison；旧 endpoint partial response + timeout 后 generation 失效，显式重连只读取新 endpoint，迟到旧字节仍留在已关闭 endpoint。Debug/Release 各 17/17 CTest，transport 各 100/100。隔离 `ENABLE_NATIVE_IPC=ON` full product link：AI-off `29478400` bytes / `F468391129BE4149209A2FE07B79A73FC2B4D865E12E9BEEC20D77625509B973`，AI-on `35727360` bytes / `B121C81942CDE51A4C2AD5FD4A0773F3B0B17B12B5061445621DF371F7CB8A85`。

第三十九批提取并验证三端口 lifecycle owner。新增 `MultiPortClientManager`，在一个 `DeviceSession` exclusive lease 内关闭旧 MAIN/DEBUG/ERROR、执行目标清理回调、顺序连接三端口并全量回滚失败；任一 client poison generation 后，新 request 全局拒绝，显式 reconnect 替换全部旧 endpoint。`WinSocketClientMgr` 改为委托该 owner，保留端口 mutex/transaction gate/epoch 和日志。新增 6 组 `native_multi_port_client_manager`：真实 Winsock 三连接 loopback、正常 connect/disconnect、MAIN/DEBUG/ERROR 逐端口失败回滚、单端口 timeout poison + 全端口重连、活动 request 阻塞 disconnect、50 轮 endpoint/generation 隔离。Debug/Release 各 18/18 CTest，socket 与 manager 各 100/100。隔离 `ENABLE_NATIVE_IPC=ON` full product link：AI-off `29480448` bytes / `ACA217055C4A9AD507030BE9D17ADE20846207B6AB5B4D62F4A0E10FFB98A7D3`，AI-on `35729920` bytes / `ED7AAEA0FEEE35C469EB070044AC2B0C48FF39FCB1C14EF75FF1E5BAAC52FF21`。

第四十批关闭 provider 截断流成功问题。`SSEParser` 从 `HttpClient.cpp` 提取为纯组件，不再吞掉 `[DONE]`；`StreamTerminalTracker` 统一记录合法 start、terminal 与首个 malformed/schema error。Claude 要求 `message_start`/`message_stop`，OpenAI/DeepSeek 要求合法 `choices` 起始并接受非空 `finish_reason` 或 `[DONE]`。三个 provider 对 HTTP 2xx 强制验证，失败时保留 partial content/tool calls 供 UI 展示，但 `CompletionResponse.error` 阻止工具执行。新增 14 组 `native_provider_stream_terminal`，Debug/Release 完整 19/19 CTest；隔离 `ENABLE_NATIVE_IPC=ON` fresh full product link：AI-off `29480448` bytes / `D4783B1C77C609DB02DAFE3C2A3E6333DA775438A943DC4D0C2A02CC7041C01B`，AI-on `35746816` bytes / `3A65E74C75E4D5B6E1730F48D9CDE1796A8A6F78A0C9EA18BD5113A563FA7406`。

第四十一批关闭 A-04。新增统一 `PersistenceLoadStatus`：`Loaded`、`Missing`、`Recovered`、`Invalid`、`IoError`；`ApiKeyStore`、`AiSettings`、`SessionManager` 和 `ChatSession` 都在临时状态中完整解析/校验后提交，只有缺失才创建默认文件。损坏索引保留为 `.corrupt*` 后扫描合法会话重建，无法保留原件时禁止自动写回；失败会话不再绑定原路径，`saveBound()` 防止退出/切换覆盖，写入统一走 `installTempFile()`。新增 `native_persistence_recovery` 4 组测试并连续 50/50 通过，完整套件 20/20。隔离 `ENABLE_NATIVE_IPC=ON` fresh Release full product link：AI-off `29480448` bytes / `FC54C68056BC718FFE5A3F7640A5E2E11A261912EE01642E010A6C6232EB8587`，AI-on `35794944` bytes / `5FB98A4BB85B0D3338861BCC8A662969CBB0329E010A316C6E6F8928513AF7BB`。

第四十二批关闭 A-05。`HttpClient` 删除 detached、`inFlight_` 与 3 秒 bounded wait，为每个请求保存 cancellation token、best-effort `Client::stop()` hook 和 joinable thread；completion callback 返回后才标记完成，下次 dispatch 回收已完成线程，进程 shutdown 禁止新请求并 join 全部 active/completed worker。从自身 callback 调用 shutdown 显式返回 `false`，主退出路径检查该结果。新增 `native_http_client_lifecycle` 4 组本机 HTTP 测试并连续 100/100 通过，完整套件 21/21。隔离 `ENABLE_NATIVE_IPC=ON` fresh Release full product link：AI-off `29480448` bytes / `CF35E2B998889E3975952A26610E51C5DBA67CAEE89A1313151CC2D16B6F87D2`，AI-on `35806720` bytes / `E616AEBBE5659F864A5323E9AD1948AF00F3E65C9F4ECD44E459BDC061653760`。

第四十三批关闭 A-09。新增统一 `AiLimits` 与 provider response helper：HTTP 原始累计 16 MiB、SSE line/event 1/2 MiB、assistant/tool result 4 MiB、普通消息 8 MiB，tool calls/id/name/arguments 为 64、256/64 bytes、512 KiB/调用与 4 MiB/消息。三个 provider 的流式/非流式路径在追加/物化前校验并把超限归类为 `InvalidResponse`；最终 tool JSON 超限会释放，mutation 返回 `completion_unknown`，tool audit 最终收缩到消息上限。持久化在 parse 和 session install 前限制 32 MiB；磁盘消息最多 10,000、只物化最后 1,000，retained payload 16 MiB，runtime 按完整对话组裁剪且失败不丢旧历史。assistant/tool outcome 无法入会话时停止后续执行/follow-up。测试增至 provider/SSE 18 组、persistence 7 组、HTTP 5 组、Agent 24 组，四个 executable 连续 20/20，完整 21/21。隔离 `ENABLE_NATIVE_IPC=ON` fresh Release full product link：AI-off `29480448` bytes / `6607AB90B21FF6F5B6E0CABA2FCEAF0D633A86A49D29546D2B03F5486AC4E196`，AI-on `35843072` bytes / `E6B0DCEED9B3F9A0AD36854550E62C63EDEE7A053CC06EF762B4B3D272A924A8`。

第四十四批关闭 A-12 并收敛 NativeAgent 构建基线。`AiSettings` 成为 system prompt/token limit 的唯一持久化所有者；session format v2 只保存 version/messages，v1 的旧同名字段无论类型是否有效都忽略，成功 load 后立即按 live global token budget 裁剪。persistence 增至 8 组，覆盖 v1 migration、v2 save/reload 和 load-time global budget，连续 50/50 通过。CMake 同时删除 `ENABLE_AI_CHAT`、AI source existence filtering 及 Capstone/Keystone/OpenSSL 缺失时的降级分支；AI Chat、Capstone 和 Keystone 现在是强制依赖，缺失即配置失败，三个 `HAVE_*` 仅保留为固定实现宏。标准 cache 不再含 `ENABLE_AI_CHAT`；传入旧 `-DENABLE_AI_CHAT=OFF` 仍装配 AI/Capstone/Keystone，无效 Capstone/Keystone 路径或禁用 OpenSSL 查找都会按预期配置失败。`native_agent_required_features` 固定该契约。隔离 `ENABLE_NATIVE_IPC=ON` fresh mandatory-feature Release 完整 22/22 CTest 并完成产品链接：`35798528` bytes / `3C8AB99975B2D368FA3FF5E3F33FE829186ADC7E2F6588C2D3CDE6B85E47A18C`。

四十四个切片已落地。下一批应处理 A-15 provider key 删除/设置草稿和 A-21 provider-aware context 预算，并完成 GUI approval click、真实 Android device operation、跨用户/session 与 remote-client 负向验证，以及真实设备三端口 timeout/reconnect 后的 driver/process/scan/breakpoint 状态记录。真实 provider HTTP/TLS/full-response 和 JSON DOM allocator 故障注入仍缺自动化；静默 read 可能让 owned shutdown 等待配置的 I/O timeout，但不得恢复 bounded wait 或 detached。不得把逐请求 grant 扩大为 Hello 中的长期 privileged capability；outcome audit 也不是设备事务日志，进程在 effect 与 flush 之间崩溃仍可能缺失记录。
