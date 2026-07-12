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

`native_agent_mem_service` is the current no-device C++ test. Its 15 groups cover address and scalar codecs, native `MemService` adapters, module list/resolve, raw/typed write completion semantics, target/generation checks, `DeviceSession` locking/poisoning, approval invalidation, and the `AgentTaskExecutor` queue/cancellation/shutdown lifecycle. Provider, IPC, real transport, and device operations still lack complete automation. For protocol checks use `mcp/reference/amem_client.py`; for the live Lua API use `scripts/dump_api.lua` as described in `scripts/README.md`. Changes involving real device state, concurrency, cancellation, or teardown still need manual end-to-end verification with the GUI and an Android device.

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
- `WinSocketClientMgr` owns `PORT_MAIN`, `PORT_DEBUG`, and `PORT_ERROR`, each with a mutex.
- Commands are split across `ProcessCommands.cpp`, `MemoryCommands.cpp`, `ScanCommands.cpp`, `BreakpointCommands.cpp`, `FreezeCommands.cpp`, and `SymbolCommands.cpp`.
- Prefer `SocketCommand::execute`, `executeNoHandle`, or `executeWithResult`. These handle the shared connection lease, generation checks, process handle setup, and per-port locking.
- `SocketIoTimeout::ScopedTimeout` applies a thread-local I/O budget.

Critical distinction: a port mutex makes one request-response pair serial. It does not make a multi-command business operation transactional. Existing compound sequences include:

- `ScanSetRange` followed by a scan command
- `SymbolInit` followed by `SymbolGetList`
- the cleanup/open/set-PID sequence in `AppContext::selectProcess()`

If a new operation depends on multiple commands sharing global server state, use a higher-level transaction/revision/epoch or add a server-side compound command.

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
- `ToolDefinitions.cpp` currently has 43 executable names. Thirteen legacy aliases are hidden from providers, leaving 30 advertised definitions.
- The native slice (`mem/`, `AgentMemTools`) owns status/process/open, module list/resolve, raw and typed memory validation, scalar encoding, and structured results. Do not bypass it when extending those operations.
- `ChatSession::getMessagesForRequest()` is the required provider boundary; it cleans and pairs tool calls/results.

### Tool Safety

Every in-app tool is `ToolSafety::ReadOnly` or `ToolSafety::Write`. Write tools require confirmation unless `autoApproveWrites` is enabled.

Current code classifies `symbol_init` and `symbol_list` as ReadOnly because they do not modify target memory, but they do mutate the server's active symbol-table state. `DefaultSystemPrompt.h` still describes them as write-classified. When changing this area, define whether safety means target mutation or any shared-state mutation, then update registration, prompt, docs, and retry policy together.

Unknown tools cannot execute and are rejected by `ToolExecutor`.

### Threading and Cancellation

Provider and tool threads must never access ImGui or run state directly. In-app background results return through `UIMessageQueue`.

Current runtime ownership is imperfect:

- `HttpClient::postAsync()` creates detached HTTP workers.
- In-app tools run only on the joinable `AgentTaskExecutor` worker; do not reintroduce outer or inner detached tool threads.
- Stop cancels model orchestration and signals queued/active tool contexts. It cannot retract a sent write, and the current run-id filter still drops its late final result from the session/trace.
- `AgentTaskExecutor::shutdown()` cancels queued/active work and joins the worker before device disconnect. `HttpClient::shutdown()` remains a bounded best-effort wait, not proof that every HTTP worker exited.
- Claude/DeepSeek record stream terminal state but do not validate it; OpenAI does not record it. HTTP 2xx with a truncated stream can currently be committed as success.

Do not add new detached threads. Extend the owned task model and keep completion callbacks in lifecycle accounting. See `docs/agent_project_issues.md` before touching cancellation or teardown.

### Target Binding

`AgentRunContext` captures the connection generation and `{pid, handle, processRevision}`. The approval dialog shows expected generation, PID, and revision; approval, dequeue, and result collection revalidate them. `process_open` uses `Selection` policy and explicitly advances the run context only when its returned snapshot is still current.

This closes the boundary only for operations migrated to `MemService`, including module list/resolve and raw/typed memory read/write. New and legacy process-bound operations must consume the explicit `OperationContext` again at the actual service/socket send boundary. The approval dialog still lacks the process name, and Stop-time late write receipts still need independent audit visibility.

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

Do not claim that all AI data is encrypted. Session files can contain driver cards, Lua code, addresses, memory bytes, registers, tool arguments, and tool results. That history can also be sent to the active remote provider in later turns.

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

The exposed surfaces are intentionally overlapping, not identical: the in-app registry has 30 advertised definitions and 43 executable names (13 hidden aliases), IPC has 29 methods, and MCP has 30 tools. IPC `read_batch` is not wrapped by MCP; in-app `read_disassembly`/`resolve_symbol` have no same-name IPC method; Lua availability also differs by feature gate. Keep a machine-checkable capability matrix rather than claiming MCP exposes every C++ capability.

Address parsing currently differs:

- Canonical in-app memory tools (`memory_read`, `memory_write`, `memory_read_value`, `memory_write_value`) reject unprefixed addresses; hidden and unmigrated legacy tools still interpret many unprefixed strings as hexadecimal.
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
