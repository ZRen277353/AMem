# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

AMem is a Windows desktop application for remote Android memory debugging, similar to Cheat Engine. It connects to an Android device over Socket and provides memory scanning, hardware breakpoint debugging, a hex memory viewer, value freezing, ELF symbol resolution, and Lua scripting. The UI is built with Dear ImGui (docking branch) rendered via DirectX 12.

On top of the GUI it ships two AI integration paths: an **in-app AI chat agent** (multi-provider: Claude / OpenAI / DeepSeek) that can drive the debugger via tool calls, and an **MCP server** (`mcp/`) that exposes the same capabilities to external AI assistants (Claude Code / Desktop, Codex, Cursor, etc.).

Language: C++17 (app) + Python 3.10+ (MCP server). Platform: Windows 10/11 x64 only.

## Build Commands

```bash
# Configure (from repo root, using Clang + Ninja)
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=TRUE \
  -DCMAKE_C_COMPILER="C:/Program Files/LLVM/bin/clang.exe" \
  -DCMAKE_CXX_COMPILER="C:/Program Files/LLVM/bin/clang++.exe" \
  --no-warn-unused-cli -S . -B build -G Ninja

# Build
cmake --build build
```

Output binary: `bin/ImGuiProject.exe`. The project can also be opened directly in Visual Studio via CMakeLists.txt (select x64-Release or x64-Debug).

There is no automated C++ test suite. To exercise the device protocol end-to-end, use the MCP reference client (`mcp/reference/amem_client.py`) or dump the live Lua API surface with `scripts/dump_api.lua` (see `scripts/README.md`).

### MCP server (Python)

```bash
cd mcp
pip install -e .        # registers the `amem-mcp` command
amem-mcp                # starts the stdio MCP server (talks to the GUI's IPC server)
```

## Dependencies

- **Required**: Visual Studio 2022 (C++17), CMake 3.16+, DirectX 12 SDK, Windows SDK
- **Required**: LuaJIT — must be placed in `third_party/LuaJIT/` with `include/` and `lib/lua51.lib` (a `FATAL_ERROR` otherwise)
- **AI chat** (`ENABLE_AI_CHAT`, default ON): `third_party/httplib/httplib.h` and `third_party/nlohmann/json.hpp` are vendored in-tree; additionally requires **OpenSSL** for HTTPS via `vcpkg install openssl:x64-windows-static` (static `/MT`; linked into the exe, no DLL shipped). If OpenSSL is missing, AI chat is silently disabled but the rest of the app builds.
- **Optional**: Capstone — disassembly. Static `/MT` only: official installer at `capstone_ROOT` (default `C:/Program Files/capstone`, 6.x) or `vcpkg install capstone[core,arm,arm64,x86,mips]:x64-windows-static` (5.x). Linked into the exe. Do NOT use the dynamic `x64-windows` triplet — its `/MD` import lib conflicts with the `/MT` build.
- **Optional**: Keystone — assembly-to-machine-code. Static `/MT`: `vcpkg install keystone:x64-windows-static` (or set `keystone_ROOT`). Linked into the exe.
- **Static single-exe distribution**: the whole app links `/MT` (static CRT via `CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded` + CMP0091) plus static capstone/keystone/OpenSSL/LuaJIT. **This is a hard constraint**: the prebuilt `lua51.lib` and official `capstone.lib` are both `/MT` (LIBCMT), so the exe must be `/MT` to avoid CRT conflicts. Result: `bin/ImGuiProject.exe` (~34MB) is fully self-contained — no capstone/keystone/OpenSSL DLLs, no VC runtime DLLs, no VC++ Redistributable needed; only Windows 10/11 x64 system DLLs. Copy the exe alone to another machine and it runs.

CMake options: `USE_DX12` (default ON), `USE_DX11` (OFF), `ENABLE_AI_CHAT` (default ON), `LUAJIT_STATIC` (default ON).

Feature gates resolved by CMake: `HAVE_AI_CHAT`, `HAVE_CAPSTONE`, `HAVE_KEYSTONE`, `HAVE_LUAJIT`. AI chat additionally defines `CPPHTTPLIB_OPENSSL_SUPPORT` and links `OpenSSL::SSL OpenSSL::Crypto crypt32` (crypt32 for DPAPI key encryption).

## Architecture

### The command pipeline — the unifying idea

The free functions declared in `socket/client_singleton.h` (`ReadProcessMemoryBytes`, `WriteProcessMemoryBytes`, `ScanValueWithProgress`, `ScanNextValueWithProgress`, `SetKernelBreakpoint`, `ResolveModuleOffsetChain`, `SymbolFind`, …) are the **single source of truth** for the Android device protocol. Everything else is a front-end onto them. There are **three** callers:

1. **GUI windows** (`gui/`) call them directly in response to user actions.
2. **In-app AI agent** (`gui/ai/ToolDefinitions.cpp`) wraps them as tool executors registered with `ToolExecutor`.
3. **External AI assistants** call the MCP server (`mcp/`, Python), which forwards HTTP JSON to the in-process **IPC server** (`ipc/IpcServer.cpp`), whose handlers call the same functions.

```
GUI windows ─┐
in-app AI  ──┼──▶ socket/client_singleton.h  ──▶ WinSocketClientMgr (3 ports) ──▶ Android device
MCP ▶ IPC ───┘        (the protocol layer)
```

**Practical consequence:** adding a new device capability usually means (a) add the socket command in `socket/*Commands.cpp` + declare it in `client_singleton.h`, then (b) surface it in whichever front-ends need it — a GUI panel, an AI tool in `ToolDefinitions.cpp`, and/or an IPC handler in `IpcServer::RegisterBuiltinMethods()` (which the MCP server then wraps in `mcp/amem_mcp/tools/`).

### Entry point & rendering

- `main.cpp` — creates the Win32 window, drives the render loop (`Gui::mainLoop()`), installs `ExceptionHandler.h` (crash dumps), and **starts the IPC server on port 28100**.
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

- `gui/AppContext` (`AppContext::Get()`) — singleton holding global process state (`selectedPid`, `processHandle`, `processRevision`, thread-safe selected name) and a `moduleCache` (module list + per-module ELF symbol cache, with address→module/symbol formatting). The GUI, the AI tools, and the IPC server all read/write process state through this.
- `gui/EventBus` (`EventBus::Get()`) — templated publish/subscribe singleton for decoupling windows. Events live in `gui/Events.h` (`ProcessSelectedEvent`, `NavigateToAddressEvent`). Handler lists are heap-allocated (never destructed) to dodge static-destruction-order issues when windows unsubscribe at shutdown.

### Socket communication (`socket/`)

- `client.hpp` — `WindowsSocketClient` wrapping Winsock2 send/receive.
- `client_singleton.h/cpp` — `WinSocketClientMgr` singleton managing three port connections (`PORT_MAIN`, `PORT_DEBUG`, `PORT_ERROR`), each with its own mutex. Declares the entire remote command surface.
- **Command implementations are split by domain**: `ProcessCommands.cpp`, `MemoryCommands.cpp`, `ScanCommands.cpp`, `BreakpointCommands.cpp`, `FreezeCommands.cpp`, `SymbolCommands.cpp`.
- `SocketCommand.h` — the modern way to issue a command: `SocketCommand::execute` / `executeNoHandle` / `executeWithResult` templates handle the boilerplate (connection check → `EnsureOpenHandle` → per-port mutex lock → `DrainPending` → run the lambda). Prefer these over hand-rolling the locking.
- `socket_request_manager.h` — `SocketRequestManager` serializes request/response pairs so concurrent callers don't interleave responses on a shared port.
- `socket_io_timeout.h` — `SocketIoTimeout::ScopedTimeout` applies a bounded I/O timeout for the current scope (used by long Lua/IPC calls).

### AI Chat subsystem (`gui/ai/`, gated by `HAVE_AI_CHAT`)

The whole subsystem lives in the `AI` namespace and is wired up lazily in `ChatWindow`'s constructor (idempotent `initBuiltin*` calls).

- **Providers**: `AIProvider` is the abstract interface (`sendCompletion` runs async on a background thread). Built-ins `ClaudeProvider`, `OpenAIProvider`, `DeepSeekProvider` are registered in `ProviderRegistry`. `HttpClient` wraps cpp-httplib (HTTPS via OpenSSL).
- **Agent loop**: `ChatWindow` (UI) → `AgentController` (provider lookup, request construction, async dispatch, run state) → `AgentRunner` (the model→tool→model loop, step/tool budgets, approval gating) → `ToolExecutor` (thread-safe tool registry + validated execution). The ~10 built-in tools are defined in `ToolDefinitions.cpp` and each wraps a `client_singleton.h` command.
- **Tool safety**: every tool is classified `ToolSafety::ReadOnly` or `Write`. Write/stateful tools require explicit user approval in the UI unless `autoApproveWrites` is set (`AgentRunner::Config`). Much of the recent hardening (see git log) is input validation on tool/IPC arguments.
- **Threading model**: providers do HTTP on background threads and post results back to the ImGui main thread via `UIMessageQueue` (message kinds: `Token`, `Completion`, `Error`, `ToolResult`), consumed in `ChatWindow::pollMessages()`. Never touch ImGui state from a provider thread.
- **Persistence** (all relative to the working dir, i.e. next to the exe):
  - `ai_config.json` — provider configs; **API keys are encrypted with Windows DPAPI** (`ApiKeyStore`, base64 over the encrypted blob). Keys are per-Windows-user and never bundled.
  - `ai_settings.json` — non-secret, hand-editable settings (`AiSettings`): system prompt, proxy, etc. `DefaultSystemPrompt.h` seeds an AMem-specific prompt on first run.
  - `ai_sessions/<id>.json` + `ai_sessions/index.json` — chat history (`SessionManager` + `ChatSession`; max 1000 messages, token-limit truncation).
  - Legacy migrations run once on startup: `ai_config.dat`→`ai_config.json`, `ai_session.json`→`ai_sessions/`.

