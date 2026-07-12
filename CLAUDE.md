# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

AMem is a Windows desktop application for remote Android memory debugging, similar to Cheat Engine. It connects to an Android device over Socket and provides memory scanning, hardware breakpoint debugging, a hex memory viewer, value freezing, ELF symbol resolution, and Lua scripting. The UI is built with Dear ImGui (docking branch) rendered via DirectX 12.

On top of the GUI it ships an **in-app AI chat agent** (multi-provider: Claude / OpenAI / DeepSeek) that drives the debugger through native tool calls. The Python MCP proxy has been removed. Default builds exclude the loopback HTTP IPC server; `ENABLE_LEGACY_HTTP_IPC=ON` restores that unsafe endpoint only for migration diagnostics. Native framing, secured transport, strict Hello/request sessions, a complete method catalog, 12 Observe `MemService` adapters, an owned runtime, and explicit status/control exist under `ipc/` and `gui/`. `ENABLE_NATIVE_IPC` defaults OFF; compiled builds still start with the pipe stopped and require a user click to enable Observe. Privileged approval and a privileged external Agent adapter do not exist yet.

The `NativeAgent` branch is migrating all front ends to one native `MemService` and replacing or deleting HTTP IPC. Treat `docs/native_agent_refactor_plan.md` as the target design; the existing architecture documents describe the current code after each landed phase.

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

`native_agent_mem_service` contains 23 no-device C++ groups. Native IPC adds 5 protocol, 5 transport, 7 framed-I/O, 8 handshake, 6 request-contract, 9 request-session, 4 method-catalog, 5 `MemService` dispatcher, and 7 runtime groups; the total suite has fourteen CTests. Coverage includes framing/transport, Hello, strict JSON, ID/session bounds, catalog classification, Observe dispatch, error mapping, connection/target invalidation, sequential sessions, restart, cooperative deadline/client/Stop cancellation, and joined shutdown. Provider, native GUI/privileged approval, opt-in legacy IPC, real Android transport, and device paths still need coverage. For manual wire-protocol checks, use `tools/protocol_reference/amem_client.py` or dump the live Lua API surface with `scripts/dump_api.lua` (see `scripts/README.md`).

## Dependencies

- **Required**: Visual Studio 2022 (C++17), CMake 3.16+, DirectX 12 SDK, Windows SDK
- **Required**: LuaJIT — must be placed in `third_party/LuaJIT/` with `include/` and `lib/lua51.lib` (a `FATAL_ERROR` otherwise)
- **AI chat** (`ENABLE_AI_CHAT`, default ON): `third_party/httplib/httplib.h` and `third_party/nlohmann/json.hpp` are vendored in-tree; additionally requires **OpenSSL** for HTTPS via `vcpkg install openssl:x64-windows-static` (static `/MT`; linked into the exe, no DLL shipped). If OpenSSL is missing, AI chat is silently disabled but the rest of the app builds.
- **Optional**: Capstone — disassembly. Static `/MT` only: official installer at `capstone_ROOT` (default `C:/Program Files/capstone`, 6.x) or `vcpkg install capstone[core,arm,arm64,x86,mips]:x64-windows-static` (5.x). Linked into the exe. Do NOT use the dynamic `x64-windows` triplet — its `/MD` import lib conflicts with the `/MT` build.
- **Optional**: Keystone — assembly-to-machine-code. Static `/MT`: `vcpkg install keystone:x64-windows-static` (or set `keystone_ROOT`). Linked into the exe.
- **Static single-exe distribution**: the whole app links `/MT` (static CRT via `CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded` + CMP0091) plus static capstone/keystone/OpenSSL/LuaJIT. **This is a hard constraint**: the prebuilt `lua51.lib` and official `capstone.lib` are both `/MT` (LIBCMT), so the exe must be `/MT` to avoid CRT conflicts. Result: `bin/ImGuiProject.exe` (~34MB) is fully self-contained — no capstone/keystone/OpenSSL DLLs, no VC runtime DLLs, no VC++ Redistributable needed; only Windows 10/11 x64 system DLLs. Copy the exe alone to another machine and it runs.

