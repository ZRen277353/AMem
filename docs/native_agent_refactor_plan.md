# NativeAgent 原生内存工具重构方案

状态：实施中，24 个 canonical Agent/IPC 名称、共享 `MemJsonTools`、原生 driver/module/pointer/disassembly/scan/symbol/breakpoint/raw/typed memory、GUI breakpoint/symbol/scan、Lua host boundary、独立 mutation audit、连接生命周期、run target、受管工具 worker、退役 alias 清理、Python MCP 删除、HTTP IPC default-off gate、native IPC framing/Hello/request session/catalog/Observe adapter/安全 transport/owned runtime/显式 GUI control/privileged broker management surface 已落地
适用分支：`NativeAgent`
分支角色：独立的 Agent 产品分支，目前不以合并回 `dev` 为目标
基线提交：`0bf354f`
最后更新：2026-07-13

本文给出从原 AI Chat + HTTP IPC + Python MCP 基线迁移到“内置原生内存工具 Agent”的实施方案。Python MCP 已删除，HTTP IPC 仍待替换或删除；Named Pipe 的 framing、有界 I/O、Observe-only Hello、严格 request session、安全 transport、owned runtime composition、显式 GUI control 和 privileged broker management surface 已落地。编译和运行均默认关闭，用户只能显式启用 Observe；broker 尚无产品 submission/consume 链。当前实现和真实调用链见 [`agent_architecture.md`](./agent_architecture.md) 与 [`agent_walkthrough.md`](./agent_walkthrough.md)，已确认问题见 [`agent_project_issues.md`](./agent_project_issues.md)。

## 0. 当前进度

截至 2026-07-13 已完成三十个纵向切片：

