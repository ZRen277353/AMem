# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

AMem is a Windows desktop application for remote Android memory debugging, similar to Cheat Engine. It connects to an Android device over Socket and provides memory scanning, hardware breakpoint debugging, a hex memory viewer, value freezing, ELF symbol resolution, and Lua scripting. The UI is built with Dear ImGui (docking branch) rendered via DirectX 12.

On top of the GUI it ships an **in-app AI chat agent** (multi-provider: Claude / OpenAI / DeepSeek) that drives the debugger through native tool calls. The Python MCP proxy and unauthenticated loopback HTTP IPC server have been removed. Native framing, secured transport, strict Hello/request sessions, a complete method catalog, an owned runtime, explicit control, and a bounded privileged approval state machine exist under `ipc/` and `gui/`. Hello grants only `Observe`; privileged methods use a fresh per-request GUI approval instead of a standing capability. Broker `consume()` burns authorization and withholds the grant unless the consumed transition is durable. The dispatcher executes approved requests through 11 shared `MemJsonTools`/`MemService` adapters or the injected Lua host boundary, with `process_open` as a controlled baseline transition. The same bounded plaintext JSONL now distinguishes approval transitions and final execution outcomes without params, result JSON, or error messages. `ENABLE_NATIVE_IPC` defaults OFF; compiled builds start with the pipe stopped.

The `NativeAgent` branch is migrating all front ends to one native `MemService`. Legacy HTTP IPC is deleted; optional external automation uses the default-off Native Named Pipe adapter. Treat `docs/native_agent_refactor_plan.md` as the target design; the existing architecture documents describe the current code after each landed phase.

Language: C++17. Platform: Windows 10/11 x64 only. `tools/protocol_reference/amem_client.py` is an optional standard-library diagnostic probe, not a build/runtime dependency.

## Build Commands

```bash
# Configure (from repo root, using Clang + Ninja)
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=TRUE \
  -DCMAKE_C_COMPILER="C:/Program Files/LLVM/bin/clang.exe" \
  -DCMAKE_CXX_COMPILER="C:/Program Files/LLVM/bin/clang++.exe" \
  --no-warn-unused-cli -S . -B build -G Ninja

# Build
cmake --build build

# Run native no-device tests
ctest --test-dir build --output-on-failure
```

Output binary: `bin/ImGuiProject.exe`. The project can also be opened directly in Visual Studio via CMakeLists.txt (select x64-Release or x64-Debug).

For isolated validation, override `CMAKE_RUNTIME_OUTPUT_DIRECTORY`, `CMAKE_LIBRARY_OUTPUT_DIRECTORY`, and `CMAKE_ARCHIVE_OUTPUT_DIRECTORY`; source-tree `bin/` and `lib/` remain the defaults.

`native_agent_mem_service` contains 31 no-device C++ groups, including the ToolExecutor schema matrix, AgentRunner approval combinations, and tool-history pairing; `native_app_context_state` separately covers transactional target publication and presentation-cache invalidation. Native IPC adds 6 security-audit, 12 approval-broker, 5 protocol, 5 transport, 8 framed-I/O, 8 handshake, 7 request-contract, 9 request-session, 4 method-catalog, 15 dispatcher, and 10 runtime groups. Socket coverage adds 4 client-transport and 6 multi-port manager groups for real Winsock loopback, scripted partial I/O/failures, all-port rollback, request/disconnect exclusion, and reconnect isolation. Provider/SSE coverage has 19 groups, local provider HTTPS/full-response coverage has 6, context budgeting has 5, provider trust has 3, persistence recovery/limits/session protection/settings-draft/atomic-install/JSON-structure coverage has 13, and HTTP lifecycle/response limits have 5. The total suite now has thirty CTests, including nine static gates. `native_gui_mem_service_boundary` checks GUI/Lua/main protocol isolation, explicit Lua context binding, single-owner target publication, and transactional process switching; `native_context_budget_gate` fixes dispatch ordering, user/provider limits, and provider output-token bounds; `native_provider_trust_gate` fixes the official OpenAI default and both send-boundary trust checks; `native_session_protection_gate` fixes DPAPI session/index wiring and startup migration. Local self-signed HTTPS tests use the real cpp-httplib/OpenSSL transport and verify certificate rejection; external production-provider interoperability, GUI approval clicks, real Android transport, and device paths still need coverage.