CMake options: `USE_DX12` (default ON), `USE_DX11` (OFF), `ENABLE_AI_CHAT` (default ON), `ENABLE_LEGACY_HTTP_IPC` (default OFF), `ENABLE_NATIVE_IPC` (default OFF, compile-only), `LUAJIT_STATIC` (default ON).

Feature gates resolved by CMake: `HAVE_AI_CHAT`, `HAVE_LEGACY_HTTP_IPC`, `HAVE_NATIVE_IPC`, `HAVE_CAPSTONE`, `HAVE_KEYSTONE`, `HAVE_LUAJIT`. AI chat additionally defines `CPPHTTPLIB_OPENSSL_SUPPORT` and links `OpenSSL::SSL OpenSSL::Crypto crypt32` (crypt32 for DPAPI key encryption).

## Architecture

### The command pipeline — the unifying idea

The free functions declared in `socket/client_singleton.h` (`ReadProcessMemoryBytes`, `WriteProcessMemoryBytes`, `ScanValueWithProgress`, `ScanNextValueWithProgress`, `SetKernelBreakpoint`, `ResolveModuleOffsetChain`, `SymbolFind`, …) are the **single source of truth** for the Android device protocol. Everything else is a front-end onto them. Current callers are:

1. **GUI windows** (`gui/`) call them directly in response to user actions.
2. **In-app AI agent** (`gui/ai/ToolDefinitions.cpp`) wraps them as tool executors registered with `ToolExecutor`.
3. An explicit `ENABLE_LEGACY_HTTP_IPC=ON` build includes **HTTP IPC** (`ipc/IpcServer.cpp`), whose legacy JSON handlers call the same functions without in-app Agent approval.

```
GUI windows ─────┐
in-app AI  ──────┼──▶ MemService / socket command layer ──▶ WinSocketClientMgr ──▶ Android
opt-in HTTP IPC ──┘
```

**Practical consequence:** adding a new device capability usually means (a) add the socket command in `socket/*Commands.cpp` + declare it in `client_singleton.h`, then (b) expose it through `MemService` to the GUI and/or canonical AI catalog. Do not expand the legacy HTTP IPC while its replacement decision is pending.

### Entry point & rendering

- `main.cpp` — creates the Win32 window, drives the render loop (`Gui::mainLoop()`), installs `ExceptionHandler.h` (crash dumps), and starts port 28100 only behind `HAVE_LEGACY_HTTP_IPC`.
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

- `gui/AppContext` (`AppContext::Get()`) — singleton holding global process state (`selectedPid`, `processHandle`, `processRevision`, thread-safe selected name) and a `moduleCache` (module list + per-module ELF symbol presentation cache). GUI symbol cache misses call the injected `IMemService::loadSymbolTable()`; the service owns init/fetch transaction and target validation, while cache installation rechecks target/module under the cache mutex. The GUI, AI tools, and IPC server still share the underlying process state.
- `gui/EventBus` (`EventBus::Get()`) — templated publish/subscribe singleton for decoupling windows. Events live in `gui/Events.h` (`ProcessSelectedEvent`, `NavigateToAddressEvent`). Handler lists are heap-allocated (never destructed) to dodge static-destruction-order issues when windows unsubscribe at shutdown.

### Socket communication (`socket/`)

- `client.hpp` — `WindowsSocketClient` wrapping Winsock2 send/receive.
- `client_singleton.h/cpp` — `WinSocketClientMgr` singleton managing three port connections (`PORT_MAIN`, `PORT_DEBUG`, `PORT_ERROR`), each with its own mutex. Declares the entire remote command surface.
- **Command implementations are split by domain**: `ProcessCommands.cpp`, `MemoryCommands.cpp`, `ScanCommands.cpp`, `BreakpointCommands.cpp`, `FreezeCommands.cpp`, `SymbolCommands.cpp`.
- `SocketCommand.h` — the modern way to issue a command: `SocketCommand::execute` / `executeNoHandle` / `executeWithResult` templates acquire a shared `DeviceSession` request lease, check the connection/handle and generation, lock the port, and run the request. Prefer these over hand-rolling the locking.
- `socket_request_manager.h` — `SocketRequestManager` serializes request/response pairs so concurrent callers don't interleave responses on a shared port.
- `socket_io_timeout.h` — `SocketIoTimeout::ScopedTimeout` applies a bounded I/O timeout for the current scope (used by long Lua/IPC calls).

