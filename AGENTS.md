# AGENTS.md

This file is the repository-level guide for coding agents working on AMem. The current AI-agent architecture, runtime walkthrough, and audit findings live in:

- `docs/agent_architecture.md`
- `docs/agent_walkthrough.md`
- `docs/agent_project_issues.md`
- `docs/native_agent_refactor_plan.md`

Read the relevant documents before changing `gui/ai/`, `ipc/`, `tools/protocol_reference/`, shared process state, or socket commands.

## Project Overview

AMem is a Windows desktop application for remote Android memory debugging, similar to Cheat Engine. It connects to an Android server over sockets and provides memory scanning, memory read/write, hardware breakpoints, a hex viewer, value freezing, ELF symbols, and Lua scripting. Dear ImGui (docking branch) is rendered through DirectX 12.

The supported AI integration is the in-app AI Chat agent in `gui/ai/`. It supports Claude, OpenAI-compatible, and DeepSeek providers and calls native debugger tools. The Python MCP proxy, old loopback HTTP IPC server, and their IDE/build entry points have been removed from `NativeAgent`. Native framing, secured transport, strict Hello/request sessions, a complete method catalog, 12 Observe adapters, an owned runtime, explicit status/control, and a bounded privileged-approval UI/state machine now exist under `ipc/` and `gui/`. Hello still grants only `Observe`; each privileged request instead submits bounded metadata for a one-shot GUI decision while the reader remains available for Cancel. After durable grant consumption, the dispatcher executes all 11 `MemService`-backed privileged methods through shared `MemJsonTools` and executes `lua_execute` through an injected host boundary. `process_open` is the only controlled session-baseline transition. The bounded plaintext security JSONL distinguishes approval transitions from final execution outcomes without storing params, result JSON, or error messages; failures are visible in the control window. `ENABLE_NATIVE_IPC` still defaults OFF; even when compiled, the pipe starts disabled and requires a user action.

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

The three standard CMake output-directory variables may be overridden for an
isolated build; defaults remain `bin/` and `lib/` under the source tree.

The project can also be opened directly through `CMakeLists.txt` in Visual Studio 2022 using an x64 Release/Debug configuration.

`native_agent_mem_service` is the main no-device C++ test with 31 groups, including connection lifecycle, exact/invalid/partial batch reads, freeze completion receipts, target/generation checks, the ToolExecutor schema matrix, AgentRunner approval combinations, tool-history pairing, and the existing Agent boundaries. `native_app_context_state` covers odd/even target publication, stale mutation rejection, move ownership, cache invalidation, and disconnect clearing. Native IPC has 6 security-audit, 12 approval-broker, 5 protocol, 5 transport, 8 framed-I/O, 8 handshake, 7 request-contract, 9 request-session, 4 method-catalog, 15 dispatcher, and 10 owned-runtime groups. Socket coverage adds 4 client-transport groups and 6 multi-port manager groups for system Winsock loopback, partial I/O, timeout/EOF poisoning, all-port rollback, request/disconnect exclusion, and reconnect generation isolation. `native_provider_stream_terminal` has 19 parser/state-machine and payload/JSON-structure groups. `native_provider_http_integration` has 6 local HTTPS groups covering all three full-response provider paths, HTTP error mapping, response-structure limits, and certificate trust rejection. `native_context_budget` has 5 groups for UTF-8 estimation, provider/user/custom limits, tool/output reserves, complete-group trimming, and fail-closed rejection. `native_provider_trust` has 3 groups for canonical endpoint identity, implicit default trust, and endpoint-bound custom trust. `native_persistence_recovery` has 13 groups for transactional recovery, atomic-install fault recovery, JSON-structure limits, Windows-user session protection/migration/tamper rejection, provider update/remove, secure settings drafts, file/message/session limits, and global prompt/token ownership. `native_http_client_lifecycle` has 5 local-HTTP groups, including the 16 MiB receiver boundary. Thirty CTests include nine static gates; `native_gui_mem_service_boundary` enforces the GUI/Lua/main service boundary and transactional target publication, `native_context_budget_gate` fixes dispatch integration and provider output bounds, `native_provider_trust_gate` prevents third-party defaults or trust-check bypass, and `native_session_protection_gate` prevents plaintext session/index persistence from returning. External production-provider interoperability, GUI approval clicks, real Android transport, and device operations still require manual coverage. For manual wire-protocol checks use `tools/protocol_reference/amem_client.py`; for the live Lua API use `scripts/dump_api.lua` as described in `scripts/README.md`. Changes involving real device state, concurrency, cancellation, or teardown still need manual end-to-end verification with the GUI and an Android device.