## Dependencies

- **Required**: Visual Studio 2022 (C++17), CMake 3.16+, DirectX 12 SDK, Windows SDK
- **Required**: LuaJIT — must be placed in `third_party/LuaJIT/` with `include/` and `lib/lua51.lib` (a `FATAL_ERROR` otherwise)
- **Required AI chat**: `third_party/httplib/httplib.h` and `third_party/nlohmann/json.hpp` are vendored in-tree; **OpenSSL** is required for HTTPS via `vcpkg install openssl:x64-windows-static` (static `/MT`; linked into the exe, no DLL shipped). `NativeAgent` has no supported AI-off build, and missing dependencies fail configuration.
- **Required Capstone**: disassembly, static `/MT` only. Use the official installer at `capstone_ROOT` (default `C:/Program Files/capstone`, 6.x) or `vcpkg install capstone[core,arm,arm64,x86,mips]:x64-windows-static` (5.x). Do NOT use the dynamic `x64-windows` triplet.
- **Required Keystone**: assembly-to-machine-code, static `/MT`; use `vcpkg install keystone:x64-windows-static` or set `keystone_ROOT`.
- **Static single-exe distribution**: the whole app links `/MT` (static CRT via `CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded` + CMP0091) plus static capstone/keystone/OpenSSL/LuaJIT. **This is a hard constraint**: the prebuilt `lua51.lib` and official `capstone.lib` are both `/MT` (LIBCMT), so the exe must be `/MT` to avoid CRT conflicts. Result: `bin/ImGuiProject.exe` (~34MB) is fully self-contained — no capstone/keystone/OpenSSL DLLs, no VC runtime DLLs, no VC++ Redistributable needed; only Windows 10/11 x64 system DLLs. Copy the exe alone to another machine and it runs.

CMake options: `USE_DX12` (default ON), `USE_DX11` (OFF), `ENABLE_NATIVE_IPC` (default OFF, compile-only), `LUAJIT_STATIC` (default ON).

Supported product builds always define `HAVE_AI_CHAT`, `HAVE_CAPSTONE`, and `HAVE_KEYSTONE`; these are implementation macros, not OFF switches. `HAVE_NATIVE_IPC` remains opt-in. AI chat additionally defines `CPPHTTPLIB_OPENSSL_SUPPORT` and links `OpenSSL::SSL OpenSSL::Crypto crypt32` (crypt32 for DPAPI key encryption).

## Architecture

### The command pipeline — the unifying idea

The free functions declared in `socket/client_singleton.h` (`ReadProcessMemoryBytes`, `WriteProcessMemoryBytes`, `ScanValueWithProgress`, `ScanNextValueWithProgress`, `SetKernelBreakpoint`, `ResolveModuleOffsetChain`, `SymbolFind`, …) are the **single source of truth** for the Android device protocol. Product front ends do not call them directly. Current callers flow through:

1. **GUI windows** receive an explicit `IMemService&` from `Gui.cpp`/`CEWindow`.
2. **In-app AI agent** and **Lua** use `MemJsonTools`/the injected Lua service boundary.
3. An explicit `ENABLE_NATIVE_IPC=ON` build includes the secured Named Pipe runtime; it starts stopped and requires a GUI action.

```
GUI windows ────────┐
in-app AI  ─────────┼──▶ MemService / socket command layer ──▶ WinSocketClientMgr ──▶ Android
native Named Pipe ──┘
```

**Practical consequence:** adding a new device capability usually means (a) add the socket command in `socket/*Commands.cpp` + declare it in `client_singleton.h`, then (b) expose it through `MemService` to the GUI, canonical AI catalog, and/or native IPC catalog.

### Entry point & rendering

