# Agent 项目待修复问题记录

记录时间：2026-05-29

本文档记录除搜索/扫描协议外，当前 AMem 内置 AI Chat、MCP/IPC、通用 socket 命令层中已经确认的待修复问题。搜索/扫描协议问题单独记录在 `docs/scan_protocol_issues.md`。

## 1. AI 工具返回 `{"error": ...}` 时仍被标记为成功

状态：已修复。`ToolExecutor` 会解析工具返回 JSON，遇到顶层非空 `error` 字段时返回 `success=false`；`AgentRunner` 会在失败审计里保留原始错误详情。

涉及代码：

- `gui/ai/ToolDefinitions.cpp`
- `gui/ai/ToolExecutor.cpp`
- `gui/ai/AgentRunner.cpp`

现状：

- 内置工具失败时通常返回 `makeError()`，也就是 JSON 字符串：`{"error":"..."}`。
- `ToolExecutor::execute()` 当前只要 executor 正常返回字符串，就设置 `ToolResult.success = true`。
- `AgentRunner::makeToolMessage()` 因此会生成 `success: true`，并把 `{"error": ...}` 放进 `result` 字段。

影响：

- trace 会显示 `ToolSucceeded`，但实际工具结果包含错误。
- tool role 审计消息中 `success` 与 `result.error` 矛盾。
- 后续模型可能把 socket 失败、参数错误、未连接设备等当成成功工具调用继续推理。
- 默认 system prompt 已提醒模型看 `success=false` 或 `error`，但当前执行器不会把这类工具错误转成 `success=false`。

建议：

- `ToolExecutor::execute()` 在 executor 返回后尝试解析 `resultJson`。
- 如果顶层 JSON object 包含非空 `"error"` 字段，应设置 `ToolResult.success = false`，`errorMessage` 使用该字段。
- 保留原始 `resultJson` 可选，用于调试，但 tool audit 应统一进入 error 分支。
- 后续可把工具返回结构统一成 `{ "ok": true, ... }` / `{ "ok": false, "error": ... }`，但短期先兼容现有 `makeError()`。

## 2. 工具超时后底层任务仍会继续占用 socket

涉及代码：

- `gui/ai/ToolExecutor.cpp`
- `socket/socket_request_manager.h`
- `socket/client.hpp`

现状：

- `ToolExecutor` 用 `std::async` 和 `wait_for()` 实现工具超时。
- 超时后返回错误，但底层 executor 线程不会被取消。
- 如果 executor 卡在 socket `Receive()`，该线程仍持有对应端口请求锁，后续工具调用可能继续排队或阻塞。

影响：

- UI/agent 看起来已经收到“工具超时”，但 socket 通道实际仍被旧请求占用。
- 下一步 agent 工具调用可能继续失败或卡住，表现为状态恢复不彻底。
- 这与扫描协议卡住风险叠加后，会让 agent loop 更难恢复。

建议：

- 短期：为 socket receive 路径增加局部超时，优先覆盖扫描和大响应读取。
- 中期：工具执行上下文传入 cancellation token，socket 命令可检查取消状态。
- 长期：把工具任务从不可控 `std::async` 改成可管理的 worker/任务队列，并能在超时后关闭或重建对应连接。

## 3. Claude Provider 错误走 `UIMessageType::Error` 会丢失结构化信息

状态：已修复。`ChatWindow::pollMessages()` 会在 `UIMessageType::Error` 中优先识别 `msg.response.error`，并复用 `displayErrorForCategory()` 的统一错误处理；如果结构化错误附带了未进入流式缓冲区的 partial content，也会先补入 `streamingContent_` 以保留上下文。

涉及代码：

- `gui/ai/ClaudeProvider.cpp`
- `gui/ai/ChatWindow.cpp`
- `gui/ai/UIMessageQueue.h`

现状：

- `ChatWindow::pollMessages()` 对 `UIMessageType::Error` 只读取 `msg.data`，不会使用 `msg.response`。
- ClaudeProvider 在 API key 缺失、取消、HTTP 错误、stream error 等路径推送 `UIMessageType::Error`。
- 这些错误消息虽然有时填充了 `msg.response`，但 UI 层不会调用 `displayErrorForCategory()`。

影响：

- 错误类别丢失，认证失败不能稳定打开设置面板。
- 部分流式内容无法按 `displayErrorForCategory()` 的逻辑保留下来。
- OpenAI/DeepSeek 与 Claude 的错误路径不一致，agent trace 和恢复行为不统一。

建议：

