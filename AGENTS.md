# AGENTS.md

This file provides guidance to Codex (Codex.ai/code) when working with code in this repository.

## Project Overview

AMem is a Windows desktop application for remote Android memory debugging, similar to Cheat Engine. It connects to an Android device over Socket and provides memory scanning, hardware breakpoint debugging, and a hex memory viewer. The UI is built with Dear ImGui (docking branch) rendered via DirectX 12.

Language: C++17. Platform: Windows 10/11 x64 only.

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

Output binary: `bin/ImGuiProject.exe`

The project can also be opened directly in Visual Studio via CMakeLists.txt (select x64-Release or x64-Debug).

## Dependencies

- **Required**: Visual Studio 2022 (C++17), CMake 3.16+, DirectX 12 SDK, Windows SDK
- **Required**: LuaJIT — must be placed in `third_party/LuaJIT/` with `include/` and `lib/lua51.lib`
- **Optional**: Capstone disassembly library — enables disassembly features in breakpoint window. Install via `vcpkg install capstone:x64-windows` or set `capstone_ROOT`
- **Optional**: Keystone assembler library — enables assembly-to-machine-code features. Install via `vcpkg install keystone:x64-windows` or set `keystone_ROOT`

CMake options: `USE_DX12` (default ON), `USE_DX11` (OFF), `LUAJIT_STATIC` (default ON).

## Architecture

### Entry Point & Rendering

`main.cpp` — Sets up the Win32 window, DirectX 12 device/swapchain, ImGui context, font loading (Chinese fonts), and the main render loop. Includes `ExceptionHandler.h` for crash dump generation.

### GUI Layer (`gui/`)

- `Window` — Base class for all windows. Has `pOpen`, `name`, virtual `onDraw()`, and `draw()` which wraps ImGui Begin/End.
- `Gui` namespace (`Gui.h/cpp`) — Owns a `std::list<std::unique_ptr<Window>>` of all active windows. `Gui::mainLoop()` iterates and draws them. `Gui::addWindow()` registers new windows. `Gui::log()` for debug logging.
- `CEWindow` — Main application window (menu bar, process selection). Acts as the hub that opens child windows (ScanWindow, MemoryViewerWindow, BreakpointWindow, etc.).
- `ColorScheme.h` — Centralized `ImVec4` color constants in the `ColorScheme` namespace. All UI code should reference these instead of hardcoding colors.
- `ConfigManager` — Persists user settings.
- Window implementations: `ScanWindow`, `MemoryViewerWindow`, `BreakpointWindow`, `ModulesWindow`, `ProcessListWindow`, `ServerConnectWindow`, `LogWindow`, `VersionWindow`, `LuaScriptWindow`, `LuaImGuiWindow`.
- `DisassemblyHelper` — ARM64 disassembly rendering (requires Capstone, guarded by `HAVE_CAPSTONE`).
- `AssemblyHelper` — ARM64 assembly-to-machine-code (requires Keystone, guarded by `HAVE_KEYSTONE`).

### Socket Communication (`socket/`)

- `client.hpp` — `WindowsSocketClient` wrapping Winsock2 send/receive.
- `client_singleton.h/cpp` — `WinSocketClientMgr` singleton managing three port connections: `PORT_MAIN`, `PORT_DEBUG`, `PORT_ERROR`. Also declares all remote command functions (memory read/write, scan, breakpoints, process/module listing, etc.).
- `socket_request_manager.h` — `SocketRequestManager` singleton with mutex-based locking to serialize socket request-response pairs across multiple UI windows. Use `EXECUTE_SOCKET_REQUEST()` or `EXECUTE_SOCKET_REQUEST_WITH_LOCK()` macros for thread-safe socket operations.

### Lua Scripting (`lua/`)

- `LuaEngine` — Singleton managing LuaJIT state. Guarded by `HAVE_LUAJIT` compile definition.
- `LuaAPI.cpp`, `LuaAPI_Memory.cpp`, `LuaAPI_ImGui.cpp`, `LuaAPI_Assembly.cpp` — C++ bindings exposed to Lua scripts for memory operations, ImGui drawing, and assembly.

### Version System

`version.h.in` — CMake template generating `build/generated/version.h` with project version, protocol version, git hash, and build timestamp.

## Key Patterns

- **Singletons**: `WinSocketClientMgr`, `SocketRequestManager`, `LuaEngine` all use Meyer's singleton (`static` local in `GetInstance()`).
- **Socket thread safety**: All socket send/receive pairs must be wrapped in `SocketRequestManager` locks to prevent response interleaving between windows. Use per-port mutexes from `GetSocketMgr().GetMutex(PORT_*)` for better concurrency.
- **Conditional compilation**: `HAVE_CAPSTONE` gates disassembly features; `HAVE_KEYSTONE` gates assembly features; `HAVE_LUAJIT` gates Lua scripting. All are set automatically by CMake based on library availability.
- **ImGui docking**: The project uses ImGui's docking branch. Windows use `ImGuiWindowFlags_NoDocking` selectively to control docking behavior.

## Branches

- `WinGui` — main branch (PR target)
- `docking` — current development branch
