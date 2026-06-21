# AMem AI Agent 架构文档

适用分支：`AIChat`　|　最后更新：2026-06-21

本文档描述 AMem 内置 **AI Chat Agent** 子系统(`gui/ai/`,编译开关 `HAVE_AI_CHAT`)的整体架构:分层、核心数据结构、线程模型、运行状态机、工具系统与持久化。配套的代码走读 / 设计原理见 [`agent_walkthrough.md`](./agent_walkthrough.md)。

> 术语:本文中的 **Agent** 专指应用内的 AI 对话代理(`ChatWindow` 驱动、可调用工具操控调试器)。它与「外部 AI 助手通过 MCP 调用」是**两条并行的前端**,但最终都落到同一套 `socket/client_singleton.h` 设备协议函数上——见最后一节。

---

## 1. 总览

整个子系统位于 `AI` 命名空间,几乎全部用 Meyer's 单例,并在 `ChatWindow` 构造时惰性装配(`initBuiltin*` 幂等)。它做的事:

1. 把用户消息 + 历史 + 工具定义组装成 provider 请求;
2. 在后台线程发起 HTTPS(支持 SSE 流式),把 token / 完成 / 错误回投到 UI 线程;
3. 模型若返回 tool_call,运行 **model→tool→model 循环**:执行工具(写类工具需用户审批)→ 把结果回喂模型 → 直到模型给出最终文本或触达预算上限;
4. 全程持久化会话历史、记录可观测的 trace。

```
                              ┌──────────────────────────────────────────────┐
   用户 (ImGui 主线程)        │                  AI 命名空间                    │
        │                     │                                                │
        ▼                     │   ChatWindow ──▶ AgentController ──▶ AgentRunner│
   ┌─────────────┐            │      │  ▲             │                  │      │
   │  ChatWindow │────────────┼──────┘  │             ▼                  ▼      │
   │  (UI/呈现)  │◀── poll ───┤      ProviderRegistry  ToolExecutor ── ToolDefs │
   └─────────────┘   Messages │          │                  │                  │
        ▲                     │          ▼                  │                  │
        │  UIMessageQueue     │   ClaudeProvider /          │                  │
        │  (唯一跨线程桥)      │   OpenAIProvider /          │                  │
        │                     │   DeepSeekProvider          │                  │
   ┌────┴───────┐             │          │                  │                  │
   │ 后台 HTTP   │◀───────────┼──── HttpClient (cpp-httplib+OpenSSL)            │
   │ worker 线程 │            └──────────┼───────────────────┼─────────────────┘
   └────────────┘                        │ (HTTPS/SSE)       │ (工具执行)
        │                                ▼                   ▼
        │                         AI Provider API   socket/client_singleton.h
        ▼                         (api.anthropic…)   (设备协议:读写/扫描/断点/符号/Lua)
   AI Provider                                              │
                                                            ▼
                                                      Android 设备
```

---

## 2. 分层与职责