- `main.cpp` — creates the Win32 window, drives the render loop (`Gui::mainLoop()`), installs `ExceptionHandler.h` (crash dumps), and performs joined shutdown. It has no HTTP IPC listener.
- `renderer/DX12Renderer` — DirectX 12 device/swapchain/frame management (extracted out of `main.cpp`).
- `renderer/StyleSetup` — ImGui style + Chinese font loading.

### GUI layer (`gui/`)

- `Window` — base class for all windows (`pOpen`, `name`, virtual `onDraw()`, `draw()` wraps ImGui Begin/End).
- `Gui` namespace (`Gui.h/cpp`) — owns the `std::list<std::unique_ptr<Window>>`, iterates/draws them, `Gui::addWindow()`, `Gui::log()`.
- `CEWindow` — main window (menu bar, process selection); the hub that opens child windows. `windowlist.h` is the single include aggregating window headers.
- **Large windows delegate to panel classes** rather than holding all their logic:
  - `gui/scan/` — `ScanPanel`, `ScanResultsPanel`, `AddressListPanel`, `ScanEngine` (behind `ScanWindow`)
  - `gui/memview/` — `HexEditorPanel`, `DisassemblyPanel`, `StructAnalyzerPanel`, `WatchListPanel` (behind `MemoryViewerWindow`)
  - `gui/breakpoint/` — `BreakpointListPanel`, `BreakpointDetailPanel` (behind `BreakpointWindow`)
- `ColorScheme.h` — centralized `ImVec4` color constants; UI code references these instead of hardcoding.
- `ValueFormatter` — formats raw memory values per data type. `MemoryTypes.h` defines the scan flag bits and memory-region enums (see Key Patterns).
- `ConfigManager` — persists user settings.
- `DisassemblyHelper` (Capstone-gated) / `AssemblyHelper` (Keystone-gated) — ARM64 disassembly / assembly.

### Shared state & cross-window events

- `gui/AppContext` (`AppContext::Get()`) — target/presentation-state owner for `selectedPid`, `processHandle`, odd/even `processRevision`, thread-safe selected name, and module/symbol presentation cache. Only movable RAII `TargetMutation` publishes these fields and invalidates caches. `SystemMemBackend::openProcess()` owns the request lease, target mutation, DEBUG cleanup, MAIN cleanup/close/open transaction, and confirmed publication. GUI symbol cache misses call the injected `IMemService::loadSymbolTable()` and recheck target/module before installation.
- `gui/EventBus` (`EventBus::Get()`) — templated publish/subscribe singleton for decoupling windows. Events live in `gui/Events.h` (`ProcessSelectedEvent`, `NavigateToAddressEvent`). Handler lists are heap-allocated (never destructed) to dodge static-destruction-order issues when windows unsubscribe at shutdown.

### Socket communication (`socket/`)

- `client.hpp` — `WindowsSocketClient` wraps Winsock2 through injectable `IWindowsSocketOps`; product code uses `SystemWindowsSocketOps`, and tests script partial I/O/failures without changing the public client API.
- `MultiPortClientManager` — owns the three clients and one `DeviceSession` lifecycle transition, closes all ports on setup failure/reconnect, and runs the injected target-reset callback under the exclusive lifecycle lease. `WinSocketClientMgr` delegates connection ownership to it.
- `client_singleton.h/cpp` — `WinSocketClientMgr` singleton managing three port connections (`PORT_MAIN`, `PORT_DEBUG`, `PORT_ERROR`), each with its own mutex. Declares the entire remote command surface.
- **Command implementations are split by domain**: `ProcessCommands.cpp`, `MemoryCommands.cpp`, `ScanCommands.cpp`, `BreakpointCommands.cpp`, `FreezeCommands.cpp`, `SymbolCommands.cpp`.
- `SocketCommand.h` — the modern way to issue a command: `SocketCommand::execute` / `executeNoHandle` / `executeWithResult` templates acquire a shared `DeviceSession` request lease, check the connection/handle and generation, lock the port, and run the request. Prefer these over hand-rolling the locking.
- `socket_request_manager.h` — `SocketRequestManager` serializes request/response pairs so concurrent callers don't interleave responses on a shared port.
- `socket_io_timeout.h` — `SocketIoTimeout::ScopedTimeout` applies a bounded I/O timeout for the current scope (used by long Lua/IPC calls).

