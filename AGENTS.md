# AGENTS.md

This file is the repository-level guide for coding agents working on AMem. The current AI-agent architecture, runtime walkthrough, and audit findings live in:

- `docs/agent_architecture.md`
- `docs/agent_walkthrough.md`
- `docs/agent_project_issues.md`
- `docs/native_agent_refactor_plan.md`

Read the relevant documents before changing `gui/ai/`, `ipc/`, `tools/protocol_reference/`, shared process state, or socket commands.

## Project Overview

AMem is a Windows desktop application for remote Android memory debugging, similar to Cheat Engine. It connects to an Android server over sockets and provides memory scanning, memory read/write, hardware breakpoints, a hex viewer, value freezing, ELF symbols, and Lua scripting. Dear ImGui (docking branch) is rendered through DirectX 12.

The supported AI integration is the in-app AI Chat agent in `gui/ai/`. It supports Claude, OpenAI-compatible, and DeepSeek providers and calls native debugger tools. The Python MCP proxy and its IDE configurations have been removed from `NativeAgent`. Default builds exclude the old loopback HTTP IPC server; `ENABLE_LEGACY_HTTP_IPC=ON` restores it only for migration diagnostics. Native framing, secured transport, strict Hello/request sessions, a complete method catalog, 12 Observe `MemService` adapters, an owned runtime, and explicit status/control now exist under `ipc/` and `gui/`. `ENABLE_NATIVE_IPC` still defaults OFF; even when compiled, the pipe starts disabled and requires a user action. There is no privileged approval or supported privileged external Agent adapter yet.

Language: C++17 for the app. Platform: Windows 10/11 x64. The optional protocol probe in `tools/protocol_reference/` uses Python's standard library but is not a build or runtime dependency.

## Build and Validation

```bash
# Configure from the repository root with Clang + Ninja
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=TRUE \
  -DCMAKE_C_COMPILER="C:/Program Files/LLVM/bin/clang.exe" \
  -DCMAKE_CXX_COMPILER="C:/Program Files/LLVM/bin/clang++.exe" \
  --no-warn-unused-cli -S . -B build -G Ninja

# Build
cmake --build build

# Run native no-device tests
ctest --test-dir build --output-on-failure
```

Output: `bin/ImGuiProject.exe`.

The project can also be opened directly through `CMakeLists.txt` in Visual Studio 2022 using an x64 Release/Debug configuration.

`native_agent_mem_service` is the main no-device C++ test with 23 groups. Native IPC has 5 protocol, 5 transport, 7 framed-I/O, 8 handshake, 6 request-contract, 9 request-session, 4 method-catalog, 5 `MemService` dispatcher, and 7 owned-runtime groups. They cover framing/transport, Hello, strict JSON, lifetime request ids, capability/target policies, Observe dispatch, error normalization, connection/target invalidation, sequential clients, restart, deadline/client/Stop cancellation, and joined shutdown. Fourteen CTests include those nine native IPC binaries plus the service suite and four static gates; the native gate also fixes compile default-off, user-only start, visible Observe-only status, and shutdown-before-disconnect ordering. Provider, privileged approval, live GUI interaction, opt-in legacy IPC, real Android transport, and device operations still lack complete automation. For manual wire-protocol checks use `tools/protocol_reference/amem_client.py`; for the live Lua API use `scripts/dump_api.lua` as described in `scripts/README.md`. Changes involving real device state, concurrency, cancellation, or teardown still need manual end-to-end verification with the GUI and an Android device.

## Dependencies and Feature Gates

- Required: Visual Studio 2022, CMake 3.16+, Windows SDK, DirectX 12 SDK.
- Required: LuaJIT under `third_party/LuaJIT/` with `include/` and `lib/lua51.lib`.
- AI Chat (`ENABLE_AI_CHAT`, default ON): vendored cpp-httplib/nlohmann JSON plus static OpenSSL. If OpenSSL is absent, AI Chat is disabled while the rest of the app can still build.
- Optional: Capstone for disassembly and Keystone for assembly. Prefer static x64 vcpkg triplets.
- The executable uses the static `/MT` CRT. Do not introduce `/MD` libraries into the final link.