- Provider 层优先统一通过 `UIMessageType::Completion` 携带 `CompletionResponse.error`。
- 或者 `ChatWindow::pollMessages()` 在 `UIMessageType::Error` 分支检测 `msg.response.error`，并转入 `displayErrorForCategory()`。
- 推荐后者作为短期修复，改动小且兼容现有 provider。

## 4. 可变长度 socket 响应缺少统一上限校验

状态：已修复当前文档列出的前端读取点。`FetchProcessList()`、`FetchModuleList()`、`SymbolGetList()`、`FreezeGetList()`、`ReadKernelBreakpointInfo()`、`GetScanResult()`、`GetTypedScanResult()` 已增加 count/name size 上限和负数校验；进程列表中途读取失败现在会返回失败。

涉及代码：

- `socket/ProcessCommands.cpp`
- `socket/SymbolCommands.cpp`
- `socket/FreezeCommands.cpp`
- `socket/BreakpointCommands.cpp`
- `socket/ScanCommands.cpp`

已确认风险点：

- `FetchProcessList()` 没校验 `len`、进程名长度 `proc.size`。
- `FetchModuleList()` 只校验 `len < 0` 和 `modulenamesize < 0`，没有最大值限制。
- `SymbolGetList()` 没校验 `output.actualCount`、`entry.nameSize` 的负数和上限。
- `FreezeGetList()` 没校验 `output.count < 0` 或超大 count。
- `ReadKernelBreakpointInfo()` 没校验 `result < 0` 或超大 result。
- `GetScanResult()` / `GetTypedScanResult()` 没校验 `actual_count` 是否超过请求 count 或合理上限。

影响：

- 后端异常、协议错位或连接读到脏数据时，前端可能尝试分配超大 vector/string。
- 负数转换为 `size_t` 后可能造成异常或崩溃。
- 错误路径难以定位，可能表现为 UI 卡死、内存暴涨或 socket 读乱。

建议：

- 在 socket 命令层增加本地常量上限，例如：
  - process count 最大 65536
  - module count 最大 65536
  - symbol page 最大请求 count，且 nameSize 最大 64KB
  - freeze item count 最大 100000
  - breakpoint hit count 最大 100000
  - scan result actual_count 不得大于请求 count，且 count 最大 1000 或调用方上限
- 所有可变长度读取先校验，再分配，再接收。
- 对读失败必须返回 false，不要 `break` 后返回 true。

## 5. `FetchProcessList()` 部分读取失败仍返回成功

状态：已修复。读取进程条目或进程名失败会返回 false，不再返回半截列表。

涉及代码：

- `socket/ProcessCommands.cpp`

现状：

- 循环读取进程条目时，如果读取 `proc` 或 `name` 失败，会 `break`。
- lambda 随后仍返回 `true`。

影响：

- 调用方会得到一个不完整列表，但认为请求成功。
- 如果协议已经错位，后续请求可能继续在同一连接上读取残留数据。

建议：

- 改为任何条目读取失败都返回 false。
- 同时增加 `len` 和 `proc.size` 上限校验。

## 6. `InitDriver()` 对返回字符串长度缺少上限

状态：已修复。`InitDriver()` 会拒绝空 card、超长 card 和负数/超大返回字符串长度；空返回消息不再触发大分配。

涉及代码：

- `socket/ProcessCommands.cpp`

现状：

- `InitDriver()` 读取 `resStrlen` 后直接 resize。
- 只判断 `resStrlen == 0`，没有判断负数或超大值。
- 发送卡密长度时用 `int Cardlen = Card.size()`，没有限制输入长度。

影响：

- 后端异常返回可能导致超大分配。
- 空字符串被视为通信失败，无法表达“成功但无消息”的合法状态。

建议：

- 限制 card 长度和返回消息长度。
- `resStrlen < 0` 或超过上限直接失败。
- 是否允许空消息需要按后端语义确认。

## 7. `SymbolGetList()` 对负数 count/nameSize 风险较高

状态：已修复。`offset/count`、服务端 `totalCount/actualCount`、符号名 `nameSize` 均已做非负和上限校验。

涉及代码：

- `socket/SymbolCommands.cpp`
- `gui/AppContext.cpp`
- `gui/ai/ToolDefinitions.cpp`
- `ipc/IpcServer.cpp`

现状：

- `outSymbols.reserve(output.actualCount)` 直接使用服务端返回值。
- `entry.nameSize > 0` 时直接 `name.resize(entry.nameSize)`。
- AppContext、AI 工具、IPC 都会调用该接口。

影响：

- 符号表功能一旦收到异常长度，影响面较广。
- agent 的 `symbol_list` 可能被一次异常响应拖垮。

建议：