### AI Chat subsystem (`gui/ai/`, mandatory `HAVE_AI_CHAT` product macro)

The whole subsystem lives in the `AI` namespace and is wired up lazily in `ChatWindow`'s constructor (idempotent `initBuiltin*` calls).

- **Providers**: `AIProvider` is the abstract interface (`sendCompletion` runs async on a background thread). Built-ins `ClaudeProvider`, `OpenAIProvider`, `DeepSeekProvider` are registered in `ProviderRegistry`. `HttpClient` wraps cpp-httplib (HTTPS via OpenSSL).
- **Agent loop**: `ChatWindow` (UI) → `AgentController` (provider lookup, request construction, async dispatch, run state) → `AgentRunner` (the model→tool→model loop, step/tool budgets, approval gating) → `AgentTaskExecutor` (bounded queue + one joinable worker) → synchronous `ToolExecutor` (thread-safe registry + validated execution). With LuaJIT the registry has **24 executable and advertised canonical names** with no hidden built-in aliases; without it the count is 23. `status`/driver/process/open, module/pointer/disassembly/symbol resolution, canonical scan/symbol/breakpoint operations, and raw/typed memory operations go through `mem/MemJsonTools`; AI retains `AgentMemTools` as an alias and native IPC reuses the same adapter. Lua remains at its host-execution boundary.
- **Tool safety**: every tool is classified `ToolSafety::ReadOnly` or `Write`. Write currently means target/host mutation and requires explicit UI approval unless `autoApproveWrites` is set. Canonical `symbol_resolve`/`symbol_list` are ReadOnly because they do not modify target memory; their internal active-table mutation is serialized and epoch-bound. Retired names such as `symbol_init`, `symbol_find`, and `resolve_symbol` are no longer registered, and the default prompt is aligned with the canonical surface.
- **Threading model**: providers and the tool worker post results to the ImGui main thread via `UIMessageQueue` (message kinds: `Token`, `Completion`, `Error`, `ToolResult`), consumed in `ChatWindow::pollMessages()`. Before a mutation/session-effect completion callback, `AgentTaskExecutor` writes its bounded redacted outcome to `AgentMutationAuditLog`, so stale run filtering cannot erase the final receipt. Never touch ImGui/run state from a worker. HTTP request threads, tool execution, and Native IPC handler/dispatch threads are owned and joinable. `HttpClient::shutdown()` rejects new requests, cancels/stops active transports, and joins through provider completion callbacks; a silent read may still take up to its configured I/O timeout because transport stop is best effort. Do not add detached work.
- **Streaming completion**: HTTP 2xx is insufficient. The shared parser exposes `[DONE]`, and `StreamTerminalTracker` requires Claude `message_start`/`message_stop` or an OpenAI-compatible valid `choices` start plus `finish_reason`/`[DONE]`. Malformed or unterminated streams return `InvalidResponse`; partial content/tool calls remain visible but never enter execution.
- **Target binding**: `AgentRunContext` captures connection generation plus `{pid, handle, processRevision}`. Approval, dequeue, and result collection validate it, and `process_open` explicitly advances a `Selection` context. Every registered process-bound tool validates again at its `MemService` boundary. `lua_execute` binds that same approved `OperationContext` for the entire script so Lua APIs cannot recapture a replacement target. Run ids only isolate UI messages.
- **Retired tool history**: `ChatSession::getMessagesForRequest()` converts any tool-call group containing one of the 33 retired names into inert assistant text with redacted arguments and recorded results. Retired calls are neither sent as provider tool protocol nor executable.
- **GUI service migration**: `Gui.cpp` obtains the system service only as the composition root and injects it through `CEWindow`, connection/process/module/version/scan/memory/breakpoint windows, Lua, and all subpanels. GUI/Lua sources have no `client_singleton.h` dependency or direct protocol calls; `main.cpp` disconnects through the service. Breakpoint hit batches preserve GPR/FPSIMD state and keep the newest 50,000 GUI entries. The Agent keeps the newest 100 and reports `available`/`dropped`; the wire protocol has no continuation cursor.
- **GUI symbol cache**: `MemoryViewerWindow` and `BreakpointWindow` pass their injected service to `AppContext::ModuleCache`. A miss loads the full symbol table with one init and one transaction across all 1000-item protocol pages, bounded to 1,000,000 symbols/64 MiB names. Direct GUI `SymbolInit`/`SymbolGetList` calls are gone.
- **GUI scan sessions**: `ScanWindow` receives `IMemService`; start/refine/result paging/clear/selected-result removal bind the captured target and latest scan epoch. Range selection plus start, count plus page, and count-confirmed removal are service transactions. GUI cancellation sets the shared token and reports a confirmed result after Stop separately from an unconfirmed completion.
- **Context budget**: `AgentController` calls `ContextBudget` before every provider dispatch. The effective window combines the global user limit, provider/model capability, optional persisted endpoint override, conservative UTF-8/message framing, all advertised tool schemas, provider overhead, and a 256..4096 output reserve. Only the request copy loses complete old conversation groups; an oversized latest group fails locally. Built-in endpoint overrides can only lower capability, while custom endpoints may explicitly replace it but stay user-capped. The estimate is conservative rather than tokenizer-exact.
- **Persistence** (all relative to the process working directory; this is only next to the exe when launched from there):
  - `ai_config.json` — provider configs and optional model/endpoint context-window override; **API keys are encrypted with Windows DPAPI** (`ApiKeyStore`, base64 over the encrypted blob). Keys are per-Windows-user and never bundled.
  - `ai_settings.json` — non-secret, hand-editable settings (`AiSettings`): the sole persistent system-prompt/token-budget owner, proxy, and other global settings. `DefaultSystemPrompt.h` seeds an AMem-specific prompt on first run.
  - `ai_sessions/<id>.json` + `ai_sessions/index.json` — binary, current-user DPAPI envelopes containing format-v2 chat/tool history and index/title metadata (`SessionManager` + `ChatSession`; max 1000 messages after load/truncation). Valid legacy plaintext is atomically migrated only after schema validation; invalid/oversized files remain unchanged and unbound.
  - `ai_mutation_audit.jsonl` + `.1` — **plaintext**, redacted mutation/session-effect summaries. Records are capped at 64 KiB; the active file is capped at 4 MiB and keeps one rotated backup. The Audit window shows the most recent valid records.
  - `native_ipc_approval_audit.jsonl` + `.1` — **plaintext**, bounded approval-state metadata with no params/results. Records are capped at 16 KiB; active/backup files are capped at 4 MiB and the Native IPC window shows recent entries and write failures.
  - Legacy migrations run once on startup: `ai_config.dat`→`ai_config.json`, `ai_session.json`→`ai_sessions/`.
  - Config, settings, index, and session loaders validate temporary state before commit and distinguish `Loaded`, `Missing`, `Recovered`, `Invalid`, and `IoError`. Only missing files seed defaults. Corrupt/I/O failures preserve the original; corrupt indexes are backed up as `.corrupt*` and rebuilt from valid session files. Valid session/index JSON is protected with a purpose-bound `AMEMAIP1` DPAPI envelope and installed atomically; failed loads are neither migrated nor rebound. Persistence and retained-message byte limits plus depth/node/container/string limits are enforced before DOM construction and install/materialization; allocation failures fail closed, although DOM overhead remains larger than serialized bytes. Legacy v1 prompt/token fields are ignored; v2 does not store them, and a successful load trims messages with the live global token budget.
  - The settings panel edits one disposable `ChatSettingsDraft`, reloaded from persistence on every open. Save alone commits; Cancel/title-bar close clear provider, prompt, numeric, and proxy edits. `Forget saved key` is staged until Save, which atomically installs the complete provider update/remove snapshot before changing the live store/provider. Owned plaintext key buffers are actively cleared on replacement and teardown, but already-sent HTTP headers and OS/allocator/clipboard copies cannot be retracted.