| 层 | 主要类 / 文件 | 职责 | 线程 |
|----|--------------|------|------|
| **UI / 呈现** | `ChatWindow`,`ChatWindowSettings.cpp` | 绘制对话、设置面板、审批弹窗;`pollMessages()` 消费跨线程消息;持有 `cancelToken_`、run-id、`State` | 仅主线程 |
| **编排** | `AgentController` | provider 查找、请求构造、异步派发、run 状态机、trace 累积 | 主线程 |
| | `AgentRunner` | 纯逻辑的 model→tool→model 循环:步数/调用预算、审批门、跳过级联、生成审计消息(**不含任何 ImGui**) | 主线程 |
| **Provider** | `AIProvider`(抽象)、`ProviderRegistry` | provider 抽象接口与注册表 | 主线程查询 |
| | `ClaudeProvider` / `OpenAIProvider` / `DeepSeekProvider` | 把 `ChatMessage[]`+工具定义转成各家 JSON;解析 SSE/最终响应 | 构造在主线程,I/O 在 worker |
| | `HttpClient` | 封装 cpp-httplib(HTTPS via OpenSSL),`postAsync` 在 detached worker 上跑、SSE 解析、取消、退出排空 | 后台 worker |
| **工具** | `ToolExecutor` | 线程安全工具注册表 + 校验执行(JSON 解析 → schema 校验 → 异步执行 + 超时) | 注册主线程 / 执行在工具线程 |
| | `ToolDefinitions.cpp` | ~36 个内置工具的 executor,每个封装一个 `client_singleton.h` 设备命令 | 工具线程 |
| **会话 / 运行态** | `ChatSession` | 单会话历史(≤1000 条)、token 截断、**请求前配对清洗**(`getMessagesForRequest`)、JSON 持久化 | 主线程(自带锁) |
| | `SessionManager` | 多会话索引 `ai_sessions/index.json`、legacy 迁移 | 主线程(自带锁) |
| | `AgentRun` / `AgentTrace` | 单次运行的状态、轮次、trace 事件(可观测性) | 主线程 |
| **配置 / 持久化** | `ApiKeyStore` | provider 配置;**API Key 用 Windows DPAPI 加密**后存 `ai_config.json` | 自带锁 |
| | `AiSettings` | 非密设置(系统提示、代理、预算等)`ai_settings.json` | 自带锁 |
| | `DefaultSystemPrompt.h` | 首次运行写入的 AMem 专用系统提示 | — |
| **跨线程桥** | `UIMessageQueue` | 后台线程 → 主线程的**唯一**通道(`Token`/`Completion`/`Error`/`ToolResult`) | 双向 |
| **协议层(共享)** | `socket/client_singleton.h` | 设备命令的唯一真相源;GUI、Agent、MCP/IPC 三个前端共用 | 自带端口锁 |

---

## 3. 核心数据结构

均定义在 `gui/ai/AIProvider.h`、`ToolExecutor.h`、`AgentRun.h`、`AgentTrace.h`。

### 3.1 消息与工具调用(`AIProvider.h`)

```cpp
enum class Role { System, User, Assistant, Tool };

struct ToolCall {            // 模型请求调用一个工具
    std::string id;          // provider 给的调用 id(配对关键)
    std::string name;        // 工具名
    std::string arguments;   // JSON 字符串
};

struct ChatMessage {
    Role role;
    std::string content;
    std::vector<ToolCall> toolCalls;  // 仅 Assistant 可带
    std::string toolCallId;           // 仅 Tool 角色:对应哪个 ToolCall
    std::string name;                 // 工具名(Tool 角色)
    long long timestamp = 0;
    long long durationMs = 0;         // 仅 Assistant:本轮耗时
};

struct ToolDefinition { std::string name, description, parametersSchema /*JSON Schema 字符串*/; };
enum class ToolSafety { ReadOnly, Write };   // Write 需审批
```

### 3.2 请求 / 响应 / 错误(`AIProvider.h`)

```cpp
struct CompletionRequest {
    std::string runId;                 // 与 AgentRun.id 一致,用于过滤陈旧响应
    std::vector<ChatMessage> messages; // 来自 ChatSession::getMessagesForRequest()
    std::vector<ToolDefinition> tools; // 来自 ToolExecutor::getToolDefinitions()
    std::string model; bool stream;
    StreamCallback onToken; CompletionCallback onComplete;
};
struct CompletionResponse { ChatMessage message; ProviderError error; };

enum class ErrorCategory { None, Network, Authentication, RateLimit,
                           InvalidResponse, Cancelled, Timeout, Unknown };
struct ProviderError { ErrorCategory category; std::string message;
                       int httpStatusCode; std::string providerErrorCode;
                       explicit operator bool() const; };  // category!=None 即「有错」
```

### 3.3 工具执行结果(`ToolExecutor.h`)

```cpp
struct ToolRegistration { ToolDefinition definition; ToolSafety safety;
                          std::function<std::string(const std::string&)> executor; };
struct ToolResult { bool success; std::string resultJson; std::string errorMessage; };
```