- `actualCount` 必须在 `[0, requested_count]`。
- `nameSize` 必须在 `[0, 64KB]` 或更保守上限。
- 对 `totalCount` 也做非负校验，避免 UI 展示负数。

## 8. IPC HTTP 服务端请求解析较脆弱

状态：部分修复。`BuildHttpResponse()` 已按状态码输出 reason phrase，不再出现 `400 OK`；`open_process` 已改为通过 `AppContext::selectProcess()` 单次打开并读取最终 handle，失败时不会留下旧 handle。HTTP 解析严格化仍暂缓。

涉及代码：

- `ipc/IpcServer.cpp`

现状：

- `Content-Length` 解析用字符串查找和 `atoi`。
- 未校验 HTTP method/path。
- header/body 超过 1MB 后只是跳出循环，后续仍可能尝试解析不完整 body。
- `BuildHttpResponse()` 状态行始终输出 `"OK"`，例如 400 也会是 `HTTP/1.1 400 OK`。

影响：

- MCP 客户端通常能正常使用，但异常 HTTP 请求的行为不清晰。
- 调试时状态码文本误导。
- 这不是当前最高优先级，因为 IPC 仅监听 `127.0.0.1`，但 agent 化后建议提高鲁棒性。

建议：

- 后续集中修 IPC 时，改成严格解析 Content-Length、限制 body、校验 POST。
- 状态行按状态码输出 `Bad Request` / `Internal Server Error`。
- 该项可暂缓，不要和 socket 协议修复混在同一提交。

## 9. MCP 工具层与内置 AI 工具层错误表达不一致

涉及代码：

- `mcp/amem_mcp/ipc_client.py`
- `mcp/amem_mcp/tools/*.py`
- `gui/ai/ToolDefinitions.cpp`

现状：

- MCP 失败通过 Python exception 转成工具错误。
- 内置 AI 工具失败通过 JSON `{"error": ...}` 返回。
- 两套工具命名和参数已做部分兼容，但错误结构、输出格式、大小限制不完全一致。

影响：

- 同一能力在内置 AI Chat 与外部 MCP agent 中表现不同。
- 未来做统一 agent 项目时，工具结果审计和恢复策略难复用。

建议：

- 先修 `ToolExecutor` 识别 `{"error": ...}`。
- 再定义统一工具结果约定，至少包括 `success/error/result/duration_ms`。
- 后续可让 MCP 工具尽量返回结构化 JSON，再由 MCP 层渲染文本摘要。

## 10. `OpenProcessHandle()` 没有拒绝 handle=0

状态：已修复。`OpenProcessHandle()` 会先清空输出 handle，收到 `handle == 0` 时返回失败并清空 `AppContext::processHandle`；IPC `open_process` 也改为读取 `AppContext` 最终状态，不再重复打开或残留失败 pid/handle。

涉及代码：

- `socket/client_singleton.cpp`
- `ipc/IpcServer.cpp`
- `gui/ai/ToolDefinitions.cpp`

现状：

- `OpenProcessHandle()` 只要成功收到 int，就写入 `AppContext::processHandle` 并返回 true。
- 内置 AI 的 `open_process` 额外检查了 handle=0。
- IPC 的 `open_process` 没有额外检查，可能返回 `success: true, handle: 0`。

影响：

- MCP agent 可能认为进程已打开，但后续内存操作因为 handle 无效失败。
- UI 状态和工具状态可能不一致。

建议：

- 在 `OpenProcessHandle()` 底层统一要求 handle 非 0。
- 或至少 IPC `open_process` 增加 handle=0 检查。

## 建议修复顺序

1. 修 `ToolExecutor` 对 `{"error": ...}` 的失败识别，这是 agent 行为正确性的基础。
2. 修 Claude/ChatWindow 错误路由，让 provider 错误统一走结构化处理。
3. 分批加 socket 可变长度响应上限：先 `FetchProcessList()`、`FetchModuleList()`、`SymbolGetList()`。
4. 再修 `FreezeGetList()`、`ReadKernelBreakpointInfo()`、扫描结果读取的 count 校验。
5. 修 `OpenProcessHandle()` 或 IPC `open_process` 对 handle=0 的处理。
6. IPC HTTP 服务端解析和 MCP 输出统一最后单独做，避免和 socket/agent 核心修复混杂。

## 暂缓项

- IPC Server 架构重写暂缓。
- MCP 工具输出全面结构化暂缓。
- ToolExecutor 可取消任务队列暂缓，先通过 socket 超时降低卡死概率。
- Provider API 适配升级暂缓，除非当前 provider 错误路由影响 agent loop。