## Dependencies and Feature Gates

- Required: Visual Studio 2022, CMake 3.16+, Windows SDK, DirectX 12 SDK.
- Required: LuaJIT under `third_party/LuaJIT/` with `include/` and `lib/lua51.lib`.
- Required on `NativeAgent`: vendored cpp-httplib/nlohmann JSON plus static OpenSSL for AI Chat. There is no supported AI-off build; a missing dependency is a configure error.
- Required on `NativeAgent`: static `/MT` Capstone for disassembly and Keystone for assembly. Missing libraries are configure errors; prefer static x64 vcpkg triplets.
- The executable uses the static `/MT` CRT. Do not introduce `/MD` libraries into the final link.

CMake options:

- `USE_DX12` (default ON)
- `USE_DX11` (default OFF)
- `ENABLE_NATIVE_IPC` (default OFF; compile-only transport foundation)
- `LUAJIT_STATIC` (default ON)

Compile-time macros:

- `HAVE_AI_CHAT` (always defined in a supported `NativeAgent` product build)
- `HAVE_NATIVE_IPC`
- `HAVE_CAPSTONE` (always defined in a supported `NativeAgent` product build)
- `HAVE_KEYSTONE` (always defined in a supported `NativeAgent` product build)
- `HAVE_LUAJIT`

The existing AI/Capstone/Keystone source guards remain implementation boundaries, not supported OFF switches. Source that depends on Native IPC or another genuinely optional feature must remain correctly guarded.

## Unifying Command Pipeline

The free functions declared in `socket/client_singleton.h` are the shared device-protocol surface. The current front ends all terminate there:

```text
GUI windows --------------------+
In-app AI ToolDefinitions ------+--> MemService -> socket/client_singleton.h
Opt-in native Named Pipe IPC ---+                  -> WinSocketClientMgr -> Android
```

When adding a device capability:

1. Implement the protocol command in the appropriate `socket/*Commands.cpp`.
2. Declare it in `socket/client_singleton.h`.
3. Surface it only where required: a GUI panel, `ToolDefinitions.cpp`, and/or the native IPC catalog/dispatcher.
4. Keep validation, error semantics, flags, and output limits aligned across all exposed front ends.

Do not implement a second version of the wire protocol in AI or IPC code.

## Main Application Architecture

### Entry Point and Rendering

- `main.cpp` creates the Win32 window, drives `Gui::mainLoop()`, and runs joined shutdown. It has no HTTP IPC listener.
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

GUI, in-app Agent, and IPC all observe this object, but only `AppContext::TargetMutation` publishes PID, handle, name, revision, and presentation-cache invalidation. `SystemMemBackend::openProcess()` owns the connection lease and process mutation, performs DEBUG cleanup followed by one MAIN cleanup/close/open transaction, and publishes only the confirmed handle. Every GUI and Lua device operation enters through the injected `IMemService`; each in-app Agent run also captures and revalidates generation plus target at approval, dequeue, send, and result collection. `runId` alone is never a target identifier.

`EventBus` and events in `gui/Events.h` decouple GUI windows. Do not use the event bus as a substitute for target revision validation.

## Socket Communication