### IPC server (`ipc/IpcServer.cpp`)

A minimal hand-rolled HTTP server (`IpcServer` singleton) bound to **127.0.0.1:28100 only**, started from `main.cpp`. It accepts `POST /` with body `{ "method": "...", "params": {...} }` and returns `{ "success": bool, "result"/"error": ... }`. Methods are registered in `RegisterBuiltinMethods()` and call the same socket commands as the GUI. The file is heavy on input validation (size caps, scan-flag validation, address parsing) because its inputs come from an external process. This is the bridge the MCP server talks to — it is **not** a general-purpose web server.

### MCP server (`mcp/`)

A standalone Python package (`amem_mcp`, FastMCP-based) that proxies MCP tool calls over stdio to the IPC server via HTTP. It does **not** talk to the Android device directly — every tool delegates to the GUI's IPC server (so the GUI must be running and connected). Layout: `tools/` split by domain (status/process/memory/scan/breakpoint_/lua/symbols), `ipc_client.py` (HTTP client), `constants.py` (scan-flag/data-type/memory-type tables that mirror the C++ enums), `configs/` (ready-to-use snippets per IDE). See `mcp/README.md` for the full tool list and IDE setup. This repo's own `.mcp.json` wires the server for Claude Code via `python -m amem_mcp`.

### Lua scripting (`lua/`, gated by `HAVE_LUAJIT`)

`LuaEngine` (singleton) owns the LuaJIT state. `LuaAPI*.cpp` expose C++ bindings: `LuaAPI_Memory` (memory/scan/breakpoints), `LuaAPI_ImGui` (drawing), `LuaAPI_Assembly`. Lua is reachable from the GUI (`LuaScriptWindow`, `LuaImGuiWindow`), from the AI tools, and from the IPC `execute_lua` method.

### Version system

`version.h.in` → CMake generates `build/generated/version.h` with project version (1.0.0), protocol version (1.1.0, negotiated with the device), git hash, and build timestamp.

## Key Patterns

- **Singletons everywhere (Meyer's)**: `WinSocketClientMgr`, `SocketRequestManager`, `LuaEngine`, `IpcServer`, `AppContext`, `EventBus`, and most AI components (`ProviderRegistry`, `ToolExecutor`, `ApiKeyStore`, `AiSettings`, `SessionManager`, `UIMessageQueue`) use `static` local in `GetInstance()`/`Get()`.
- **Socket thread safety**: never issue a raw send/receive pair without holding the port mutex — use the `SocketCommand::execute*` templates (or the `SocketRequestManager` lock directly). Responses from concurrent callers will interleave otherwise.
- **UI thread isolation for AI**: background provider/HTTP threads communicate with ImGui exclusively through `UIMessageQueue`. ImGui calls happen only on the main thread.
- **Scan flags are bitmasks** (defined in `MemoryTypes.h`): exactly one data-type bit (`BYTE_`/`WORD_`/`DWORD_`/`QWORD_`/`FLOAT_`/`DOUBLE_`/`XOR_`) OR-ed with one scan-mode bit (`_ACCURATE_VAL`, `_LARGER_THAN_VAL`, `_LESS_THAN_VAL`, `_BETWEEN_VAL`, `_UNKNOW_VAL`, `_ADD_UNKNOW_VAL`, `_SUB_UNKNOW_VAL`, `_CHANGED_VAL`, `_UNCHANGED_VAL`, …). The IPC and AI layers validate that combinations are well-formed; keep `mcp/amem_mcp/constants.py` in sync with the C++ enums when adding values.
- **Conditional compilation**: `HAVE_AI_CHAT` / `HAVE_CAPSTONE` / `HAVE_KEYSTONE` / `HAVE_LUAJIT` gate optional features; all source that touches them is `#ifdef`-guarded so the app builds with any subset present.
- **ImGui docking**: uses the docking branch; windows use `ImGuiWindowFlags_NoDocking` selectively.

## Branches

- `WinGui` — main branch (PR target)
- `AIChat` — current development branch (AI chat + MCP/IPC work)
- `docking` — earlier development branch

## Note for maintainers

`AGENTS.md` (guidance for Codex) historically mirrored this file's content. If you make substantive architecture changes, update both so the two stay consistent.