### AI Chat subsystem (`gui/ai/`, gated by `HAVE_AI_CHAT`)

The whole subsystem lives in the `AI` namespace and is wired up lazily in `ChatWindow`'s constructor (idempotent `initBuiltin*` calls).

- **Providers**: `AIProvider` is the abstract interface (`sendCompletion` runs async on a background thread). Built-ins `ClaudeProvider`, `OpenAIProvider`, `DeepSeekProvider` are registered in `ProviderRegistry`. `HttpClient` wraps cpp-httplib (HTTPS via OpenSSL).
- **Agent loop**: `ChatWindow` (UI) → `AgentController` (provider lookup, request construction, async dispatch, run state) → `AgentRunner` (the model→tool→model loop, step/tool budgets, approval gating) → `AgentTaskExecutor` (bounded queue + one joinable worker) → synchronous `ToolExecutor` (thread-safe registry + validated execution). With LuaJIT the registry has **24 executable and advertised canonical names** with no hidden built-in aliases; without it the count is 23. `status`/driver/process/open, module/pointer/disassembly/symbol resolution, canonical scan/symbol/breakpoint operations, and raw/typed memory operations go through `mem/MemJsonTools`; AI retains `AgentMemTools` as an alias and native IPC reuses the same adapter. Lua remains at its host-execution boundary.
- **Tool safety**: every tool is classified `ToolSafety::ReadOnly` or `Write`. Write currently means target/host mutation and requires explicit UI approval unless `autoApproveWrites` is set. Canonical `symbol_resolve`/`symbol_list` are ReadOnly because they do not modify target memory; their internal active-table mutation is serialized and epoch-bound. Retired names such as `symbol_init`, `symbol_find`, and `resolve_symbol` are no longer registered, and the default prompt is aligned with the canonical surface.
- **Threading model**: providers and the tool worker post results to the ImGui main thread via `UIMessageQueue` (message kinds: `Token`, `Completion`, `Error`, `ToolResult`), consumed in `ChatWindow::pollMessages()`. Before a mutation/session-effect completion callback, `AgentTaskExecutor` writes its bounded redacted outcome to `AgentMutationAuditLog`, so stale run filtering cannot erase the final receipt. Never touch ImGui/run state from a worker. Tool execution is owned and joinable; HTTP workers and IPC handlers are still detached, so application-wide teardown is not fully closed. Do not add more detached work.
- **Streaming completion**: HTTP 2xx is insufficient. Claude/DeepSeek set terminal state but never validate it, and OpenAI does not track it, so a truncated SSE stream can currently be committed as success. New provider work must require a legal terminal before tool execution.
- **Target binding**: `AgentRunContext` captures connection generation plus `{pid, handle, processRevision}`. Approval, dequeue, and result collection validate it, and `process_open` explicitly advances a `Selection` context. Every registered process-bound tool validates again at its `MemService` or Lua host boundary. Run ids only isolate UI messages.
- **Retired tool history**: `ChatSession::getMessagesForRequest()` converts any tool-call group containing one of the 33 retired names into inert assistant text with redacted arguments and recorded results. Retired calls are neither sent as provider tool protocol nor executable.
- **GUI service migration**: `BreakpointWindow` is constructed with `IMemService`; set/remove/enable/suspend/resume and hit refresh consume a fresh target-bound context. Hit batches preserve GPR/FPSIMD state and keep the newest 50,000 GUI entries. The Agent keeps the newest 100 and reports `available`/`dropped`; the wire protocol has no continuation cursor.
- **GUI symbol cache**: `MemoryViewerWindow` and `BreakpointWindow` pass their injected service to `AppContext::ModuleCache`. A miss loads the full symbol table with one init and one transaction across all 1000-item protocol pages, bounded to 1,000,000 symbols/64 MiB names. Direct GUI `SymbolInit`/`SymbolGetList` calls are gone.
- **GUI scan sessions**: `ScanWindow` receives `IMemService`; start/refine/result paging/clear/selected-result removal bind the captured target and latest scan epoch. Range selection plus start, count plus page, and count-confirmed removal are service transactions. GUI cancellation sets the shared token and reports a confirmed result after Stop separately from an unconfirmed completion.
- **Context budget**: `ProviderCapabilities::maxContextTokens` is currently unused. The byte-count heuristic omits tool schemas and output reserve; `tokenLimit` is not a guarantee that a request fits the active model.
- **Persistence** (all relative to the process working directory; this is only next to the exe when launched from there):
  - `ai_config.json` — provider configs; **API keys are encrypted with Windows DPAPI** (`ApiKeyStore`, base64 over the encrypted blob). Keys are per-Windows-user and never bundled.
  - `ai_settings.json` — non-secret, hand-editable settings (`AiSettings`): system prompt, proxy, etc. `DefaultSystemPrompt.h` seeds an AMem-specific prompt on first run.
  - `ai_sessions/<id>.json` + `ai_sessions/index.json` — **plaintext** chat/tool history (`SessionManager` + `ChatSession`; max 1000 messages after load/truncation). Driver card fields are redacted before display/audit/persistence, but files can still contain Lua, addresses, memory data, and other complete tool arguments/results.
  - `ai_mutation_audit.jsonl` + `.1` — **plaintext**, redacted mutation/session-effect summaries. Records are capped at 64 KiB; the active file is capped at 4 MiB and keeps one rotated backup. The Audit window shows the most recent valid records.
  - Legacy migrations run once on startup: `ai_config.dat`→`ai_config.json`, `ai_session.json`→`ai_sessions/`.