- **Provider trust**: `OpenAIProvider` defaults to the official `https://api.openai.com/v1`. Custom OpenAI-compatible endpoints require an explicit `Trust this endpoint` decision stored against the canonical full URL; changing the host or path invalidates it. Chat preflight and controller dispatch both fail closed before credentials are sent. HTTPS alone still does not establish that a custom recipient is the provider the user intended.

### Native IPC foundation (`ipc/IpcProtocol.*`, `ipc/IpcFramedConnection.*`, `ipc/IpcHandshakeSession.*`, `ipc/IpcRequestProtocol.*`, `ipc/IpcRequestSession.*`, `ipc/NamedPipeServer.*`, `ipc/NativeAgentRuntime.*`)

The transport-independent codec uses an explicit 24-byte little-endian header: `AMEM` magic, exact protocol version `1.0`, message type, zero flags, request id, and payload length. The message types are `Hello`, `HelloAck`, `Request`, `Response`, `Cancel`, and `Error`; handshake ids are zero and request/response/cancel ids are non-zero. Request payloads have an immutable 1 MiB ceiling, all other frames have a 4 MiB ceiling, payloads must be valid UTF-8, and incomplete frames do not consume input.

`NativePipeSecurity` creates a protected DACL granting read/write only to the current process user SID and SYSTEM. `NamedPipeServer` creates one reusable `\\.\pipe\AMem.NativeAgent.v1` instance with remote clients rejected and first-instance ownership. Accept and the serial handler share one owned thread; stop signals an event, calls `CancelIoEx`, and joins. Status snapshots expose lifecycle state, accepted count, pipe name, and error.

