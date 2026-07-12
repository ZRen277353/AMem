# AGENTS.md

This file is the repository-level guide for coding agents working on AMem. The current AI-agent architecture, runtime walkthrough, and audit findings live in:

- `docs/agent_architecture.md`
- `docs/agent_walkthrough.md`
- `docs/agent_project_issues.md`
- `docs/native_agent_refactor_plan.md`

Read the relevant documents before changing `gui/ai/`, `ipc/`, `mcp/`, shared process state, or socket commands.

## Project Overview

AMem is a Windows desktop application for remote Android memory debugging, similar to Cheat Engine. It connects to an Android server over sockets and provides memory scanning, memory read/write, hardware breakpoints, a hex viewer, value freezing, ELF symbols, and Lua scripting. Dear ImGui (docking branch) is rendered through DirectX 12.

There are two AI integration paths:

1. The in-app AI Chat agent in `gui/ai/` supports Claude, OpenAI-compatible, and DeepSeek providers and can call debugger tools.
2. The Python MCP server in `mcp/` exposes AMem to external assistants through the GUI's local HTTP IPC server.

Language: C++17 for the app, Python 3.10+ for MCP. Platform: Windows 10/11 x64.

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

`native_agent_mem_service` is the current no-device C++ test. Its 22 groups cover address and scalar codecs, native `MemService` adapters, driver initialization receipts and card redaction, module/pointer/disassembly, native scan/symbol sessions and breakpoint receipts/hit batches, mutation-audit redaction/rotation/late delivery, raw/typed write completion semantics, target/generation checks, `DeviceSession` locking/poisoning, approval invalidation, and the `AgentTaskExecutor` queue/cancellation/shutdown lifecycle. Provider, IPC, real transport, and device operations still lack complete automation. For protocol checks use `mcp/reference/amem_client.py`; for the live Lua API use `scripts/dump_api.lua` as described in `scripts/README.md`. Changes involving real device state, concurrency, cancellation, or teardown still need manual end-to-end verification with the GUI and an Android device.

### MCP Server

```bash
cd mcp
pip install -e .
amem-mcp
```

The MCP process uses stdio for MCP and HTTP for its connection to the GUI. The GUI must be running, its IPC server must be listening, and the Android connection must already be available.

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
- `LUAJIT_STATIC` (default ON)

Compile-time gates:

- `HAVE_AI_CHAT`
- `HAVE_CAPSTONE`
- `HAVE_KEYSTONE`
- `HAVE_LUAJIT`

Source that depends on an optional feature must remain correctly guarded.

## Unifying Command Pipeline

The free functions declared in `socket/client_singleton.h` are the shared device-protocol surface. The three front ends all terminate there:

```text
GUI windows --------------------+
In-app AI ToolDefinitions ------+--> socket/client_singleton.h
MCP -> IPC handlers ------------+      -> WinSocketClientMgr -> Android
```

When adding a device capability:

1. Implement the protocol command in the appropriate `socket/*Commands.cpp`.
2. Declare it in `socket/client_singleton.h`.
3. Surface it only where required: a GUI panel, `ToolDefinitions.cpp`, and/or an IPC method plus Python MCP wrapper.
4. Keep validation, error semantics, flags, and output limits aligned across all exposed front ends.

Do not implement a second version of the wire protocol in AI or IPC code.

## Main Application Architecture

### Entry Point and Rendering

- `main.cpp` creates the Win32 window, starts IPC on port 28100, drives `Gui::mainLoop()`, and runs shutdown.
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

Every scan mutation advances `WinSocketClientMgr`'s monotonic scan epoch, including legacy GUI/IPC commands. Canonical `scan_start` holds one transaction across range selection and scan execution; `scan_refine`, `scan_results`, and `scan_clear` require the latest returned epoch. An epoch mismatch is a session conflict, not a retryable transport error.

Every symbol-table initialization advances a monotonic symbol epoch while holding the port transaction gate. Canonical `symbol_resolve` and `symbol_list` resolve the module, initialize its symbol table, and find/fetch inside one MAIN transaction. Symbol continuation pages require the previous result's epoch; legacy IPC/hidden-alias initialization invalidates it. GUI address formatting uses `IMemService::loadSymbolTable()` to initialize once and fetch the complete bounded table under one transaction before atomically installing a target-validated cache entry.

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
- With LuaJIT, `ToolDefinitions.cpp` has 57 executable names: 33 hidden legacy aliases and 24 advertised definitions. Without `HAVE_LUAJIT`, neither Lua name is registered, leaving 55 executable / 32 hidden / 23 advertised.
- The native slice (`mem/`, `AgentMemTools`) owns status/driver/process/open, module/pointer/disassembly/symbol resolution, scan/symbol sessions, breakpoint mutations/hits, raw and typed memory validation, scalar encoding, and structured results. Do not bypass it when extending those operations.
- `ChatSession::getMessagesForRequest()` is the required provider boundary; it cleans and pairs tool calls/results.

### Tool Safety

Every in-app tool is `ToolSafety::ReadOnly` or `ToolSafety::Write`. Write tools require confirmation unless `autoApproveWrites` is enabled.

Under the current binary safety model, `ToolSafety::Write` means target/host mutation requiring approval. Canonical `symbol_resolve` and `symbol_list` remain ReadOnly: their internal active-table mutation is serialized, epoch-bound, and does not modify target memory. `symbol_init`, `symbol_find`, and `resolve_symbol` are hidden compatibility names, and `DefaultSystemPrompt.h` lists only the canonical tools. A future richer effect model should represent this as session mutation without restoring model-visible initialization steps.

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