- 新增 `MemResult`、`TargetSnapshot`、`OperationContext`、`IMemBackend`、`IMemService` 和可注入的 `MemService`。
- `DeviceSession` 统一维护 shared request lease、exclusive lifecycle gate、单调 `connectionGeneration` 和 poison 状态；timeout、EOF 或 partial I/O 失败后旧连接不再复用。
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
- 33 个旧名称已从注册表和 JSON adapter 删除。LuaJIT 构建为 24 可执行 / 24 广告 / 0 hidden；无 LuaJIT 为 23/23/0。旧会话调用组只会降级为不可执行的 assistant 历史文本。
- Python FastMCP package、`.mcp.json`、安装元数据和 IDE 配置已删除；标准库 wire-protocol 探针迁至 `tools/protocol_reference/`，明确不参与产品运行或 Agent 集成。
- `ENABLE_LEGACY_HTTP_IPC` 默认 OFF；标准构建不加入 `IpcServer.cpp`，`main.cpp` 的 include/start/stop 受 `HAVE_LEGACY_HTTP_IPC` 约束。显式 opt-in 会打印未鉴权端口警告，供迁移验证。
- `IpcProtocol` 固定 24-byte little-endian header、精确 `1.0` 版本、六种 message type 与 request-id 规则；request payload 硬限制 1 MiB，其他帧硬限制 4 MiB，并验证 UTF-8，partial frame 不消费输入。
- `ENABLE_NATIVE_IPC` 默认 OFF；`NativePipeSecurity` 只允许当前进程用户 SID 与 SYSTEM read/write，`NamedPipeServer` 拒绝 remote client、固定 first/single instance 并复用同一 handle。overlapped accept 与串行 handler 共用 joinable thread，stop event + `CancelIoEx` 后 join；状态快照有 lifecycle/count/name/error。
- `IpcFramedConnection` 在 payload 分配前调用 `DecodeHeader()`，用 overlapped exact read/write、绝对 deadline 和 stop event 处理 fragmented input、short write 与取消；partial frame close 是 protocol error。
- `IpcHandshakeSession` 把首帧限制为 16 KiB `Hello` JSON，校验 identity/capability schema，精确匹配 header `1.0`，仅授予请求的 `Observe`，拒绝任何请求的 privileged capability；错误 Hello 返回 structured `Error`，unsupported version/oversized header 直接关闭，拒绝 drain 最长 1 秒。
- `IpcRequestProtocol` 固定 strict `{method, params, timeout_ms?}`、空 object Cancel、30 秒默认/5 分钟最大 timeout、统一 completion/response envelope 和 output validation。
- `IpcRequestSession` 在 Hello 后维持 one-active reader/worker 状态机；request id 连接内终身去重并以 1024 项封顶，dispatcher server metadata 决定 required capability，相对 timeout 固定为 absolute deadline，client Cancel/deadline/Stop 共用 cooperative cancellation，worker owned/joinable。
- 原 `AgentMemTools` 实现提升为 AI/IPC 共享的 `MemJsonTools`；AI alias 保持调用面，地址/scalar/分页/parser/result 只有一份实现。
- `IpcMethodCatalog` 与 Agent 同步固定 24 个 canonical name：12 Observe、1 TargetSelection、9 TargetMutation、2 HostExecution；3 None、20 Bound、1 Selection。只有 Observe 可免审批执行。
- `IpcMemServiceDispatcher` 执行 12 个 Observe method，session baseline 绑定 connection/target，前后与 250 ms polling 复核；变化时发 session Error、取消 active context 并 join。deadline/Cancel 复用 service 原子 token；privileged direct call 在 service 前返回 `approval_required`。
- `NativeAgentRuntime` 持有 `NamedPipeServer`，对每个 serial client 依次装配 framed connection、Hello、`IpcMemServiceDispatcher` 与 request session。Stop 取消并 join handler/dispatch worker；线程安全 snapshot 只保留 lifecycle、活动身份/capability、状态与有界计数，完成后清活动身份且不保存 params/results。
- `SystemNativeAgentRuntime` 延迟构造产品 owner；`NativeAgentIpcWindow` 显示 server/phase/client/session/request 状态并提供显式启停。应用启动仍不监听；main 只在 device disconnect 前 shutdown。GUI 与 gate 明确显示 Observe-only/privileged disabled。
- `IpcApprovalBroker` 固定 pending/approved/denied/invalidated/expired/cancelled/consumed 单向状态；catalog 决定 capability/target policy，记录不含 params/results，live/history 有界。approved 在一次性 consume 前仍会因 generation/target/deadline/session 失效，audit callback 在 broker 锁外执行。
- system owner 延迟持有 broker；GUI refresh/decision 只消费 bounded record，Stop/shutdown cancel-all。静态 gate 禁止窗口引用 params/results 并固定 Hello Observe-only；当前无 product submission/consume。
- `NativeAgentMemTests` 的 23 个测试组覆盖既有 service/Agent 边界；native IPC 有 7 组 approval-broker、5 组 protocol、5 组 transport、7 组 framed-I/O、8 组 handshake、6 组 request-contract、9 组 request-session、4 组 method-catalog、5 组 MemService-dispatcher 和 7 组 runtime 测试，catalog、no-Python-MCP、legacy/native IPC gate 固定其余边界，共 15 项 CTest。

尚未完成：HTTP IPC transport 的最终删除；Native IPC broker 的 submission/session/dispatcher/persistent-audit 接入与 GUI click smoke；不同用户/remote 负向集成测试；以及连接层 fake transport 的 timeout/迟到字节测试。规范模型目录、共享 JSON adapter、24-name catalog、12 Observe service adapter、session target/generation invalidation、owned runtime composition、显式 Observe GUI control、privileged broker management surface、退役历史兼容、Python MCP 删除、HTTP IPC 默认关闭、native framing/session/安全 transport、所有当前内置工具的 service/host target 边界、Stop 后 mutation 独立审计和 GUI breakpoint/symbol/scan 迁移已经完成。因此 A-02、A-03 与 A-07 已关闭；A-01、A-06、A-19、A-20 仍只能视为部分修复。

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
6. 若仍需要外部自动化，HTTP IPC 替换为默认关闭的 Windows Named Pipe；如果没有外部调用方，则直接删除 IPC，不保留第二套公开接口。

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

`NativeAgent` 不再自动启动 `127.0.0.1:28100` HTTP 服务。构建选项建议为：