CMake options:

- `USE_DX12` (default ON)
- `USE_DX11` (default OFF)
- `ENABLE_AI_CHAT` (default ON)
- `ENABLE_LEGACY_HTTP_IPC` (default OFF; unsafe migration-only endpoint)
- `ENABLE_NATIVE_IPC` (default OFF; compile-only transport foundation)
- `LUAJIT_STATIC` (default ON)

Compile-time gates:

- `HAVE_AI_CHAT`
- `HAVE_LEGACY_HTTP_IPC`
- `HAVE_NATIVE_IPC`
- `HAVE_CAPSTONE`
- `HAVE_KEYSTONE`
- `HAVE_LUAJIT`

Source that depends on an optional feature must remain correctly guarded.

## Unifying Command Pipeline

The free functions declared in `socket/client_singleton.h` are the shared device-protocol surface. The current front ends all terminate there:

```text
GUI windows --------------------+
In-app AI ToolDefinitions ------+--> MemService -> socket/client_singleton.h
Opt-in legacy HTTP IPC ---------+                  -> WinSocketClientMgr -> Android
```

When adding a device capability:

1. Implement the protocol command in the appropriate `socket/*Commands.cpp`.
2. Declare it in `socket/client_singleton.h`.
3. Surface it only where required: a GUI panel or `ToolDefinitions.cpp`. Do not expand the legacy HTTP IPC while its replacement decision is pending.
4. Keep validation, error semantics, flags, and output limits aligned across all exposed front ends.

Do not implement a second version of the wire protocol in AI or IPC code.

## Main Application Architecture

### Entry Point and Rendering

- `main.cpp` creates the Win32 window, drives `Gui::mainLoop()`, and runs shutdown. It starts port 28100 only in an explicit `ENABLE_LEGACY_HTTP_IPC=ON` build.
- `renderer/DX12Renderer` owns DirectX 12 device/swapchain/frame resources.
- `renderer/StyleSetup` owns ImGui style and font setup.
- `ExceptionHandler.h` installs crash dump handling.

### GUI

- `Window` is the base window abstraction.
- `Gui` owns active windows and logging.
- `CEWindow` is the main hub.
- Larger features are split into panel directories:
  - `gui/scan/`
  - `gui/memview/`
  - `gui/breakpoint/`
- Use `ColorScheme.h` instead of hardcoded UI colors.

### Shared Process State

`AppContext::Get()` owns global target state:

- `selectedPid`
- `processHandle`
- `processRevision`
- selected process name
- module and symbol caches

GUI, in-app Agent, and IPC all share this object. Each in-app Agent run captures the connection generation and a target snapshot; approval, execution dequeue, and result collection validate it. Migrated `MemService` operations also consume the snapshot at their actual service/send boundary. Legacy process-bound executors still need that final migration; `runId` alone is never a target identifier.

`EventBus` and events in `gui/Events.h` decouple GUI windows. Do not use the event bus as a substitute for target revision validation.

## Socket Communication

- `client.hpp` wraps Winsock send/receive.
- `WinSocketClientMgr` owns `PORT_MAIN`, `PORT_DEBUG`, and `PORT_ERROR`, each with a request-response mutex and a recursive high-level transaction gate.
- Commands are split across `ProcessCommands.cpp`, `MemoryCommands.cpp`, `ScanCommands.cpp`, `BreakpointCommands.cpp`, `FreezeCommands.cpp`, and `SymbolCommands.cpp`.
- Prefer `SocketCommand::execute`, `executeNoHandle`, or `executeWithResult`. These handle the shared connection lease, generation checks, process handle setup, and per-port locking.
- `SocketIoTimeout::ScopedTimeout` applies a thread-local I/O budget.

Critical distinction: a port mutex makes one request-response pair serial. It does not make a multi-command business operation transactional. Existing compound sequences include:

- `ScanSetRange` followed by a scan command
- `SymbolInit` followed by `SymbolGetList`
- the cleanup/open/set-PID sequence in `AppContext::selectProcess()`

If a new operation depends on multiple commands sharing global server state, use a higher-level transaction/revision/epoch or add a server-side compound command.

`SocketCommand::TransactionLease` holds the connection request lease and one per-port transaction gate. Normal commands hold it briefly; pointer-chain resolution keeps it across module lookup and every pointer read. Do not call target APIs that take `AppContext::processStateMutex_` while holding this gate; use the stable revision check before the transaction and the full snapshot check after release.

Every scan mutation advances `WinSocketClientMgr`'s monotonic scan epoch, including legacy IPC commands. Canonical `scan_start` holds one transaction across range selection and scan execution; `scan_refine`, `scan_results`, and `scan_clear` require the latest returned epoch. `ScanWindow` is injected with `IMemService` and uses the same target-bound session for start/refine/results/clear/removal. Its Stop button only sets the shared cancellation token; the system backend issues the DEBUG stop from the progress callback and the GUI must preserve confirmed-after-cancel versus `completion_unknown`. An epoch mismatch is a session conflict, not a retryable transport error.

Every symbol-table initialization advances a monotonic symbol epoch while holding the port transaction gate. Canonical `symbol_resolve` and `symbol_list` resolve the module, initialize its symbol table, and find/fetch inside one MAIN transaction. Symbol continuation pages require the previous result's epoch; legacy IPC initialization invalidates it. GUI address formatting uses `IMemService::loadSymbolTable()` to initialize once and fetch the complete bounded table under one transaction before atomically installing a target-validated cache entry.

Canonical breakpoint mutations return tracked completion receipts. A sent request without a response is non-retryable `completion_unknown`; confirmed completion after cancellation/deadline remains explicit. Remote confirmation and the local cleanup tracker update occur under the same MAIN transaction, cleanup holds that gate across snapshot/removal, and disconnect clears the local tracker without sending to a stale target. Breakpoint hit responses have no wire cursor: the service drains one response and retains its newest bounded tail. The Agent limit is 100 and reports `available`/`dropped`; do not invent continuation offsets.

Validate every untrusted count, string length, and byte size before allocating or receiving variable-length data.

`DeviceSession` gives commands a shared request lease and connect/disconnect/reconnect an exclusive lifecycle lease. `WSAETIMEDOUT`, EOF, and partial I/O poison the connection, advance its generation, close the failed client, and reject reuse until explicit reconnect. The old pending-data drain recovery path has been removed. Do not add direct `Connect()`/`Close()` calls or bypass the lifecycle gate; process handles and target snapshots remain generation-bound.

## In-App AI Chat (`gui/ai/`)

The subsystem is under the `AI` namespace and gated by `HAVE_AI_CHAT`.

### Control Flow

```text
ChatWindow
  -> AgentController
  -> provider / HttpClient
  -> UIMessageQueue
  -> AgentRunner
  -> AgentTaskExecutor
  -> ToolExecutor
  -> ToolDefinitions
  -> socket commands
```

- `ChatWindow` owns UI state, sessions, approvals, active run ids, and queue consumption.
- `AgentController` owns provider dispatch and run state/trace.
- `AgentRunner` owns the model -> tool -> model state machine, budgets, and approval gating. It must remain free of ImGui calls.
- `AgentTaskExecutor` owns a bounded serial queue and one joinable worker. It fixes the absolute deadline at enqueue, propagates cancellation, calls `ToolExecutor` synchronously, and joins during shutdown.
- `ToolExecutor` owns the thread-safe registry, schema validation, safety metadata, synchronous executor call, and result normalization.
- With LuaJIT, `ToolDefinitions.cpp` has 24 executable and advertised canonical names with no hidden built-in aliases. Without `HAVE_LUAJIT`, `lua_execute` is not registered, leaving 23 executable and advertised names.
- The native slice (`mem/`, `MemJsonTools`, and the AI `AgentMemTools` alias) owns status/driver/process/open, module/pointer/disassembly/symbol resolution, scan/symbol sessions, breakpoint mutations/hits, raw and typed memory validation, scalar encoding, and structured results. AI and native IPC share this adapter; do not duplicate its parsers or result formatting.
- `ChatSession::getMessagesForRequest()` is the required provider boundary; it cleans and pairs tool calls/results. Any group containing one of the 33 retired names is converted to inert assistant text with redacted arguments and recorded results, never sent as provider tool protocol or re-executed.