- `client.hpp` wraps Winsock send/receive behind `IWindowsSocketOps`; production uses `SystemWindowsSocketOps`, while deterministic tests inject scripted partial I/O and failures.
- `MultiPortClientManager` owns MAIN/DEBUG/ERROR clients, the shared `DeviceSession` lifecycle transition, all-port rollback, target-reset callback, poison propagation, and explicit reconnect. `WinSocketClientMgr` delegates connection ownership to it and retains protocol locks/epochs.
- `WinSocketClientMgr` owns `PORT_MAIN`, `PORT_DEBUG`, and `PORT_ERROR`, each with a request-response mutex and a recursive high-level transaction gate.
- Commands are split across `ProcessCommands.cpp`, `MemoryCommands.cpp`, `ScanCommands.cpp`, `BreakpointCommands.cpp`, `FreezeCommands.cpp`, and `SymbolCommands.cpp`.
- Prefer `SocketCommand::execute`, `executeNoHandle`, or `executeWithResult`. These handle the shared connection lease, generation checks, process handle setup, and per-port locking.
- `SocketIoTimeout::ScopedTimeout` applies a thread-local I/O budget.

Critical distinction: a port mutex makes one request-response pair serial. It does not make a multi-command business operation transactional. Existing compound sequences include:

- `ScanSetRange` followed by a scan command
- `SymbolInit` followed by `SymbolGetList`
- the DEBUG cleanup followed by MAIN cleanup/close/open sequence in `SystemMemBackend::openProcess()`

If a new operation depends on multiple commands sharing global server state, use a higher-level transaction/revision/epoch or add a server-side compound command.

`SocketCommand::TransactionLease` holds the connection request lease and one per-port transaction gate. Normal commands hold it briefly; pointer-chain resolution keeps it across module lookup and every pointer read. Do not call target APIs that take `AppContext::processStateMutex_` while holding this gate; use the stable revision check before the transaction and the full snapshot check after release.

Every scan mutation advances `WinSocketClientMgr`'s monotonic scan epoch, including any low-level mutation inside the protocol backend. Canonical `scan_start` holds one transaction across range selection and scan execution; `scan_refine`, `scan_results`, and `scan_clear` require the latest returned epoch. `ScanWindow` is injected with `IMemService` and uses the same target-bound session for start/refine/results/clear/removal. Its Stop button only sets the shared cancellation token; the system backend issues the DEBUG stop from the progress callback and the GUI must preserve confirmed-after-cancel versus `completion_unknown`. An epoch mismatch is a session conflict, not a retryable transport error.

Every symbol-table initialization advances a monotonic symbol epoch while holding the port transaction gate. Canonical `symbol_resolve` and `symbol_list` resolve the module, initialize its symbol table, and find/fetch inside one MAIN transaction. Symbol continuation pages require the previous result's epoch; any competing low-level initialization invalidates it. GUI address formatting uses `IMemService::loadSymbolTable()` to initialize once and fetch the complete bounded table under one transaction before atomically installing a target-validated cache entry.

Canonical breakpoint mutations return tracked completion receipts. A sent request without a response is non-retryable `completion_unknown`; confirmed completion after cancellation/deadline remains explicit. Remote confirmation and the local cleanup tracker update occur under the same MAIN transaction, cleanup holds that gate across snapshot/removal, and disconnect clears the local tracker without sending to a stale target. Breakpoint hit responses have no wire cursor: the service drains one response and retains its newest bounded tail. The Agent limit is 100 and reports `available`/`dropped`; do not invent continuation offsets.

Validate every untrusted count, string length, and byte size before allocating or receiving variable-length data.

`DeviceSession` gives commands a shared request lease and connect/disconnect/reconnect an exclusive lifecycle lease. `WSAETIMEDOUT`, EOF, and partial I/O poison the connection, advance its generation, close the failed client, and reject reuse until explicit reconnect. The old pending-data drain recovery path has been removed. Do not add direct `Connect()`/`Close()` calls or bypass the lifecycle gate; process handles and target snapshots remain generation-bound.

The Android server's runtime `SysCall`/`Kernel` type only selects the read/write and breakpoint implementation. It is not target identity, does not belong in `TargetSnapshot`, and does not require connection-generation or process-revision invalidation when switched.