`IpcFramedConnection` validates the fixed header before payload allocation and performs exact overlapped reads/writes with absolute deadlines and Stop cancellation. `IpcHandshakeSession` limits Hello to 16 KiB, validates structured JSON identity/capability fields, requires exact header version `1.0`, grants only requested `Observe`, and denies any requested privileged capability. Invalid schema gets a structured Error with a bounded drain; version/length protocol failures close immediately.

`IpcRequestProtocol` fixes strict `{method, params, timeout_ms?}` Request JSON, an empty-object Cancel, bounded completion/result envelopes, a 30-second default timeout, and a 5-minute maximum. `IpcRequestSession` keeps one active request and up to 1024 lifetime request ids, rejects duplicates/busy/unknown/unauthorized work before dispatch, converts relative timeout to an absolute server deadline, and routes Cancel/Stop to one owned joinable dispatch worker. Method capability metadata belongs to the server dispatcher; cancellation is cooperative.

`IpcMethodCatalog` mirrors all 24 canonical names and fixes capability plus target policy; only 12 Observe methods are approval-free. `IpcMemServiceDispatcher` reuses `MemJsonTools`, binds the external session to its initial connection/target snapshot, polls and rechecks that baseline, and propagates deadline/cancellation into `OperationContext`. Privileged requests submit bounded metadata and wait on the owned worker while the reader remains available for Cancel. After durable consume, 11 methods execute through `MemJsonTools`/`MemService`; `lua_execute` uses the injected `IIpcHostMethodExecutor` and shared `Mem::executeLuaJson()`. Cancellation is rechecked after consume and before adapter/send, and `process_open` advances the protected baseline only when its returned snapshot is still current.

`NativeAgentRuntime` owns the server and composes framed connection -> Hello -> dispatcher -> request session for each serial client. A successful Hello gets a monotonic server session id that is not reset by stop/start. Its thread-safe snapshot contains current/last session ids, lifecycle, active client identity/capabilities, bounded status/counters, and errors, but never retained request params or results. Every established session exit cancels broker approvals for that id; Stop joins both handler and dispatch worker. `ENABLE_NATIVE_IPC` defaults OFF. When compiled, `NativeAgentIpcWindow` exposes an explicit enable/stop action and visible `Observe + per-request approval` status; the system runtime is constructed lazily, begins stopped, and is shut down before device disconnect. Handshake continues to deny every standing privileged capability.