### Tool Safety

Every in-app tool is `ToolSafety::ReadOnly` or `ToolSafety::Write`. Write tools require confirmation unless `autoApproveWrites` is enabled.

Under the current binary safety model, `ToolSafety::Write` means target/host mutation requiring approval. Canonical `symbol_resolve` and `symbol_list` remain ReadOnly: their internal active-table mutation is serialized, epoch-bound, and does not modify target memory. Retired names such as `symbol_init`, `symbol_find`, and `resolve_symbol` are no longer registered; old session records are downgraded to inert assistant text at the provider boundary. `DefaultSystemPrompt.h` lists only canonical tools. A future richer effect model should represent symbol work as session mutation without restoring model-visible initialization steps.

Unknown tools cannot execute and are rejected by `ToolExecutor`.

### Threading and Cancellation

Provider and tool threads must never access ImGui or run state directly. In-app background results return through `UIMessageQueue`.

Current runtime ownership is imperfect:

- `HttpClient::postAsync()` creates detached HTTP workers.
- In-app tools run only on the joinable `AgentTaskExecutor` worker; do not reintroduce outer or inner detached tool threads.
- Stop cancels model orchestration and signals queued/active tool contexts. It cannot retract a sent write. Late results remain excluded from the stale chat/session by the run-id filter, but mutation and symbol-session outcomes are persisted to `ai_mutation_audit.jsonl` before the UI callback and remain visible in the Audit table.
- `AgentTaskExecutor::shutdown()` cancels queued/active work and joins the worker before device disconnect. `HttpClient::shutdown()` remains a bounded best-effort wait, not proof that every HTTP worker exited.
- Claude/DeepSeek record stream terminal state but do not validate it; OpenAI does not record it. HTTP 2xx with a truncated stream can currently be committed as success.

Do not add new detached threads. Extend the owned task model and keep completion callbacks in lifecycle accounting. See `docs/agent_project_issues.md` before touching cancellation or teardown.

### Target Binding

`AgentRunContext` captures the connection generation and `{pid, handle, processRevision}`. The approval dialog shows expected generation, PID, and revision; approval, dequeue, and result collection revalidate them. `process_open` uses `Selection` policy and explicitly advances the run context only when its returned snapshot is still current.

All currently registered in-app operations now use the target-bound native boundary: driver initialization, module/pointer/disassembly/symbol resolution, canonical scan/symbol/breakpoint operations, and raw/typed memory read/write use `MemService`; `lua_execute` uses its host validation boundary. `driver_initialize` is connection-bound and distinguishes unsent, rejected, completion-unknown, and confirmed-after-cancel/deadline outcomes. `pointer_resolve` holds one read transaction across module lookup and every dereference. Canonical scan tools bind `{target, scanEpoch}` and report confirmed completion after cancellation/deadline; an unconfirmed sent scan is `completion_unknown`. Canonical symbol tools bind module lookup, initialization, and list/find to one transaction and return a module-bound epoch. Canonical breakpoint mutations bind the target at send and preserve confirmed/unknown completion; hit batches are target-bound. `lua_execute` revalidates the target at its host-execution boundary and never extends the task's absolute deadline, but cannot retract a script after it starts. New process-bound operations must consume the explicit `OperationContext` again at their actual service/socket send boundary. The approval dialog still lacks the process name.

`BreakpointWindow` receives `IMemService` explicitly and routes set/remove/suspend/resume plus hit-history refresh through the target-bound service contract. `BreakpointHit` preserves GPR, `orig_x0`, syscall number, FPSR/FPCR, and all 32 128-bit vector registers. GUI refresh drains one DEBUG-port response while retaining at most the newest 50,000 entries; the Agent uses the same batch contract with a 100-entry cap.