```cmake
option(ENABLE_NATIVE_IPC "Compile native Named Pipe transport" OFF)
```

- 该选项把 codec/transport/runtime/GUI control 编入产品，但应用启动时仍不监听；只有用户在状态窗口点击“启用”才启动 Observe-only 服务。
- 没有明确外部调用方时：删除 IPC，架构停在 GUI/Agent -> `MemService`。
- 仍需外部脚本或 IDE 自动化时：实现 Named Pipe，但它只是 `MemService` 的受限 adapter，不拥有业务逻辑。

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
- `main.cpp`、`CMakeLists.txt`：注入 service/session，移除 HTTP IPC 自动启动，增加可选 Named Pipe 和测试 target。
- `README.md`、`AGENTS.md`、`CLAUDE.md`、`scripts/README.md`：完成每阶段后更新事实描述。

最终删除或移动：

- [x] 删除 `.mcp.json`。
- [x] 删除 `mcp/amem_mcp/`、`mcp/configs/`、`mcp/server.py`、`mcp/pyproject.toml`、`mcp/requirements.txt`、`mcp/README.md` 和 `mcp/.gitignore`。
- [x] 将仍用于协议排障的标准库探针移动到 `tools/protocol_reference/`，并明确它不参与产品运行。
- [x] 旧 HTTP IPC 默认不编译、不监听，仅保留带警告的显式迁移 opt-in。
- [ ] 新 IPC 上线后删除 `ipc/IpcServer.*`；若不保留外部自动化，则整个 `ipc/` 可删除。
- [x] 删除 README、IDE 配置和脚本中对 FastMCP、`python -m amem_mcp` 和 MCP 安装的引用；端口 28100 的风险说明保留到旧 IPC 删除。

## 10. 分阶段迁移

### Phase 0：冻结契约并建立测试支点（进行中）

变更：

- 保存迁移前 36/29/30 能力矩阵作为基线；迁移中的可执行/广告名称分开统计。
- 为地址解析、typed value 编解码、scan 参数映射和 tool schema 建立无设备测试。
- 引入 `IMemService` fake，覆盖 AgentRunner 的审批、target changed、取消和错误回喂。
- 为现有 socket command 建立可注入的 fake transport 或最小协议 fixture。

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

### Phase 4：替换或删除 IPC（部分完成）

变更：

- [x] 关闭 HTTP server 的默认编译和启动。
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
- 有 privileged 外部调用需求时继续接入 request submission、session lifecycle、dispatcher send boundary 与持久化审计；完成前不 grant privileged capability。
- 没有需求时直接移除 IPC source 和 CMake wiring。

退出条件：端口 28100 不再监听；不存在无审批的外部 target mutation 路径。

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

- 修复截断 SSE 成功、provider context 预算、配置损坏覆盖、会话明文策略和设置所有权。
- 将 HTTP provider worker 也改为受管生命周期。
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
- connection：partial send/receive、timeout poison、重连 generation、并发 disconnect。
- provider：完整/截断/重复终止/malformed SSE。
- persistence：损坏 JSON、字段类型错误、临时文件替换失败、旧会话迁移。
- IPC（若保留）：frame 分片、超大 payload、错误版本、重复 request id、ACL 和危险操作审批。

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

- Python MCP、FastMCP 配置和 HTTP 28100 server 已不存在。
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

三十个切片已落地。模型可见规范目录、共享 JSON adapter、完整 IPC catalog、12 Observe service dispatch、session generation invalidation、owned runtime composition、显式 Observe GUI control、privileged broker management surface、当前所有内置工具的 target/send 边界、Stop 后 mutation 审计、退役历史兼容、Python MCP 删除、HTTP IPC 默认关闭与 native transport/session 基础均已完成。下一批应先设计 privileged Request 如何在不阻塞 reader/Cancel 的前提下提交 broker，并把 session close/invalidation 精确映射到 broker session id；仍不修改 Hello grant。随后再接 persistent audit 和 dispatcher consume/send boundary。整条链完成前 `TargetSelection`、`TargetMutation` 与 `HostExecution` 仍永久 denied。legacy HTTP IPC 仍待最终删除。