Module enumeration returns mappings, not coalesced ELF objects. Resolution groups rows by case-normalized full path and uses the lowest mapping base for one ELF, while `module_list` preserves the raw segments. Distinct full paths with the same basename remain ambiguous. Keep this behavior covered for `module_resolve`, pointer resolution, and symbol lookup; the current wire fields cannot distinguish multiple true load instances of the same path.

`native_socket_client_transport` proves that partial send/receive offsets are preserved, timeout/EOF closes and poisons the failed stream, an in-flight lease becomes stale, and late bytes queued on the old endpoint cannot enter the explicitly reconnected generation. This is the client-level boundary; the companion manager suite covers three-port ownership, while Android-device behavior remains a smoke-test requirement.

`native_multi_port_client_manager` runs the production manager against both scripted failures and a real three-stream Winsock loopback. It covers MAIN/DEBUG/ERROR connect rollback, single-port poison, full reconnect, active-request exclusion, and repeated fresh endpoint generations. Android protocol and remote-state restoration still require device smoke tests.

## In-App AI Chat (`gui/ai/`)

The subsystem is under the `AI` namespace. Supported `NativeAgent` builds always define `HAVE_AI_CHAT`; the source guard is not a user-facing build option.

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

Current runtime ownership:

- `HttpClient::postAsync()` stores every request worker as a joinable thread together with its cancellation token and a best-effort `httplib::Client::stop()` hook. A request becomes reapable only after its provider completion callback returns.
- In-app tools run only on the joinable `AgentTaskExecutor` worker; do not reintroduce outer or inner detached tool threads.
- Stop cancels model orchestration and signals queued/active tool contexts. It cannot retract a sent write. Late results remain excluded from the stale chat/session by the run-id filter, but mutation and symbol-session outcomes are persisted to `ai_mutation_audit.jsonl` before the UI callback and remain visible in the Audit table.
- `HttpClient::shutdown()` rejects new work, sets every request token, invokes transport stop hooks, and joins all request workers through completion callbacks. `AgentTaskExecutor::shutdown()` then cancels queued/active tool work and joins before device disconnect.
- Provider streams pass every SSE event, including `[DONE]`, through `StreamTerminalTracker`. Claude requires `message_start` plus `message_stop`; OpenAI-compatible providers require a valid `choices` start plus non-null string `finish_reason` or `[DONE]`. A malformed or unterminated HTTP 2xx response is `InvalidResponse`; partial content/tool calls remain displayable but cannot execute.

Do not add detached threads. Extend the owned task model and keep completion callbacks in lifecycle accounting. `Client::stop()` is an interruption hint, not proof that a silent read ended immediately; shutdown may wait for the configured I/O timeout, but it must not return while a worker or provider callback is alive. See `docs/agent_project_issues.md` before touching cancellation or teardown.

### Target Binding

`AgentRunContext` captures the connection generation and `{pid, handle, processRevision}`. The approval dialog shows expected generation, PID, and revision; approval, dequeue, and result collection revalidate them. `process_open` uses `Selection` policy and explicitly advances the run context only when its returned snapshot is still current.

All currently registered in-app operations now use the target-bound native boundary: driver initialization, module/pointer/disassembly/symbol resolution, canonical scan/symbol/breakpoint operations, and raw/typed memory read/write use `MemService`; `lua_execute` validates its host boundary and binds the same approved `OperationContext` in the Lua registry for the whole script. Every Lua memory/process/module/scan/breakpoint API therefore reuses the caller's generation, target, deadline, and cancellation instead of recapturing a new target. `driver_initialize` is connection-bound and distinguishes unsent, rejected, completion-unknown, and confirmed-after-cancel/deadline outcomes. `pointer_resolve` holds one read transaction across module lookup and every dereference. Canonical scan tools bind `{target, scanEpoch}` and report confirmed completion after cancellation/deadline; an unconfirmed sent scan is `completion_unknown`. Canonical symbol tools bind module lookup, initialization, and list/find to one transaction and return a module-bound epoch. Canonical breakpoint mutations bind the target at send and preserve confirmed/unknown completion; hit batches are target-bound. Lua cannot retract a script after it starts. New process-bound operations must consume the explicit `OperationContext` again at their actual service/socket send boundary. The approval dialog still lacks the process name.