### 3.4 运行态与 trace(`AgentRun.h` / `AgentTrace.h`)

```cpp
enum class AgentRunState { Idle, WaitingModel, ExecutingTools,
                           WaitingApproval, Completed, Failed, Cancelled };
enum class AgentApprovalDecision { Pending, Approved, Denied };

struct AgentRun { std::string id; AgentRunState state;
                  int modelTurns; int toolSteps;
                  std::vector<AgentTraceEvent> trace;
                  std::optional<ToolCall> pendingApproval;
                  AgentApprovalDecision approvalDecision; };

enum class AgentTraceType { Started, ModelRequestDispatched, CallLimitTruncated,
    StepLimitReached, AwaitingApproval, Approved, Denied, AutoApproved,
    ToolStarted, ToolSucceeded, ToolFailed, ToolSkipped, ToolBatchComplete,
    FollowUpRequested, Completed, Cancelled, ProviderError };
```

---

## 4. 线程模型

只有**三类线程**会触碰本子系统,边界必须严格遵守:

| 线程 | 谁 | 能做 | 不能做 |
|------|----|------|--------|
| **ImGui 主线程** | `ChatWindow`、`AgentController`、`AgentRunner`、`pollMessages()` | 所有 ImGui 调用、读写 run 状态、写 `ChatSession` | 阻塞(网络/磁盘长任务) |
| **HTTP worker** | `HttpClient::postAsync` detach 出的线程;provider 的 `onToken`/`onComplete` 在此触发 | 网络 I/O、解析、`UIMessageQueue::push` | **绝不**碰 ImGui / run 状态 |
| **工具执行线程** | `ToolExecutor::execute` 内 `runExecutorAsync` detach 出的线程 | 调 `client_singleton` 设备命令 | 碰 ImGui |

**唯一跨线程通道 = `UIMessageQueue`**(`gui/ai/UIMessageQueue.h`)。后台线程 `push(UIMessage)`,主线程在 `ChatWindow::pollMessages()` 里 `tryPop` 消费。消息种类:

- `Token` — 流式增量文本(追加到 `streamingContent_`)
- `Completion` — 模型一轮完成(可能带 `toolCalls`,也可能带 `error`)
- `Error` — 错误(部分 provider 取消时走 `Error`,Claude 取消也走 `Error`)
- `ToolResult` — 工具执行完成

**取消**:`CancellationToken = shared_ptr<atomic<bool>>`。`ChatWindow::cancelToken_` 在取消 / 新请求 / 删除会话时 `store(true)`;`HttpClient` 的 `content_receiver` 每个数据块检查它并中止连接。

**退出排空**:`HttpClient::shutdown()`(`main.cpp` 拆除阶段调用 + 析构兜底)置 `shuttingDown_`、取消所有在途请求、有界等待 detached worker 收尾,避免 worker 在单例静态析构后触碰已释放对象。

---

## 5. Agent 运行状态机

两套相关的状态:UI 侧 `ChatWindow::State`(粗粒度)与编排侧 `AgentRunState`(细粒度,见 `AgentController`)。

```
ChatWindow::State:  Idle ─send─▶ WaitingResponse ──(模型返回 tool_call)──▶ ToolExecuting
   ▲                                   │                                      │
   │                                   │(纯文本完成)                          ▼
   └───────────────────────────────────┴──────────────────────────  ToolConfirmation
                                                                    (写类工具待审批)
```

`model→tool→model` 循环由 `AgentRunner` 推进,关键节点(`AgentRunner.cpp`):