`MemoryViewerWindow` also receives `IMemService`. `AppContext::ModuleCache` remains the GUI presentation cache, but symbol loading no longer sends `SymbolInit`/`SymbolGetList` itself. Complete tables are limited to 1,000,000 entries and 64 MiB of names; cache installation revalidates the service result's target and current module while holding the cache mutex.

### Limits

There are local limits for tool call count, names, ids, arguments, and many socket responses. There is not yet a complete limit for:

- raw HTTP/SSE accumulation
- assistant content
- tool result JSON
- total session file size
- per-message/session-load allocation

Add limits at the earliest untrusted boundary and paginate large lists.

Provider `maxContextTokens` declarations are currently unused. The local byte-count heuristic also excludes tool schemas and output-token reserve. Do not treat `tokenLimit` as proof that a request fits the active model.

## Persistence and Provider Trust

Files are relative to the process working directory:

- `ai_config.json`: provider configuration; API keys are DPAPI-encrypted.
- `ai_settings.json`: plain global settings.
- `ai_sessions/index.json`: session metadata.
- `ai_sessions/<id>.json`: plain chat/tool history.
- `ai_mutation_audit.jsonl`: plaintext mutation/session-effect summaries, capped at 4 MiB with one `.1` backup and 64 KiB per record.

Do not claim that all AI data is encrypted. `driver_initialize`/`init_driver` card fields are redacted from approval display, tool audit, session JSON, and mutation audit while the original remains transiently available for execution/provider continuity. The mutation audit also omits Lua code, raw memory data, bulk output, registers, hits, and large fields, but it is not encrypted. Session files can still contain Lua code, addresses, memory bytes, registers, other tool arguments, and tool results, and that history can be sent to the active remote provider in later turns.

Loaders must parse into temporary state and preserve the original on failure. Current loaders do not all meet that rule, and `ChatSession` does not use the same atomic install helper as the other managers.

The settings UI currently has no working provider-key deletion path: empty keys are skipped on Save even though `ApiKeyStore::removeConfig()` exists. Add an explicit Forget action and securely clear plaintext edit buffers.

The current session format also stores `systemPrompt` and `tokenLimit`, even though the settings UI presents them as global. Loading a session can override global values.

`OpenAIProvider` defaults to `https://ai.ikik.net/v1`, a third-party OpenAI-compatible gateway. HTTPS validates transport, not recipient trust. Do not silently introduce or retain third-party endpoints without explicit UI/documentation disclosure.

## IPC (`ipc/`)

`IpcProtocol.*` defines a transport-independent native frame codec. Its header is exactly 24 bytes and is encoded field-by-field in little endian: `AMEM` magic, exact `1.0` version, message type, zero flags, request id, and payload length. It supports `Hello`, `HelloAck`, `Request`, `Response`, `Cancel`, and `Error`; handshake ids must be zero, request/response/cancel ids must be non-zero. Requests are capped at 1 MiB, all other frames at 4 MiB, and payloads must be valid UTF-8. Invalid headers are rejected before payload allocation; partial frames return `NeedMoreData` without consuming input.

`IpcFramedConnection` performs overlapped exact reads/writes with absolute deadlines and the server stop event. It reads and validates the 24-byte header through `DecodeHeader()` before allocating payload storage, treats truncated frames as protocol errors, loops over partial writes, and reports complete/closed/cancelled/timed-out/protocol/I/O outcomes.

`NamedPipeServer.*` and `NativePipeSecurity.*` provide a serial transport foundation for `\\.\pipe\AMem.NativeAgent.v1`. The protected DACL grants pipe read/write only to the current process user SID and SYSTEM. The server uses `PIPE_REJECT_REMOTE_CLIENTS`, `FILE_FLAG_FIRST_PIPE_INSTANCE`, one reusable instance, overlapped accept, a stop event plus `CancelIoEx`, and one joinable server/handler thread. `snapshot()` exposes stopped/listening/connected/stopping/failed state, accepted count, pipe name, and last error.