`Gui.cpp` is the GUI composition root. It obtains the system service once and explicitly injects it through `CEWindow`, `ServerConnectWindow`, `ProcessListWindow`, `ModulesWindow`, `VersionWindow`, `ScanWindow`, `MemoryViewerWindow`, `BreakpointWindow`, and `LuaScriptWindow`; their subpanels receive the same reference. GUI and Lua source no longer include `client_singleton.h` or invoke device protocol functions. `main.cpp` also disconnects through `IMemService` after Native IPC, HTTP, and tool workers have stopped.

`BreakpointWindow` routes set/remove/suspend/resume plus hit-history refresh through the target-bound service contract. `BreakpointHit` preserves GPR, `orig_x0`, syscall number, FPSR/FPCR, and all 32 128-bit vector registers. GUI refresh drains one DEBUG-port response while retaining at most the newest 50,000 entries; the Agent uses the same batch contract with a 100-entry cap.

`MemoryViewerWindow` also receives `IMemService`. `AppContext::ModuleCache` remains the GUI presentation cache, but symbol loading no longer sends `SymbolInit`/`SymbolGetList` itself. Complete tables are limited to 1,000,000 entries and 64 MiB of names; cache installation revalidates the service result's target and current module while holding the cache mutex.

### Limits

`AiLimits.h` is the shared in-app boundary: raw HTTP is 16 MiB; one SSE line/event is 1/2 MiB; assistant content is 4 MiB; ordinary message content is 8 MiB; one response has at most 64 tool calls with 256-byte ids, 64-byte names, 512 KiB per-call arguments, and 4 MiB total arguments; final tool JSON is 4 MiB. A mutation whose oversized result is discarded returns `completion_unknown` rather than claiming it did not run.

Persistence JSON is capped at 32 MiB before parse and before session install. A session retains at most 16 MiB of message/tool payload and 1,000 materialized messages; disk input may contain at most 10,000 messages. Runtime trimming removes complete conversation groups and rejects an oversized protected latest turn transactionally. A failed assistant/tool append stops the current Agent chain instead of executing or following up with an unrecorded message.

These byte limits do not replace pagination or an exact model tokenizer. Every untrusted JSON boundary first applies byte, depth, node, container-item, per-string, and cumulative-string limits before constructing the nlohmann DOM; allocation failures fail closed. DOM/TLS/library overhead can still exceed serialized bytes. Local self-signed HTTPS tests exercise the real cpp-httplib/OpenSSL transport, certificate verification, all three provider full-response paths, and dense-response rejection; external production-provider interoperability remains a manual check.

`ContextBudget` runs at the sole `AgentController` provider-dispatch boundary. It combines the global user limit with provider/model `maxContextTokens`, a persisted endpoint/model override, conservative UTF-8/message framing, every advertised tool schema, provider envelope overhead, and a 256..4096 output reserve. It trims only an outgoing request copy by complete conversation groups and rejects an oversized latest group before dispatch. The same reserve is sent as each provider's output-token parameter. Built-in endpoint overrides may only lower declared capability; a custom endpoint may explicitly replace it but remains user-capped. This remains a conservative estimate, not an exact provider tokenizer.

## Persistence and Provider Trust

Files are relative to the process working directory:

- `ai_config.json`: provider configuration; API keys are DPAPI-encrypted.
- `ai_settings.json`: plain global settings.
- `ai_sessions/index.json`: current-Windows-user DPAPI envelope containing session metadata and titles.
- `ai_sessions/<id>.json`: current-Windows-user DPAPI envelope containing format-v2 chat/tool history.
- `ai_mutation_audit.jsonl`: plaintext mutation/session-effect summaries, capped at 4 MiB with one `.1` backup and 64 KiB per record.
- `native_ipc_approval_audit.jsonl`: plaintext Native IPC approval transitions, capped at 4 MiB with one `.1` backup and 16 KiB per record; it contains client/method/session/request/generation/target metadata but no request params or results.