`IpcApprovalBroker` owns authorization, not execution. Catalog metadata is server-owned; records exclude params/results and are bounded. `consume()` revalidates session/request/deadline/generation/target, burns the record before unlocked audit, and returns a grant only for a durable consumed transition. Audit failure returns `approval_audit_failed` with no grant and no reusable authorization; consume and session Cancel are linearized by the broker state. The production dispatcher consumes the grant, checks its request metadata, and synchronously emits one `execution_outcome` with authorized/observed snapshots and final completion. Outcome-audit failure is visible in the shared audit health but cannot rewrite a sent operation's real receipt. Hello remains Observe-only, so every privileged device/host call still needs its own approved grant.

### Removed Python MCP proxy

The FastMCP package, legacy HTTP server, `.mcp.json`, packaging metadata, and IDE configurations have been deleted from `NativeAgent`. Do not restore a Python proxy or unauthenticated HTTP control path. `tools/protocol_reference/amem_client.py` is a manual Android wire-protocol probe only and is not an Agent integration.

All canonical in-app and native IPC address fields use shared adapters and require explicit `0x` strings. With LuaJIT the in-app registry has 24 canonical definitions; without it the count is 23. Native IPC retains the 24-name catalog but rejects `lua_execute` when the injected Lua host is unavailable. Keep catalog and feature-gate checks machine-verifiable.

### Lua scripting (`lua/`, gated by `HAVE_LUAJIT`)

`LuaEngine` (singleton) owns the LuaJIT state and requires an injected `IMemService`. `LuaAPI*.cpp` expose C++ bindings: `LuaAPI_Memory` (memory/scan/breakpoints), `LuaAPI_ImGui` (drawing), `LuaAPI_Assembly`. Lua is reachable from the GUI (`LuaScriptWindow`, `LuaImGuiWindow`) and from the feature-gated canonical `lua_execute` tool used by the in-app Agent and native IPC host boundary. Retired `execute_lua` calls survive only as inert session history. Agent/IPC Lua execution stores the approved `OperationContext` in the Lua registry for the full call; GUI scripts use current service context. Execution cannot extend the absolute task deadline and cannot be retracted after it starts.

### Version system

`version.h.in` → CMake generates `build/generated/version.h` with project version (1.0.0), protocol version (1.1.0, negotiated with the device), git hash, and build timestamp.

## Key Patterns