This closes the boundary only for operations migrated to `MemService`, including driver initialization, module/pointer/disassembly/symbol resolution, canonical scan/symbol/breakpoint operations, and raw/typed memory read/write. `driver_initialize` is connection-bound and distinguishes unsent, rejected, completion-unknown, and confirmed-after-cancel/deadline outcomes. `pointer_resolve` holds one read transaction across module lookup and every dereference. Canonical scan tools bind `{target, scanEpoch}` and report confirmed completion after cancellation/deadline; an unconfirmed sent scan is `completion_unknown`. Canonical symbol tools bind module lookup, initialization, and list/find to one transaction and return a module-bound epoch. Canonical breakpoint mutations bind the target at send and preserve confirmed/unknown completion; hit batches are target-bound. `lua_execute` revalidates the target at its host-execution boundary and never extends the task's absolute deadline, but cannot retract a script after it starts. New and legacy process-bound operations must consume the explicit `OperationContext` again at the actual service/socket send boundary. The approval dialog still lacks the process name.

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

## IPC Server (`ipc/IpcServer.*`)

The IPC server accepts HTTP JSON on `127.0.0.1:28100`:

```json
{"method": "read_memory", "params": {"address": "0x1234", "size": 16}}
```

Methods call the same C++ device commands used by GUI and in-app tools.

Current security/lifecycle constraints:

- There is no authentication token.
- Responses allow `Access-Control-Allow-Origin: *`.
- Browser `OPTIONS` requests are accepted.
- External IPC calls bypass the in-app `AgentRunner` write approval.
- Each client handler is detached and not drained by `Stop()`.
- Responses use one `send()` call and do not handle short writes.

Loopback binding is not authentication. Do not add new privileged IPC methods without addressing authentication/capability and browser access. Do not rely on browser Private Network Access behavior as the service security boundary.

## MCP Server (`mcp/`)

The Python package is a FastMCP stdio proxy. It does not connect to Android directly:

```text
External assistant -> FastMCP tool -> IpcClient -> GUI IPC -> socket command
```

Keep `mcp/amem_mcp/constants.py` synchronized with C++ scan flags, value types, and memory region enums.

The exposed surfaces are intentionally overlapping, not identical: with LuaJIT the in-app registry has 24 advertised definitions and 57 executable names (33 hidden aliases); without it the counts are 23/55/32. IPC has 29 methods and MCP has 30 tools. IPC `read_batch` is not wrapped by MCP; canonical in-app `disassemble`/`symbol_resolve`/`breakpoint_hits` have no same-name IPC method; Lua availability also differs by feature gate. Keep a machine-checkable capability matrix rather than claiming MCP exposes every C++ capability.

Address parsing currently differs:

- Canonical in-app memory tools (`memory_read`, `memory_write`, `memory_read_value`, `memory_write_value`), `pointer_resolve` offsets, and breakpoint addresses reject unprefixed hexadecimal strings; hidden and unmigrated legacy tools still interpret many unprefixed strings as hexadecimal.
- IPC/MCP interprets an unprefixed string as decimal; hexadecimal requires `0x`.

Until the parsers are unified, require `0x` for every address string in schemas, prompts, examples, and tests.

MCP retry lists must include only operations that are truly safe to repeat. A method that initializes or changes shared state is not automatically retry-safe merely because it does not modify target memory.

Python HTTP timeout does not cancel the detached C++ handler. A retry can overlap the old request; do not add automatic retries without server request ids/cancellation and resource/idempotency analysis.

## Lua

`LuaEngine` owns LuaJIT. Bindings are in:

- `LuaAPI.cpp`
- `LuaAPI_Memory.cpp`
- `LuaAPI_ImGui.cpp`
- `LuaAPI_Assembly.cpp`

Lua is reachable from GUI windows, the in-app AI tool, and IPC. Treat arbitrary Lua as privileged write/stateful execution.
The in-app canonical name is `lua_execute`; hidden `execute_lua` compatibility is registered only with `HAVE_LUAJIT`. Its timeout is bounded by the Agent task deadline, and cancellation after execution starts is only reported on the receipt, not treated as a hard stop.

## Required Engineering Patterns

1. Preserve ImGui main-thread isolation.
2. Use the shared socket command layer and per-port locking.
3. Add a higher-level transaction for multi-command shared-state operations.
4. Validate `processRevision` for process-bound Agent operations.
5. Send provider history through `getMessagesForRequest()`.
6. Keep tool safety metadata and `DefaultSystemPrompt.h` synchronized.
7. Require explicit `0x` address strings across Agent/MCP boundaries.
8. Bound network input, tool output, and persisted data before allocation.
9. Redact secrets before tool audit/session persistence.
10. Use reversible config loading: parse/validate, then commit.
11. Do not add detached workers or describe bounded waits as full drainage.
12. Treat IPC authentication and provider endpoint choice as security decisions.
13. Require a valid provider stream terminal before committing success or executing tools.
14. Treat socket timeout/partial I/O as connection poisoning, not something a one-shot drain proves recovered.
15. Serialize connect/disconnect against active requests and bind state to a connection generation.
16. Keep a generated capability matrix for in-app tools, IPC methods, MCP tools, and feature gates.
17. Reserve provider context for tool schemas and output; do not rely only on UTF-8 bytes/4.

## Branches

- `dev`: remote default/main integration branch.
- `NativeAgent`: independent native in-app Agent branch. It is a separate product line and is not currently intended to merge into `dev`; follow `docs/native_agent_refactor_plan.md`.
- `AIChat`: AI Chat + MCP/IPC baseline branch.
- `WinGui`: earlier Windows GUI branch.
- `docking`: earlier development branch.

When changing Agent architecture or safety semantics, update `AGENTS.md`, `CLAUDE.md`, and the relevant `docs/agent_*.md` files in the same change.