```
beginToolCalls(calls)            ── 进入一批工具调用(stepCount++,超 maxAgentSteps 则 Stopped)
   └─ runUntilBlocked()
        ├─ 遇写类工具且未自动批准 ─▶ NeedsConfirmation (UI 弹审批)
        │       ├─ 用户批准 ─▶ resumeApproved ─▶ NeedsExecution
        │       └─ 用户拒绝 ─▶ resumeDenied   ─▶ 跳过本批剩余 ─▶ ReadyForFollowUp
        └─ 读类 / 已批准 ─▶ NeedsExecution
              └─ ChatWindow.startToolExecution() 在工具线程跑 ToolExecutor.execute()
                    └─ UIMessageQueue(ToolResult) ─▶ completeToolExecution()
                          ├─ 成功且还有下一个工具 ─▶ runUntilBlocked()(继续本批)
                          ├─ 失败 ─▶ 跳过本批剩余 ─▶ ReadyForFollowUp
                          └─ 本批完成 ─▶ ReadyForFollowUp ─▶ sendFollowUpAfterTools()
                                                              (把工具结果回喂模型,新一轮)
```

**预算 / 安全阀**(`AgentRunner::Config`,默认值来自 `AiSettings`):

- `maxAgentSteps`(默认 12,clamp [1,64]):一次用户回合内 model↔tool 往返上限,防失控循环;
- `maxToolCallsPerTurn`(默认 16,clamp [1,64]):单条 assistant 消息里执行的工具数上限,超出截断;
- `autoApproveWrites`(默认 false):YOLO 模式,写类工具免审批。

每一步产出一个 `AgentTraceEvent`,经 `AgentController` 累积进 `AgentRun.trace`(上限 128 条,超出裁剪),用于 UI 的「执行轨迹」面板与日志。

---

## 6. 工具系统

### 6.1 注册与执行(`ToolExecutor`)

- `registerTool(name, desc, schema, safety, executor)`:name≤64、desc≤256(超长截断);
- `execute(call)`:① 查表(未知工具返回可用工具清单)→ ② `json::parse` 参数(失败返回结构化错误)→ ③ **JSON Schema 校验**(`validateAgainstSchema`,支持 type/required/properties/items/anyOf/min-max/length 等子集)→ ④ `runExecutorAsync` 在 detached 线程执行并 `wait_for(timeout)`。
- **读 / 写超时语义不同**:只读工具超时即返回「timed out」错误;**写 / 有状态工具会等待真实结果返回**,避免「超时但可能已写入」的歧义目标态。
- 任何 executor 抛出的异常都被捕获并转成 `success=false` 的 `ToolResult`,绝不逃逸。

### 6.2 工具安全分级

每个工具静态分类为 `ReadOnly` 或 `Write`。**写 / 有状态工具在未开 `autoApproveWrites` 时必须经用户审批**(`AgentRunner` 的 `NeedsConfirmation`)。未注册工具默认按 `ReadOnly` 处理(反正会被 execute 以「未知工具」拒绝,不该触发审批弹窗)。

### 6.3 内置工具清单(`ToolDefinitions.cpp`,按域)

| 域 | 工具(R=ReadOnly / **W**=Write) |
|----|-------------------------------|
| 状态/驱动 | `get_status` R · `get_server_version` R · `get_architecture` R · `init_driver` **W** |
| 进程/模块 | `get_process_list`/`list_processes` R · `open_process` **W** · `get_module_list`/`list_modules` R · `get_module_base` R · `resolve_offset_chain` R |
| 内存读 | `memory_read` R · `read_memory` R · `read_value` R · `read_disassembly` R |
| 内存写 | `memory_write` **W** · `write_bytes` **W** · `write_value` **W** |
| 扫描 | `scan_set_range` **W** · `scan_value` **W** · `scan_next` **W** · `scan_fuzzy` **W** · `scan_hex` **W** · `get_scan_count` R · `get_scan_results` R · `clear_scan` **W** |
| 断点 | `set_breakpoint` **W** · `remove_breakpoint` **W** · `read_breakpoint_info` R · `suspend_breakpoint` **W** · `resume_breakpoint` **W** |
| 符号 | `resolve_symbol` R · `symbol_init` **W** · `symbol_list` **W** · `symbol_find` R |
| 脚本 | `execute_lua` **W** |