Do not claim that all AI data is encrypted. `ai_settings.json`, mutation audit, and Native IPC approval audit remain plaintext; the audit formats omit high-risk payload fields but are not encrypted. Usable session/index files are protected at rest with current-user DPAPI, including titles, Lua, addresses, memory bytes, registers, tool arguments, and tool results. Valid legacy plaintext is migrated only after schema validation; invalid/oversized input remains unchanged and unbound. Decrypted history still exists in process memory and is sent to the configured remote provider in later turns. DPAPI is local at-rest protection, not end-to-end encryption or protection from another process already running as the same Windows user.

`ApiKeyStore`, `AiSettings`, `SessionManager`, and `ChatSession` load into temporary state and report `Loaded`, `Missing`, `Recovered`, `Invalid`, or `IoError`; only a genuinely missing file may seed defaults. Invalid/I/O loads preserve the original file and prior in-memory state. A corrupt session index is moved to a `.corrupt*` backup before valid session JSON is scanned to rebuild metadata; if preservation fails, automatic write-back is disabled. `ChatSession` uses `utils::installTempFile()`, refuses serialized output above 32 MiB, and `saveBound()` prevents a failed session load from overwriting the damaged path. These guarantees close A-09's file/message/session byte boundaries. `AiSettings` is the sole persistent owner of the system prompt and token budget, closing A-12.

The settings UI uses one `ChatSettingsDraft` for provider, prompt, numeric, and proxy edits. Opening reloads a persistence snapshot; Save is the only commit path, while Cancel/title-bar close discard and actively clear the full draft. `Forget saved key` is staged and undoable until Save. `ApiKeyStore::applyChangesAndSave()` encrypts updates, applies removals to a candidate map, and commits memory/live-provider changes only after atomic file installation. `ProviderConfig` and draft teardown actively clear owned plaintext key buffers. This does not retract already-dispatched HTTP headers or prove that OS/allocator/clipboard copies do not exist.

Inside the DPAPI envelope, session format v2 stores only the version and chat/tool messages. Version 1 files remain readable, but their legacy `systemPrompt` and `tokenLimit` fields are ignored regardless of field type. Startup validates and migrates valid inactive legacy sessions as well as the active session and index. Invalid files are never rebound or rewritten. Each successful load trims retained history against the current global token budget; provider-aware dispatch budgeting remains a separate request-copy layer so switching providers does not permanently erase longer history.

`OpenAIProvider` defaults to the official `https://api.openai.com/v1` endpoint. A custom OpenAI-compatible endpoint remains supported only after the settings UI records explicit endpoint-bound trust; scheme/authority normalization and trailing slashes do not change identity, while any path or host change invalidates trust. Both the Chat window preflight and the `AgentController` dispatch boundary reject untrusted endpoints before credentials are sent. HTTPS validates transport, not recipient trust; do not introduce another built-in third-party endpoint or bypass this confirmation boundary.

## IPC (`ipc/`)

`IpcProtocol.*` defines a transport-independent native frame codec. Its header is exactly 24 bytes and is encoded field-by-field in little endian: `AMEM` magic, exact `1.0` version, message type, zero flags, request id, and payload length. It supports `Hello`, `HelloAck`, `Request`, `Response`, `Cancel`, and `Error`; handshake ids must be zero, request/response/cancel ids must be non-zero. Requests are capped at 1 MiB, all other frames at 4 MiB, and payloads must be valid UTF-8. Invalid headers are rejected before payload allocation; partial frames return `NeedMoreData` without consuming input.

`IpcFramedConnection` performs overlapped exact reads/writes with absolute deadlines and the server stop event. It reads and validates the 24-byte header through `DecodeHeader()` before allocating payload storage, treats truncated frames as protocol errors, loops over partial writes, and reports complete/closed/cancelled/timed-out/protocol/I/O outcomes.