- **Provider trust**: `OpenAIProvider` currently defaults to `https://ai.ikik.net/v1`, a third-party OpenAI-compatible gateway. HTTPS alone does not establish that the recipient is the provider the user intended.

### Native IPC foundation (`ipc/IpcProtocol.*`, `ipc/IpcFramedConnection.*`, `ipc/IpcHandshakeSession.*`, `ipc/IpcRequestProtocol.*`, `ipc/IpcRequestSession.*`, `ipc/NamedPipeServer.*`, `ipc/NativeAgentRuntime.*`)

The transport-independent codec uses an explicit 24-byte little-endian header: `AMEM` magic, exact protocol version `1.0`, message type, zero flags, request id, and payload length. The message types are `Hello`, `HelloAck`, `Request`, `Response`, `Cancel`, and `Error`; handshake ids are zero and request/response/cancel ids are non-zero. Request payloads have an immutable 1 MiB ceiling, all other frames have a 4 MiB ceiling, payloads must be valid UTF-8, and incomplete frames do not consume input.

`NativePipeSecurity` creates a protected DACL granting read/write only to the current process user SID and SYSTEM. `NamedPipeServer` creates one reusable `\\.\pipe\AMem.NativeAgent.v1` instance with remote clients rejected and first-instance ownership. Accept and the serial handler share one owned thread; stop signals an event, calls `CancelIoEx`, and joins. Status snapshots expose lifecycle state, accepted count, pipe name, and error.

`IpcFramedConnection` validates the fixed header before payload allocation and performs exact overlapped reads/writes with absolute deadlines and Stop cancellation. `IpcHandshakeSession` limits Hello to 16 KiB, validates structured JSON identity/capability fields, requires exact header version `1.0`, grants only requested `Observe`, and denies any requested privileged capability. Invalid schema gets a structured Error with a bounded drain; version/length protocol failures close immediately.

`IpcRequestProtocol` fixes strict `{method, params, timeout_ms?}` Request JSON, an empty-object Cancel, bounded completion/result envelopes, a 30-second default timeout, and a 5-minute maximum. `IpcRequestSession` keeps one active request and up to 1024 lifetime request ids, rejects duplicates/busy/unknown/unauthorized work before dispatch, converts relative timeout to an absolute server deadline, and routes Cancel/Stop to one owned joinable dispatch worker. Method capability metadata belongs to the server dispatcher; cancellation is cooperative.