`IpcHandshakeSession` requires the first frame to be a maximum 16 KiB `Hello` JSON. `client_name` is required and bounded, `client_version` is optional and bounded, and `requested_capabilities` accepts each known capability once. `HelloAck` grants only requested `Observe`; requested `TargetSelection`, `TargetMutation`, and `HostExecution` capabilities are denied. Unsupported header versions/oversized frames close without a response; wrong first types or invalid JSON/schema receive a bounded structured `Error` before close. An established caller must keep the handler alive for the next session stage.

`IpcRequestProtocol` requires Request JSON `{ "method": string, "params": object, "timeout_ms"?: uint }`, rejects unknown fields, bounds method names to 128 bytes, defaults timeout to 30 seconds, and caps it at 5 minutes. Cancel payload is exactly `{}`. Response payloads use stable `ok`, `completion`, and `result`/`error` fields and are validated before bounded serialization.

`IpcRequestSession` persists after `HelloAck`, permits one active request, remembers at most 1024 unique request ids for the connection lifetime, and requires reconnect after that limit. A server-owned dispatcher resolves each method's required capability; the client cannot self-assert it. The caller thread keeps reading Request/Cancel frames while one owned joinable worker dispatches, so client Cancel, absolute server deadline, pipe Stop, invalid/duplicate ids, and session shutdown have explicit outcomes. Cancellation remains cooperative; dispatchers must observe the supplied context, and a sent device operation is not thereby retracted.

`IpcMethodCatalog` contains the same 24 canonical names as the in-app registry: 12 Observe, 1 TargetSelection, 9 TargetMutation, and 2 HostExecution; target policies are 3 None, 20 Bound, and 1 Selection. Only the 12 Observe entries are executable without approval. `IpcMemServiceDispatcher` reuses `MemJsonTools` for those methods, captures `{connectionGeneration, target}` once per external session, checks it before/after dispatch and at 250 ms reader intervals, and propagates the same absolute deadline and atomic cancellation token into `OperationContext`. A stale baseline emits session-level Error, cancels active work, and joins. Privileged methods return `approval_required` before parsing or service invocation even if a caller constructs a dispatcher directly.

`NativeAgentRuntime` owns the `NamedPipeServer` and composes each accepted handle as framed connection -> handshake -> `IpcMemServiceDispatcher` -> request session. Its snapshot exposes only lifecycle/client identity/capability/status/counters and clears active client identity after completion; request params and results are never retained. Stop signals the pipe, cancels handshake or active dispatch, and joins the server handler and request worker. `ENABLE_NATIVE_IPC` compiles an explicit `NativeAgentIpcWindow`; it defaults OFF, and compiled builds still start with no listener. The window lazily creates the system runtime, shows status/counters, and requires a user click to enable Observe. `main.cpp` only performs shutdown before device disconnect. Privileged capability grants and the approval broker remain unimplemented, so do not describe native IPC as a privileged external Agent adapter.

### Legacy HTTP Server (`ipc/IpcServer.*`)

When explicitly compiled in, the IPC server accepts HTTP JSON on `127.0.0.1:28100`:

```json
{"method": "read_memory", "params": {"address": "0x1234", "size": 16}}
```

Methods call the same C++ device commands used by GUI and in-app tools.

Security/lifecycle constraints when the legacy option is enabled:

- There is no authentication token.
- Responses allow `Access-Control-Allow-Origin: *`.
- Browser `OPTIONS` requests are accepted.
- External IPC calls bypass the in-app `AgentRunner` write approval.
- Each client handler is detached and not drained by `Stop()`.
- Responses use one `send()` call and do not handle short writes.

Loopback binding is not authentication. Do not add new privileged IPC methods without addressing authentication/capability and browser access. Do not rely on browser Private Network Access behavior as the service security boundary.

## Removed Python MCP Proxy