`NamedPipeServer.*` and `NativePipeSecurity.*` provide a serial transport foundation for `\\.\pipe\AMem.NativeAgent.v1`. The protected DACL grants pipe read/write only to the current process user SID and SYSTEM. The server uses `PIPE_REJECT_REMOTE_CLIENTS`, `FILE_FLAG_FIRST_PIPE_INSTANCE`, one reusable instance, overlapped accept, a stop event plus `CancelIoEx`, and one joinable server/handler thread. `snapshot()` exposes stopped/listening/connected/stopping/failed state, accepted count, pipe name, and last error.

`IpcHandshakeSession` requires the first frame to be a maximum 16 KiB `Hello` JSON. `client_name` is required and bounded, `client_version` is optional and bounded, and `requested_capabilities` accepts each known capability once. `HelloAck` grants only requested `Observe`; requested `TargetSelection`, `TargetMutation`, and `HostExecution` capabilities are denied. Unsupported header versions/oversized frames close without a response; wrong first types or invalid JSON/schema receive a bounded structured `Error` before close. An established caller must keep the handler alive for the next session stage.

`IpcRequestProtocol` requires Request JSON `{ "method": string, "params": object, "timeout_ms"?: uint }`, rejects unknown fields, bounds method names to 128 bytes, defaults timeout to 30 seconds, and caps it at 5 minutes. Cancel payload is exactly `{}`. Response payloads use stable `ok`, `completion`, and `result`/`error` fields and are validated before bounded serialization.

`IpcRequestSession` persists after `HelloAck`, permits one active request, remembers at most 1024 unique request ids for the connection lifetime, and requires reconnect after that limit. A server-owned dispatcher resolves each method's required capability; the client cannot self-assert it. The caller thread keeps reading Request/Cancel frames while one owned joinable worker dispatches, so client Cancel, absolute server deadline, pipe Stop, invalid/duplicate ids, and session shutdown have explicit outcomes. Cancellation remains cooperative; dispatchers must observe the supplied context, and a sent device operation is not thereby retracted.

`IpcMethodCatalog` contains the same 24 canonical names as the in-app registry: 12 Observe, 1 TargetSelection, 9 TargetMutation, and 2 HostExecution; target policies are 3 None, 20 Bound, and 1 Selection. Only the 12 Observe entries are executable without approval. `IpcMemServiceDispatcher` reuses `MemJsonTools`, captures `{connectionGeneration, target}` once per external session, checks it before/after dispatch and at 250 ms reader intervals, and propagates the same absolute deadline and atomic cancellation token into `OperationContext`. A stale baseline emits session-level Error, cancels active work, and joins. A dispatcher without a bound broker/session still returns `approval_required`. The product runtime binds both: privileged requests submit only method/client/session/target/deadline metadata, consume one durable grant, then parse and execute the active request. Eleven privileged methods use `MemJsonTools`/`MemService`; `lua_execute` uses an injected `IIpcHostMethodExecutor` backed by shared `Mem::executeLuaJson()` when LuaJIT is available. Cancellation after consume is checked before the adapter/send boundary.

`NativeAgentRuntime` owns the `NamedPipeServer` and composes each accepted handle as framed connection -> handshake -> `IpcMemServiceDispatcher` -> request session. Each successful Hello receives a server-assigned monotonic session id; the snapshot exposes current/last ids alongside lifecycle/client identity/capability/status/counters and clears active client identity after completion. Session ids are not reset across stop/start. The system approval audit outlives the broker, host executor, and runtime; every established session exit cancels approvals bound to that exact id. Request params and results are not retained by runtime/broker/audit/GUI. Stop joins the server handler and request worker. `ENABLE_NATIVE_IPC` defaults OFF; compiled builds still start with no listener and require a user click to enable `Observe + per-request approval`. Hello never grants a standing privileged capability.