`IpcMethodCatalog` mirrors all 24 canonical names and fixes capability plus target policy; only 12 Observe methods are approval-free. `IpcMemServiceDispatcher` reuses `MemJsonTools`, binds the external session to its initial connection/target snapshot, polls and rechecks that baseline, propagates deadline/cancellation into `OperationContext`, and rejects all privileged methods before service invocation.

`NativeAgentRuntime` owns the server and composes framed connection -> Hello -> Observe dispatcher -> request session for each serial client. Its thread-safe snapshot contains lifecycle, active client identity/capabilities, bounded status/counters, and errors, but never request params or results; Stop joins both handler and dispatch worker. `ENABLE_NATIVE_IPC` defaults OFF. When compiled, `NativeAgentIpcWindow` exposes an explicit enable/stop action and visible Observe-only status; the system runtime is constructed lazily, begins stopped, and is shut down before device disconnect. There is still no privileged approval broker, and handshake continues to deny every privileged capability.

### Legacy IPC server (`ipc/IpcServer.cpp`)

A minimal hand-rolled HTTP server (`IpcServer` singleton) bound to **127.0.0.1:28100 only**. It is excluded from default builds and starts from `main.cpp` only with `ENABLE_LEGACY_HTTP_IPC=ON`. It accepts `POST /` with body `{ "method": "...", "params": {...} }` and returns `{ "success": bool, "result"/"error": ... }`. Methods are registered in `RegisterBuiltinMethods()` and call the same socket commands as the GUI.

Loopback is not authentication. The current server has no token, allows `Access-Control-Allow-Origin: *`, accepts browser preflight, bypasses the in-app write approval path, detaches each client handler, and sends each response with one `send()` call. Do not add privileged methods without addressing authentication/capabilities, browser access, handler drainage, and partial sends.

### Removed Python MCP proxy

The FastMCP package, `.mcp.json`, packaging metadata, and IDE configurations have been deleted from `NativeAgent`. Do not restore a Python wrapper around the legacy HTTP IPC. `tools/protocol_reference/amem_client.py` is a manual Android wire-protocol probe only and is not an Agent integration.

The remaining HTTP IPC path does not pass through `AgentRunner` approval. All in-app address fields require explicit `0x` strings, while IPC still treats unprefixed strings as decimal. With LuaJIT the in-app registry has 24 canonical definitions; without it the count is 23. IPC has 29 legacy methods with different target, result, and feature-gate semantics. Keep a generated capability matrix until IPC is replaced or deleted. An HTTP client timeout does not cancel the detached C++ handler, so automatic retry can overlap an old request.

### Lua scripting (`lua/`, gated by `HAVE_LUAJIT`)

`LuaEngine` (singleton) owns the LuaJIT state. `LuaAPI*.cpp` expose C++ bindings: `LuaAPI_Memory` (memory/scan/breakpoints), `LuaAPI_ImGui` (drawing), `LuaAPI_Assembly`. Lua is reachable from the GUI (`LuaScriptWindow`, `LuaImGuiWindow`), from the feature-gated canonical AI tool `lua_execute`, and from the IPC `execute_lua` method. Retired in-app `execute_lua` calls survive only as inert session history. Agent Lua execution is target-bound, cannot extend the absolute task deadline, and cannot be retracted after it starts.

### Version system

`version.h.in` → CMake generates `build/generated/version.h` with project version (1.0.0), protocol version (1.1.0, negotiated with the device), git hash, and build timestamp.

## Key Patterns