> 注:部分名字是别名对(如 `memory_read`/`read_memory`、`list_modules`/`get_module_list`、`list_processes`/`get_process_list`),指向相同实现,以兼容不同模型的命名习惯。每个 executor 都直接调用对应的 `client_singleton.h` 命令——**这就是 Agent 和 GUI / MCP 共享同一协议层的体现**。

---

## 7. 持久化与配置

所有文件相对工作目录(即 exe 同级):

| 文件 | 管理者 | 内容 | 备注 |
|------|--------|------|------|
| `ai_config.json` | `ApiKeyStore` | provider 配置 | **API Key 用 DPAPI 加密**(`CryptProtectData`,base64 over 密文),按 Windows 用户隔离,从不内置 |
| `ai_settings.json` | `AiSettings` | 系统提示、代理、`executionTimeout`、`maxAgentSteps`、`maxToolCallsPerTurn`、`tokenLimit`、`autoApproveWrites` | 明文、可手改;数值项均 clamp |
| `ai_sessions/<id>.json` | `ChatSession` | 单会话完整历史 | ≤1000 条,超 token 限额按对话组截断 |
| `ai_sessions/index.json` | `SessionManager` | 会话元数据列表 + `activeId` + `legacyMigrated` | |

**一次性迁移**(启动时各执行一次):`ai_config.dat`→`ai_config.json`;`ai_session.json`→`ai_sessions/`(由 `index.json` 里持久化的 `legacyMigrated` 标志守护,避免重复导入)。

**原子写**:三处持久化(`ApiKeyStore`/`SessionManager`/`AiSettings`)统一经 `utils/AtomicFileWrite.h` 的 `installTempFile`——写 `.tmp` → rename;失败则把原文件挪到 `.bak` 再装新文件,失败回滚,**任何路径都不会两个文件都没了**。

---

## 8. 与 MCP / IPC 的关系(三前端共享协议层)

```
GUI 窗口 ───────────────┐
应用内 Agent (ToolDefs) ─┼──▶ socket/client_singleton.h ──▶ 三端口 socket ──▶ Android 设备
MCP(Python)▶ IPC :28100 ┘        (设备协议唯一真相源)
```

- **应用内 Agent**:`ToolDefinitions.cpp` 的 executor **直接**调 `client_singleton` 命令(同进程)。
- **外部 AI 助手**:经 `mcp/`(Python,FastMCP)→ HTTP → `ipc/IpcServer.cpp`(`127.0.0.1:28100`)→ 同一批 `client_singleton` 命令。
- 因此新增一个设备能力,通常是:① 在 `socket/*Commands.cpp` 加命令并在 `client_singleton.h` 声明 → ② 按需在三前端暴露(GUI 面板 / `ToolDefinitions.cpp` 工具 / `IpcServer` 方法 + MCP 包装)。

> 同一逻辑能力因此存在「重复校验」:`ToolDefinitions.cpp`(Agent)与 `IpcServer.cpp`(MCP)各自校验同类参数(扫描 flag、地址、size 上限等),两者应保持一致。

---

## 9. 关键不变量(改代码前务必遵守)

1. **UI 线程隔离**:后台 / worker 线程只能经 `UIMessageQueue` 与主线程通信,绝不直接碰 ImGui 或 run 状态。
2. **socket 串行化**:任何设备命令必经 `SocketCommand::execute*` 模板(端口互斥锁 + DrainPending),否则并发响应会串包。
3. **配对清洗**:发给 provider 的消息一律经 `ChatSession::getMessagesForRequest()`——它保证 `tool_use`/`tool_result` 严格配对(丢弃孤儿、空/重复 id、不完整组),provider 才能安全地按位置拼装。
4. **审批门**:状态变更类工具必须分类为 `ToolSafety::Write`,否则会绕过用户确认。
5. **runId 过滤**:`pollMessages` 只处理 `msg.runId == 当前 run id` 的消息,陈旧响应被丢弃。
6. **MCP/IPC 同步**:改扫描 flag / 数据类型 / 内存区枚举时,`mcp/amem_mcp/constants.py` 与 C++ 枚举须同步。