- **Singletons remain common (Meyer's)**: `WinSocketClientMgr`, `SocketRequestManager`, `LuaEngine`, `AppContext`, `EventBus`, and most AI components (`ProviderRegistry`, `ToolExecutor`, `ApiKeyStore`, `AiSettings`, `SessionManager`, `UIMessageQueue`) use `static` local in `GetInstance()`/`Get()`.
- **Socket thread safety**: never issue a raw send/receive pair without the normal `SocketCommand::execute*` path. It holds the connection lease, recursive per-port transaction gate, and request-response mutex; responses from concurrent callers will interleave if the last lock is bypassed.
- **Single-command locking is not a transaction**: scan, symbol, pointer, breakpoint cleanup, and process switching are multi-command shared-state sequences. Their current `MemService` implementations own the required transaction/revision/epoch; every new compound operation needs the same treatment.
- **Pointer transaction**: `pointer_resolve` holds `SocketCommand::TransactionLease` across module lookup and every dereference, then validates the full target after releasing it. Do not acquire the AppContext process-state mutex while the transaction gate is held.
- **Scan session**: every legacy/native scan mutation advances a monotonic epoch. Canonical start owns range+scan in one transaction; refine/results/clear require the latest epoch and reject cross-frontend replacement.
- **Symbol session**: every `SymbolInit` advances its epoch inside the transaction gate. Canonical resolve/list bind module lookup + init + find/page in one transaction; continuation pages require the latest epoch.
- **Breakpoint receipts**: set/remove/suspend/resume update the remote state and cleanup tracker in one MAIN transaction. Sent-without-response is `completion_unknown`; hit data is paged with 64-bit values encoded as strings at the Agent boundary. Disconnect resets only the local tracker.
- **Timeout poisons unframed connections**: timeout, EOF, or partial I/O poisons `DeviceSession`, closes the failed client, and advances the generation. The old pending-data drain recovery path is gone; explicitly reconnect before reuse.
- **Transport regression boundary**: `native_socket_client_transport` verifies the default Winsock loopback path, partial send/receive loops, timeout/EOF poison, stale leases, and that old endpoint bytes cannot cross into a reconnected generation. The companion manager suite owns three-port lifecycle coverage; Android-device behavior remains separate.
- **Three-port regression boundary**: `native_multi_port_client_manager` verifies real three-stream loopback connect/close, per-port failure rollback, single-port poison, full reconnect, active-request exclusion, and repeated fresh generations. Real Android server state restoration remains a device test.
- **Connection lifecycle**: commands hold a shared `DeviceSession` request lease; connect/disconnect/reconnect hold an exclusive lifecycle lease. Do not bypass this gate with direct client `Connect()`/`Close()` calls.
- **UI thread isolation for AI**: background provider/HTTP threads communicate with ImGui exclusively through `UIMessageQueue`. ImGui calls happen only on the main thread.
- **Process target consistency**: process-bound Agent operations carry an explicit generation/PID/handle/revision snapshot and must validate it at their actual service/send boundary; runId is not a target identifier.
- **Backend selection semantics**: the Android server's runtime `SysCall`/`Kernel` type only selects read/write and breakpoint implementations. It is not target identity and does not require generation/revision invalidation.
- **Module mapping identity**: module enumeration returns individual mappings. Resolution folds rows with the same case-normalized full path and selects the lowest base, while distinct paths with the same basename remain ambiguous. The shared rule covers module, pointer, and symbol resolution without changing raw `module_list` output.
- **Address format**: use `0x` for address strings across AI, IPC, docs, and tests.
- **Resource bounds**: `AiLimits.h` caps HTTP at 16 MiB, SSE line/event at 1/2 MiB, assistant/tool-result payloads at 4 MiB, ordinary messages at 8 MiB, persistence JSON at 32 MiB, and retained sessions at 16 MiB/1,000 messages (10,000 on disk). Keep validation before append/materialization; these limits do not replace pagination or provider-aware context budgeting.
- **Sensitive data**: DPAPI protects provider API keys plus usable session/index files for the current Windows user. It does not encrypt `ai_settings.json`, the redacted JSONL audits, process memory, crash dumps, or data sent to the selected provider. Continue redacting credentials before they enter history; local at-rest encryption is not a reason to persist reusable secrets.
- **Scan flags are bitmasks** (defined in `MemoryTypes.h`): exactly one data-type bit (`BYTE_`/`WORD_`/`DWORD_`/`QWORD_`/`FLOAT_`/`DOUBLE_`/`XOR_`) OR-ed with one scan-mode bit (`_ACCURATE_VAL`, `_LARGER_THAN_VAL`, `_LESS_THAN_VAL`, `_BETWEEN_VAL`, `_UNKNOW_VAL`, `_ADD_UNKNOW_VAL`, `_SUB_UNKNOW_VAL`, `_CHANGED_VAL`, `_UNCHANGED_VAL`, …). The native Agent and temporary IPC validate combinations independently; new work belongs in the shared service contract rather than another constants mirror.
- **Conditional compilation**: AI/Capstone/Keystone source retains `HAVE_*` guards as implementation boundaries, but the supported `NativeAgent` product always defines all three and CMake no longer supports a subset build. `HAVE_NATIVE_IPC` remains an actual opt-in feature gate.
- **ImGui docking**: uses the docking branch; windows use `ImGuiWindowFlags_NoDocking` selectively.

## Branches

- `dev` — remote default/main integration branch
- `NativeAgent` — independent native in-app Agent branch; not currently intended to merge into `dev`
- `AIChat` — AI chat + MCP/IPC baseline branch
- `WinGui` — earlier Windows GUI branch
- `docking` — earlier development branch

## Note for maintainers

`AGENTS.md` (guidance for Codex) historically mirrored this file's content. If you make substantive architecture changes, update both plus the relevant `docs/agent_*.md` files. The detailed current risk register is `docs/agent_project_issues.md`.