- **Singletons everywhere (Meyer's)**: `WinSocketClientMgr`, `SocketRequestManager`, `LuaEngine`, `IpcServer`, `AppContext`, `EventBus`, and most AI components (`ProviderRegistry`, `ToolExecutor`, `ApiKeyStore`, `AiSettings`, `SessionManager`, `UIMessageQueue`) use `static` local in `GetInstance()`/`Get()`.
- **Socket thread safety**: never issue a raw send/receive pair without the normal `SocketCommand::execute*` path. It holds the connection lease, recursive per-port transaction gate, and request-response mutex; responses from concurrent callers will interleave if the last lock is bypassed.
- **Single-command locking is not a transaction**: `ScanSetRange`→scan, `SymbolInit`→`SymbolGetList`, and process switching are multi-command shared-state sequences. Add a higher-level transaction/revision/epoch when correctness spans more than one request.
- **Pointer transaction**: `pointer_resolve` holds `SocketCommand::TransactionLease` across module lookup and every dereference, then validates the full target after releasing it. Do not acquire the AppContext process-state mutex while the transaction gate is held.
- **Scan session**: every legacy/native scan mutation advances a monotonic epoch. Canonical start owns range+scan in one transaction; refine/results/clear require the latest epoch and reject cross-frontend replacement.
- **Symbol session**: every `SymbolInit` advances its epoch inside the transaction gate. Canonical resolve/list bind module lookup + init + find/page in one transaction; continuation pages require the latest epoch.
- **Breakpoint receipts**: set/remove/suspend/resume update the remote state and cleanup tracker in one MAIN transaction. Sent-without-response is `completion_unknown`; hit data is paged with 64-bit values encoded as strings at the Agent boundary. Disconnect resets only the local tracker.
- **Timeout poisons unframed connections**: timeout, EOF, or partial I/O poisons `DeviceSession`, closes the failed client, and advances the generation. The old pending-data drain recovery path is gone; explicitly reconnect before reuse.
- **Connection lifecycle**: commands hold a shared `DeviceSession` request lease; connect/disconnect/reconnect hold an exclusive lifecycle lease. Do not bypass this gate with direct client `Connect()`/`Close()` calls.
- **UI thread isolation for AI**: background provider/HTTP threads communicate with ImGui exclusively through `UIMessageQueue`. ImGui calls happen only on the main thread.
- **Process target consistency**: process-bound Agent operations carry an explicit generation/PID/handle/revision snapshot and must validate it at their actual service/send boundary; runId is not a target identifier.
- **Address format**: use `0x` for address strings across AI, IPC, docs, and tests.
- **Resource bounds**: validate untrusted sizes before allocation and cap raw HTTP/SSE data, tool outputs, and persisted session input, not just tool arguments.
- **Sensitive data**: DPAPI protects provider API keys only. Redact secrets before putting tool arguments/results into session history.
- **Scan flags are bitmasks** (defined in `MemoryTypes.h`): exactly one data-type bit (`BYTE_`/`WORD_`/`DWORD_`/`QWORD_`/`FLOAT_`/`DOUBLE_`/`XOR_`) OR-ed with one scan-mode bit (`_ACCURATE_VAL`, `_LARGER_THAN_VAL`, `_LESS_THAN_VAL`, `_BETWEEN_VAL`, `_UNKNOW_VAL`, `_ADD_UNKNOW_VAL`, `_SUB_UNKNOW_VAL`, `_CHANGED_VAL`, `_UNCHANGED_VAL`, …). The native Agent and temporary IPC validate combinations independently; new work belongs in the shared service contract rather than another constants mirror.
- **Conditional compilation**: `HAVE_AI_CHAT` / `HAVE_CAPSTONE` / `HAVE_KEYSTONE` / `HAVE_LUAJIT` gate optional features; all source that touches them is `#ifdef`-guarded so the app builds with any subset present.
- **ImGui docking**: uses the docking branch; windows use `ImGuiWindowFlags_NoDocking` selectively.

## Branches

- `dev` — remote default/main integration branch
- `NativeAgent` — independent native in-app Agent branch; not currently intended to merge into `dev`
- `AIChat` — AI chat + MCP/IPC baseline branch
- `WinGui` — earlier Windows GUI branch
- `docking` — earlier development branch

## Note for maintainers

`AGENTS.md` (guidance for Codex) historically mirrored this file's content. If you make substantive architecture changes, update both plus the relevant `docs/agent_*.md` files. The detailed current risk register is `docs/agent_project_issues.md`.