`IpcApprovalBroker` defines the privileged authorization core. It derives capability and target policy from `IpcMethodCatalog`, rejects Observe/unknown/unbounded submissions, stores no params/results, bounds live/history records, and permits only pending -> approved -> consumed or terminal deny/invalidate/expire/cancel transitions. `consume()` revalidates session, request, deadline, connection generation, and target under the broker mutex. It burns the record before the unlocked audit call and returns a one-shot grant only if the consumed transition is durable; audit failure returns `approval_audit_failed`, leaves the record consumed, and cannot be retried. A concurrent session Cancel and consume have one terminal winner. After consume, `IpcMemServiceDispatcher` synchronously records one `execution_outcome` with authorized/observed snapshots, success, completion, and bounded error code. Post-effect audit failure remains visible but must not rewrite a confirmed or uncertain device receipt. Static gates require dispatcher consumption, grant-bound execution/outcome audit, a shared Lua target recheck, and Observe-only Hello.

## Removed Python MCP Proxy

The FastMCP package, legacy HTTP server, `.mcp.json`, install metadata, and IDE configurations have been removed. Do not restore a Python proxy or unauthenticated HTTP control path. The optional `tools/protocol_reference/amem_client.py` script talks directly to the Android wire protocol for manual diagnostics; it is not an Agent integration or a second protocol authority.

With LuaJIT the in-app registry has 24 executable and advertised canonical definitions; without it the count is 23. Native IPC uses the same 24-name server catalog and shared adapters, but rejects `lua_execute` when the injected Lua host is unavailable. Keep catalog and feature-gate checks machine-verifiable.

Address parsing currently differs:

- In-app address fields, including raw/typed memory, scan ranges, disassembly, pointer offsets, and breakpoint addresses, require explicit `0x` strings. Retired aliases are not executable.
Native IPC reuses `MemJsonTools`, so canonical in-app and external address fields require explicit `0x` strings. Preserve that rule in schemas, prompts, examples, and tests.

## Lua

`LuaEngine` owns LuaJIT. Bindings are in:

- `LuaAPI.cpp`
- `LuaAPI_Memory.cpp`
- `LuaAPI_ImGui.cpp`
- `LuaAPI_Assembly.cpp`

Lua is reachable from GUI windows, the in-app AI tool, and IPC. Treat arbitrary Lua as privileged write/stateful execution.
`LuaEngine::Initialize()` requires an injected `IMemService`. The only in-app Lua name is `lua_execute`, registered only with `HAVE_LUAJIT`; retired `execute_lua` session calls are preserved only as inert history text. Agent and Native IPC execution bind their approved `OperationContext` for the complete Lua call, while GUI scripts capture the current context per API call. Its timeout is bounded by the Agent task deadline, and cancellation after execution starts is only reported on the receipt, not treated as a hard stop.

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
19. Keep native IPC compile-time default-off and runtime default-stopped; only explicit GUI action may enable `Observe + per-request approval`. Hello must not grant standing privileged capability; every privileged execution requires a fresh target-bound, durable one-shot grant. All handlers and workers stay owned and joinable.
20. Validate native frame headers before allocation; handshake grants only Observe, request capabilities/target policy come from the shared catalog, session baselines are generation-bound, and cancellation remains cooperative.
21. Treat IPC approval as a one-shot, target-bound authorization: never retain raw params/results in broker records, revalidate session/request/deadline/generation/target at consume/send, return a grant only after durable consumed audit, and never equate an approved record with execution.
22. Keep GUI and Lua device operations on injected `IMemService`; only composition roots may obtain the system service. Bind Agent/IPC Lua to the caller's complete `OperationContext`, and let only `AppContext::TargetMutation` publish target state.

## Branches

- `dev`: remote default/main integration branch.
- `NativeAgent`: independent native in-app Agent branch. It is a separate product line and is not currently intended to merge into `dev`; follow `docs/native_agent_refactor_plan.md`.
- `AIChat`: AI Chat + MCP/IPC baseline branch.
- `WinGui`: earlier Windows GUI branch.
- `docking`: earlier development branch.

When changing Agent architecture or safety semantics, update `AGENTS.md`, `CLAUDE.md`, and the relevant `docs/agent_*.md` files in the same change.