The FastMCP package, `.mcp.json`, install metadata, and IDE configurations have been removed. Do not restore a Python wrapper around the legacy HTTP IPC. The optional `tools/protocol_reference/amem_client.py` script talks directly to the Android wire protocol for manual diagnostics; it is not an Agent integration or a second protocol authority.

The remaining surfaces are still not identical: with LuaJIT the in-app registry has 24 executable and advertised canonical definitions; without it the count is 23. Opt-in legacy HTTP IPC has 29 methods and does not use the in-app approval, target-context, result, or feature-gate contract. Keep a machine-checkable capability matrix while IPC is replaced or deleted.

Address parsing currently differs:

- In-app address fields, including raw/typed memory, scan ranges, disassembly, pointer offsets, and breakpoint addresses, require explicit `0x` strings. Retired aliases are not executable.
- Legacy HTTP IPC interprets an unprefixed string as decimal; hexadecimal requires `0x`.

Until the parsers are unified, require `0x` for every address string in schemas, prompts, examples, and tests.

An external HTTP client timeout does not cancel the detached C++ handler. Do not add automatic retries without server request ids/cancellation and resource/idempotency analysis; a method that initializes or changes shared state is not retry-safe merely because it does not modify target memory.

## Lua

`LuaEngine` owns LuaJIT. Bindings are in:

- `LuaAPI.cpp`
- `LuaAPI_Memory.cpp`
- `LuaAPI_ImGui.cpp`
- `LuaAPI_Assembly.cpp`

Lua is reachable from GUI windows, the in-app AI tool, and IPC. Treat arbitrary Lua as privileged write/stateful execution.
The only in-app Lua name is `lua_execute`, registered only with `HAVE_LUAJIT`; retired `execute_lua` session calls are preserved only as inert history text. Its timeout is bounded by the Agent task deadline, and cancellation after execution starts is only reported on the receipt, not treated as a hard stop.

## Required Engineering Patterns

1. Preserve ImGui main-thread isolation.
2. Use the shared socket command layer and per-port locking.
3. Add a higher-level transaction for multi-command shared-state operations.
4. Validate `processRevision` for process-bound Agent operations.
5. Send provider history through `getMessagesForRequest()`.
6. Keep tool safety metadata and `DefaultSystemPrompt.h` synchronized.
7. Require explicit `0x` address strings across Agent and IPC boundaries.
8. Bound network input, tool output, and persisted data before allocation.
9. Redact secrets before tool audit/session persistence.
10. Use reversible config loading: parse/validate, then commit.
11. Do not add detached workers or describe bounded waits as full drainage.
12. Treat IPC authentication and provider endpoint choice as security decisions.
13. Require a valid provider stream terminal before committing success or executing tools.
14. Treat socket timeout/partial I/O as connection poisoning, not something a one-shot drain proves recovered.
15. Serialize connect/disconnect against active requests and bind state to a connection generation.
16. Keep a generated capability matrix for in-app tools, temporary IPC methods, and feature gates.
17. Reserve provider context for tool schemas and output; do not rely only on UTF-8 bytes/4.
18. Encode IPC headers field-by-field; keep version, flags, request-id, UTF-8, and immutable payload-limit validation at the codec boundary.
19. Keep native IPC compile-time default-off and runtime default-stopped; only explicit GUI action may enable Observe. Privileged capabilities remain denied until approval, target binding, invalidation, and audit are complete; all handlers and workers stay owned and joinable.
20. Validate native frame headers before allocation; handshake grants only Observe, request capabilities/target policy come from the shared catalog, session baselines are generation-bound, and cancellation remains cooperative.

## Branches

- `dev`: remote default/main integration branch.
- `NativeAgent`: independent native in-app Agent branch. It is a separate product line and is not currently intended to merge into `dev`; follow `docs/native_agent_refactor_plan.md`.
- `AIChat`: AI Chat + MCP/IPC baseline branch.
- `WinGui`: earlier Windows GUI branch.
- `docking`: earlier development branch.

When changing Agent architecture or safety semantics, update `AGENTS.md`, `CLAUDE.md`, and the relevant `docs/agent_*.md` files in the same change.
