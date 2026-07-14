cmake_minimum_required(VERSION 3.16)

# 项目版本定义
# ==================== 项目版本定义 ====================
set(PROJECT_VERSION_MAJOR 1)
set(PROJECT_VERSION_MINOR 0)
set(PROJECT_VERSION_PATCH 0)
set(PROJECT_VERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}")

# 协议版本（与服务端协商使用）
set(PROTOCOL_VERSION_MAJOR 1)
set(PROTOCOL_VERSION_MINOR 1)
set(PROTOCOL_VERSION_PATCH 0)
set(PROTOCOL_VERSION "${PROTOCOL_VERSION_MAJOR}.${PROTOCOL_VERSION_MINOR}.${PROTOCOL_VERSION_PATCH}")


# ==================== 完全静态链接（单 exe 分发） ====================
# 目标：产出自包含 exe，无需随附任何第三方 DLL 或 VC 运行时 DLL。
# 静态 CRT（/MT）是硬约束：预编译的 LuaJIT lua51.lib 与官方 capstone.lib 均为 /MT（LIBCMT），
# 整个 exe 必须 /MT 才能避免 CRT 状态/堆不一致的冲突。
# CMP0091 需在第一个 project() 之前设为 NEW，MSVC_RUNTIME_LIBRARY 抽象方可生效。
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded" CACHE STRING "静态 CRT (/MT)，单 exe 分发")

project(ImGuiProject VERSION ${PROJECT_VERSION})
set(CMAKE_CXX_STANDARD 17)
include(CTest)

# 渲染后端选项
option(USE_DX11 "Use DirectX 11 backend" OFF)
option(USE_DX12 "Use DirectX 12 backend" ON)

# NativeAgent 产品固定包含 AI Chat、Capstone 和 Keystone；缺失依赖时配置失败。

# 原生 Named Pipe transport 是显式编译能力；即使编入产品，运行时仍默认停止，
# 只能由用户在 GUI 中启用 Observe + 逐请求特权策略。
option(ENABLE_NATIVE_IPC "Compile native Named Pipe transport" OFF)

# 生成版本信息头文件
string(TIMESTAMP BUILD_DATE "%Y-%m-%d")
string(TIMESTAMP BUILD_TIME "%H:%M:%S")

find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_COMMIT_HASH OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --abbrev-ref HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_BRANCH OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
else()
    set(GIT_COMMIT_HASH "unknown")
    set(GIT_BRANCH "unknown")
endif()

configure_file(${CMAKE_SOURCE_DIR}/version.h.in ${CMAKE_BINARY_DIR}/generated/version.h @ONLY)


# 输出目录
if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
endif()
if(NOT CMAKE_LIBRARY_OUTPUT_DIRECTORY)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/lib)
endif()
if(NOT CMAKE_ARCHIVE_OUTPUT_DIRECTORY)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/lib)
endif()

# 查找 Capstone 库（完全静态）：优先官方安装器静态版（C:/Program Files/capstone，6.x，/MT），
# 整体链入 exe，不分发 DLL；也接受 vcpkg 静态三元组 x64-windows-static（5.x，/MT）。
# 切勿使用 vcpkg x64-windows 动态版（/MD 导入库 + DLL）——会与 /MT 冲突且需分发 capstone.dll。
set(capstone_ROOT "C:/Program Files/capstone" CACHE PATH "Capstone installation directory")

find_path(CAPSTONE_INCLUDE_DIR NAMES capstone/capstone.h
    HINTS "${capstone_ROOT}/include" "C:/vcpkg/installed/x64-windows-static/include" PATH_SUFFIXES include)
find_library(CAPSTONE_LIBRARY NAMES capstone capstone_static
    HINTS "${capstone_ROOT}/lib" "C:/vcpkg/installed/x64-windows-static/lib" PATH_SUFFIXES lib lib64)

if(NOT CAPSTONE_INCLUDE_DIR
   OR NOT EXISTS "${CAPSTONE_INCLUDE_DIR}/capstone/capstone.h"
   OR NOT CAPSTONE_LIBRARY
   OR NOT EXISTS "${CAPSTONE_LIBRARY}")
    message(FATAL_ERROR
        "Capstone is required on NativeAgent. Install a static /MT build or set capstone_ROOT.")
endif()
set(CAPSTONE_LIBRARIES ${CAPSTONE_LIBRARY})
set(CAPSTONE_INCLUDE_DIRS ${CAPSTONE_INCLUDE_DIR})

# 查找 Keystone 库（完全静态，vcpkg x64-windows-static，/MT）
set(keystone_ROOT "C:/vcpkg/installed/x64-windows-static" CACHE PATH "Keystone installation directory")

find_path(KEYSTONE_INCLUDE_DIR keystone/keystone.h
    HINTS "${keystone_ROOT}/include" "${keystone_ROOT}" PATH_SUFFIXES include)
find_library(KEYSTONE_LIBRARY NAMES keystone
    HINTS "${keystone_ROOT}/lib" "${keystone_ROOT}" PATH_SUFFIXES lib lib64)

if(NOT KEYSTONE_INCLUDE_DIR
   OR NOT EXISTS "${KEYSTONE_INCLUDE_DIR}/keystone/keystone.h"
   OR NOT KEYSTONE_LIBRARY
   OR NOT EXISTS "${KEYSTONE_LIBRARY}")
    message(FATAL_ERROR
        "Keystone is required on NativeAgent. Install keystone:x64-windows-static or set keystone_ROOT.")
endif()
set(KEYSTONE_LIBRARIES ${KEYSTONE_LIBRARY})
set(KEYSTONE_INCLUDE_DIRS ${KEYSTONE_INCLUDE_DIR})

# ==================== AI Chat Integration ====================
# Requirements: 13.1, 13.2, 13.3, 13.4, 13.5
set(HTTPLIB_HEADER "${CMAKE_SOURCE_DIR}/third_party/httplib/httplib.h")
set(NLOHMANN_JSON_HEADER "${CMAKE_SOURCE_DIR}/third_party/nlohmann/json.hpp")

if(NOT EXISTS "${HTTPLIB_HEADER}")
    message(FATAL_ERROR "Required cpp-httplib header not found at ${HTTPLIB_HEADER}")
endif()
if(NOT EXISTS "${NLOHMANN_JSON_HEADER}")
    message(FATAL_ERROR "Required nlohmann/json header not found at ${NLOHMANN_JSON_HEADER}")
endif()

# cpp-httplib 需要 OpenSSL 才能支持 HTTPS（CPPHTTPLIB_OPENSSL_SUPPORT）。
# 完全静态：使用 vcpkg x64-windows-static，避免 /MD 与最终 /MT 产品冲突。
# 不设置 OPENSSL_USE_STATIC_LIBS：Clang GNU-like driver 会因此只查找 .a，
# 无法匹配 vcpkg 提供的静态 .lib。
if(NOT OPENSSL_ROOT_DIR AND EXISTS "C:/vcpkg/installed/x64-windows-static/include/openssl/ssl.h")
    set(OPENSSL_ROOT_DIR "C:/vcpkg/installed/x64-windows-static")
    set(AMEM_OPENSSL_STATIC TRUE)
endif()
find_package(OpenSSL REQUIRED)
message(STATUS "Required AI chat dependencies found: cpp-httplib + nlohmann/json + OpenSSL")

# AI 聊天模块是 NativeAgent 产品的固定组成部分。
set(AI_CHAT_SOURCES
    gui/ai/ProviderRegistry.cpp
    gui/ai/HttpClient.cpp
    gui/ai/SSEParser.cpp
    gui/ai/ProviderStreamTerminal.cpp
    gui/ai/ProviderResponseLimits.cpp
    gui/ai/ContextBudget.cpp
    gui/ai/ProviderTrust.cpp
    gui/ai/Persistence.cpp
    gui/ai/ProtectedPersistence.cpp
    gui/ai/UIMessageQueue.cpp
    gui/ai/OpenAIProvider.cpp
    gui/ai/ClaudeProvider.cpp
    gui/ai/DeepSeekProvider.cpp
    gui/ai/ToolExecutor.cpp
    gui/ai/ToolDefinitions.cpp
    gui/ai/ChatSession.cpp
    gui/ai/ApiKeyStore.cpp
    gui/ai/AiSettings.cpp
    gui/ai/SessionManager.cpp
    gui/ai/AgentController.cpp
    gui/ai/AgentRunner.cpp
    gui/ai/AgentMutationAudit.cpp
    gui/ai/ToolCallSecurity.cpp
    gui/ai/AgentRunContext.cpp
    gui/ai/AgentTaskExecutor.cpp
    gui/ai/AgentMemToolsSystem.cpp
    gui/ai/ChatSettingsDraft.cpp
    gui/ai/ChatWindow.cpp
    gui/ai/ChatWindowSettings.cpp
)

set(IMGUI_SOURCES
    imgui/imgui.cpp
    imgui/imgui_draw.cpp
    imgui/imgui_tables.cpp
    imgui/imgui_widgets.cpp
    imgui/imgui_demo.cpp
    imgui/backends/imgui_impl_win32.cpp
)

set(GUI_SOURCES
    gui/Gui.cpp
    gui/Window.cpp
    gui/CEWindow.cpp
    gui/ServerConnectWindow.cpp
    gui/ModulesWindow.cpp
    gui/ScanWindow.cpp
    gui/scan/ScanPanel.cpp
    gui/scan/ScanResultsPanel.cpp
    gui/scan/AddressListPanel.cpp
    gui/scan/ScanEngine.cpp
    gui/MemoryViewerWindow.cpp
    gui/memview/HexEditorPanel.cpp
    gui/memview/StructAnalyzerPanel.cpp
    gui/memview/WatchListPanel.cpp
    gui/memview/DisassemblyPanel.cpp
    gui/BreakpointWindow.cpp
    gui/breakpoint/BreakpointListPanel.cpp
    gui/breakpoint/BreakpointDetailPanel.cpp
    gui/ProcessListWindow.cpp
    gui/VersionWindow.cpp
    gui/DisassemblyHelper.cpp
    gui/AssemblyHelper.cpp
    gui/ConfigManager.cpp
    gui/LogWindow.cpp
    gui/AppContext.cpp
    gui/ValueFormatter.cpp
)

set(LUA_SOURCES
    lua/LuaEngine.cpp
    lua/LuaAPI.cpp
    lua/LuaAPI_Memory.cpp
    lua/LuaAPI_ImGui.cpp
    lua/LuaAPI_Assembly.cpp
    mem/LuaJsonTool.cpp
)

# 查找 LuaJIT 库
option(LUAJIT_STATIC "Link LuaJIT statically" ON)

set(LUAJIT_DIR "${CMAKE_SOURCE_DIR}/third_party/LuaJIT")
set(LUAJIT_INCLUDE "${LUAJIT_DIR}/include")
set(LUAJIT_LIB "${LUAJIT_DIR}/lib")
set(LUAJIT_BIN "${LUAJIT_DIR}/bin")

if((EXISTS "${LUAJIT_INCLUDE}/lua.hpp" OR EXISTS "${LUAJIT_INCLUDE}/luajit.h" OR EXISTS "${LUAJIT_INCLUDE}/lua.h")
   AND EXISTS "${LUAJIT_LIB}/lua51.lib")
    set(HAVE_LUAJIT TRUE)
    set(LUAJIT_LIBRARY "${LUAJIT_LIB}/lua51.lib")
    set(LUAJIT_IS_STATIC ${LUAJIT_STATIC})
else()
    message(FATAL_ERROR "LuaJIT not found in third_party/LuaJIT directory!")
    set(HAVE_LUAJIT FALSE)
endif()

if(HAVE_LUAJIT)
    list(APPEND GUI_SOURCES gui/LuaScriptWindow.cpp gui/LuaImGuiWindow.cpp)
endif()

list(APPEND GUI_SOURCES ${AI_CHAT_SOURCES})

set(SOCKET_SOURCES
    socket/DeviceSession.cpp
    socket/MultiPortClientManager.cpp
    socket/ServerHandshake.cpp
    socket/client_singleton.cpp
    socket/ProcessCommands.cpp
    socket/MemoryCommands.cpp
    socket/ScanCommands.cpp
    socket/BreakpointCommands.cpp
    socket/FreezeCommands.cpp
    socket/SymbolCommands.cpp
)

set(MEM_SOURCES
    mem/Address.cpp
    mem/LuaOperationContext.cpp
    mem/MemService.cpp
    mem/SystemMemService.cpp
    mem/ValueCodec.cpp
)

add_library(NativeBoundedJson STATIC
    utils/BoundedJson.cpp
)
target_include_directories(NativeBoundedJson PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/third_party
)
if(MSVC)
    target_compile_options(NativeBoundedJson PRIVATE /W4)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(NativeBoundedJson PRIVATE -Wall -Wextra)
endif()
set_target_properties(NativeBoundedJson PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/native-json"
)

add_library(NativeMemJsonTools STATIC
    mem/MemJsonTools.cpp
)
target_include_directories(NativeMemJsonTools PUBLIC
    ${CMAKE_SOURCE_DIR}
)
target_include_directories(NativeMemJsonTools PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party
)
target_compile_definitions(NativeMemJsonTools PRIVATE
    HAVE_CAPSTONE
    HAVE_KEYSTONE
)
if(HAVE_LUAJIT)
    target_compile_definitions(NativeMemJsonTools PRIVATE HAVE_LUAJIT)
endif()
if(MSVC)
    target_compile_options(NativeMemJsonTools PRIVATE /W4)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(NativeMemJsonTools PRIVATE -Wall -Wextra)
endif()
set_target_properties(NativeMemJsonTools PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/native-mem"
)

set(RENDERER_SOURCES
    renderer/DX12Renderer.cpp
    renderer/StyleSetup.cpp
)

if(ENABLE_NATIVE_IPC OR BUILD_TESTING)
    add_library(NativeIpcTransport STATIC
        ipc/IpcApprovalAudit.cpp
        ipc/IpcApprovalBroker.cpp
        ipc/IpcFramedConnection.cpp
        ipc/IpcHandshakeSession.cpp
        ipc/IpcMemServiceDispatcher.cpp
        ipc/IpcMethodCatalog.cpp
        ipc/IpcProtocol.cpp
        ipc/IpcRequestProtocol.cpp
        ipc/IpcRequestSession.cpp
        ipc/NamedPipeServer.cpp
        ipc/NativePipeSecurity.cpp
        ipc/NativeAgentRuntime.cpp
    )
    target_include_directories(NativeIpcTransport PUBLIC
        ${CMAKE_SOURCE_DIR}
    )
    target_include_directories(NativeIpcTransport PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party
    )
    target_link_libraries(NativeIpcTransport PUBLIC
        NativeMemJsonTools
        NativeBoundedJson
        advapi32
    )
    if(MSVC)
        target_compile_options(NativeIpcTransport PRIVATE /W4)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(NativeIpcTransport PRIVATE -Wall -Wextra)
    endif()
    set_target_properties(NativeIpcTransport PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/native-ipc"
    )
endif()

if(ENABLE_NATIVE_IPC)
    add_library(NativeIpcGuiControl STATIC
        gui/NativeAgentIpcWindow.cpp
        ipc/SystemNativeAgentRuntime.cpp
    )
    target_include_directories(NativeIpcGuiControl PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/imgui
        ${CMAKE_SOURCE_DIR}/gui
    )
    target_link_libraries(NativeIpcGuiControl PUBLIC
        NativeIpcTransport
    )
    target_compile_definitions(NativeIpcGuiControl PRIVATE HAVE_AI_CHAT)
    if(HAVE_LUAJIT)
        target_compile_definitions(NativeIpcGuiControl PRIVATE HAVE_LUAJIT)
    endif()
    if(MSVC)
        target_compile_options(NativeIpcGuiControl PRIVATE /W4)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(NativeIpcGuiControl PRIVATE -Wall -Wextra)
    endif()
    set_target_properties(NativeIpcGuiControl PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/native-ipc"
    )
    message(STATUS
        "Native IPC compiled with explicit Observe + privileged policy control")
else()
    message(STATUS "Native IPC transport disabled")
endif()

# 配置 DirectX 后端
if(USE_DX11)
    add_definitions(-DUSE_DX11)
    list(APPEND IMGUI_SOURCES imgui/backends/imgui_impl_dx11.cpp)
    set(DX_LIBS d3d11 dxgi d3dcompiler)
elseif(USE_DX12)
    add_definitions(-DUSE_DX12)
    list(APPEND IMGUI_SOURCES imgui/backends/imgui_impl_dx12.cpp)
    set(DX_LIBS d3d12 dxgi d3dcompiler ws2_32)
endif()

add_definitions(-DIMGUI_DISABLE_DEBUG_TOOLS)

if(MSVC)
    add_compile_options(/utf-8)
endif()

add_executable(ImGuiProject
    main.cpp
    ${IMGUI_SOURCES}
    ${GUI_SOURCES}
    ${MEM_SOURCES}
    ${SOCKET_SOURCES}
    ${RENDERER_SOURCES}
    ${LUA_SOURCES}
)

if(ENABLE_NATIVE_IPC)
    target_compile_definitions(ImGuiProject PRIVATE HAVE_NATIVE_IPC)
    target_link_libraries(ImGuiProject NativeIpcTransport NativeIpcGuiControl)
endif()

target_link_libraries(ImGuiProject NativeMemJsonTools NativeBoundedJson)

# 包含目录
target_include_directories(ImGuiProject PRIVATE
    imgui imgui/backends gui lua mem renderer utils ipc
    ${CMAKE_SOURCE_DIR}/third_party
    ${CMAKE_BINARY_DIR}/generated
)

target_include_directories(ImGuiProject PRIVATE
    ${CAPSTONE_INCLUDE_DIRS}
    ${KEYSTONE_INCLUDE_DIRS}
)

if(HAVE_LUAJIT)
    target_include_directories(ImGuiProject PRIVATE ${LUAJIT_INCLUDE})
endif()

# 链接系统库
target_link_libraries(ImGuiProject ${DX_LIBS})

# 链接 LuaJIT 库
if(HAVE_LUAJIT)
    get_filename_component(LUAJIT_LIBRARY_ABS ${LUAJIT_LIBRARY} ABSOLUTE)
    
    target_link_libraries(ImGuiProject ${LUAJIT_LIBRARY_ABS})
    target_compile_definitions(ImGuiProject PRIVATE HAVE_LUAJIT)
    
    if(LUAJIT_IS_STATIC)
        target_compile_definitions(ImGuiProject PRIVATE LUAJIT_STATIC)
    elseif(EXISTS "${LUAJIT_BIN}/lua51.dll")
        add_custom_command(TARGET ImGuiProject POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${LUAJIT_BIN}/lua51.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/lua51.dll")
    endif()
endif()

# Capstone、Keystone 与 AI Chat 是 NativeAgent 的必需产品能力。
target_compile_definitions(ImGuiProject PRIVATE
    HAVE_CAPSTONE
    HAVE_KEYSTONE
    HAVE_AI_CHAT
    CPPHTTPLIB_OPENSSL_SUPPORT
)
target_include_directories(ImGuiProject PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/httplib
    ${OPENSSL_INCLUDE_DIR}
)
target_link_libraries(ImGuiProject
    ${CAPSTONE_LIBRARIES}
    ${KEYSTONE_LIBRARIES}
    OpenSSL::SSL
    OpenSSL::Crypto
    crypt32
)

# 静态 OpenSSL（libssl/libcrypto）在 Windows 上额外依赖的系统库。
# Keystone 静态库自带的 /DEFAULTLIB 指令由链接器自动采纳。
if(AMEM_OPENSSL_STATIC)
    target_link_libraries(ImGuiProject ws2_32 advapi32 user32 gdi32 bcrypt)
endif()

if(BUILD_TESTING)
    add_executable(NativePersistenceTests
        tests/PersistenceTests.cpp
        gui/ai/Persistence.cpp
        gui/ai/ProtectedPersistence.cpp
        gui/ai/ProviderResponseLimits.cpp
        gui/ai/ContextBudget.cpp
        gui/ai/ApiKeyStore.cpp
        gui/ai/AiSettings.cpp
        gui/ai/ChatSettingsDraft.cpp
        gui/ai/SessionManager.cpp
        gui/ai/ChatSession.cpp
        gui/ai/ToolCallSecurity.cpp
    )
    target_include_directories(NativePersistenceTests PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/third_party
    )
    target_compile_definitions(NativePersistenceTests PRIVATE HAVE_AI_CHAT)
    target_link_libraries(NativePersistenceTests PRIVATE
        NativeBoundedJson
        crypt32
    )
    if(MSVC)
        target_compile_options(NativePersistenceTests PRIVATE /W4)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(NativePersistenceTests PRIVATE -Wall -Wextra)
    endif()
    set_target_properties(NativePersistenceTests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )
    add_test(NAME native_persistence_recovery COMMAND NativePersistenceTests)

    add_executable(NativeContextBudgetTests
        tests/ContextBudgetTests.cpp
        gui/ai/ContextBudget.cpp
    )
    target_include_directories(NativeContextBudgetTests PRIVATE
        ${CMAKE_SOURCE_DIR}
    )
    target_compile_definitions(NativeContextBudgetTests PRIVATE HAVE_AI_CHAT)
    if(MSVC)
        target_compile_options(NativeContextBudgetTests PRIVATE /W4)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(NativeContextBudgetTests PRIVATE -Wall -Wextra)
    endif()
    set_target_properties(NativeContextBudgetTests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )
    add_test(NAME native_context_budget COMMAND NativeContextBudgetTests)

    add_executable(NativeProviderTrustTests
        tests/ProviderTrustTests.cpp
        gui/ai/ProviderTrust.cpp
    )
    target_include_directories(NativeProviderTrustTests PRIVATE
        ${CMAKE_SOURCE_DIR}
    )
    target_compile_definitions(NativeProviderTrustTests PRIVATE HAVE_AI_CHAT)
    if(MSVC)
        target_compile_options(NativeProviderTrustTests PRIVATE /W4)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(NativeProviderTrustTests PRIVATE -Wall -Wextra)
    endif()
    set_target_properties(NativeProviderTrustTests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )
    add_test(NAME native_provider_trust COMMAND NativeProviderTrustTests)

    add_executable(NativeProviderStreamTerminalTests
        tests/ProviderStreamTerminalTests.cpp
        gui/ai/SSEParser.cpp
        gui/ai/ProviderStreamTerminal.cpp
        gui/ai/ProviderResponseLimits.cpp
    )
    target_include_directories(NativeProviderStreamTerminalTests PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/third_party
    )
    target_compile_definitions(NativeProviderStreamTerminalTests PRIVATE HAVE_AI_CHAT)
    target_link_libraries(NativeProviderStreamTerminalTests PRIVATE
        NativeBoundedJson
    )
    if(MSVC)
        target_compile_options(NativeProviderStreamTerminalTests PRIVATE /W4)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(NativeProviderStreamTerminalTests PRIVATE -Wall -Wextra)
    endif()
    set_target_properties(NativeProviderStreamTerminalTests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )
    add_test(
        NAME native_provider_stream_terminal
        COMMAND NativeProviderStreamTerminalTests
    )

    add_executable(NativeProviderHttpIntegrationTests
        tests/ProviderHttpIntegrationTests.cpp
        gui/ai/HttpClient.cpp
        gui/ai/SSEParser.cpp
        gui/ai/ProviderStreamTerminal.cpp
        gui/ai/ProviderResponseLimits.cpp
        gui/ai/OpenAIProvider.cpp
        gui/ai/ClaudeProvider.cpp
        gui/ai/DeepSeekProvider.cpp
        gui/ai/UIMessageQueue.cpp
    )
    target_include_directories(NativeProviderHttpIntegrationTests PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/third_party
        ${CMAKE_SOURCE_DIR}/third_party/httplib
    )
    target_compile_definitions(NativeProviderHttpIntegrationTests PRIVATE
        HAVE_AI_CHAT
        CPPHTTPLIB_OPENSSL_SUPPORT
    )
    target_link_libraries(NativeProviderHttpIntegrationTests PRIVATE
        NativeBoundedJson
        OpenSSL::SSL
        OpenSSL::Crypto
        crypt32
        ws2_32
    )
    if(MSVC)
        target_compile_options(NativeProviderHttpIntegrationTests PRIVATE /W4)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(NativeProviderHttpIntegrationTests PRIVATE
            -Wall -Wextra)
    endif()
    set_target_properties(NativeProviderHttpIntegrationTests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )
    add_test(
        NAME native_provider_http_integration
        COMMAND NativeProviderHttpIntegrationTests
    )
    set_tests_properties(native_provider_http_integration PROPERTIES TIMEOUT 20)

    add_executable(NativeHttpClientLifecycleTests
        tests/HttpClientLifecycleTests.cpp
        gui/ai/HttpClient.cpp
        gui/ai/SSEParser.cpp
    )
    target_include_directories(NativeHttpClientLifecycleTests PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/third_party
        ${CMAKE_SOURCE_DIR}/third_party/httplib
    )
    target_compile_definitions(NativeHttpClientLifecycleTests PRIVATE HAVE_AI_CHAT)
    target_link_libraries(NativeHttpClientLifecycleTests PRIVATE ws2_32)
    if(MSVC)
        target_compile_options(NativeHttpClientLifecycleTests PRIVATE /W4)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(NativeHttpClientLifecycleTests PRIVATE -Wall -Wextra)
    endif()
    set_target_properties(NativeHttpClientLifecycleTests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )
    add_test(
        NAME native_http_client_lifecycle
        COMMAND NativeHttpClientLifecycleTests
    )
    set_tests_properties(native_http_client_lifecycle PROPERTIES TIMEOUT 20)

    add_executable(NativeIpcApprovalAuditTests
        tests/IpcApprovalAuditTests.cpp
    )
    target_include_directories(NativeIpcApprovalAuditTests PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party
    )
    target_link_libraries(NativeIpcApprovalAuditTests PRIVATE
        NativeIpcTransport
    )
    if(MSVC)
        target_compile_options(NativeIpcApprovalAuditTests PRIVATE /W4)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(NativeIpcApprovalAuditTests PRIVATE -Wall -Wextra)
    endif()
    set_target_properties(NativeIpcApprovalAuditTests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )
    add_test(NAME native_ipc_approval_audit COMMAND NativeIpcApprovalAuditTests)

    add_executable(NativeIpcApprovalBrokerTests
        tests/IpcApprovalBrokerTests.cpp
    )
    target_link_libraries(NativeIpcApprovalBrokerTests PRIVATE
        NativeIpcTransport
    )
    if(MSVC)
        target_compile_options(NativeIpcApprovalBrokerTests PRIVATE /W4)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(NativeIpcApprovalBrokerTests PRIVATE -Wall -Wextra)
    endif()
    set_target_properties(NativeIpcApprovalBrokerTests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )
    add_test(NAME native_ipc_approval_broker COMMAND NativeIpcApprovalBrokerTests)

    add_executable(NativeIpcMemServiceDispatcherTests
        tests/IpcMemServiceDispatcherTests.cpp
        mem/Address.cpp
        mem/ValueCodec.cpp
    )
    target_include_directories(NativeIpcMemServiceDispatcherTests PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party
    )
    target_link_libraries(NativeIpcMemServiceDispatcherTests PRIVATE
        NativeIpcTransport
    )
    if(MSVC)
        target_compile_options(NativeIpcMemServiceDispatcherTests PRIVATE /W4)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(NativeIpcMemServiceDispatcherTests PRIVATE -Wall -Wextra)
    endif()
    set_target_properties(NativeIpcMemServiceDispatcherTests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )
    add_test(NAME native_ipc_mem_dispatcher COMMAND NativeIpcMemServiceDispatcherTests)

    add_executable(NativeIpcMethodCatalogTests
        tests/IpcMethodCatalogTests.cpp
    )
    target_link_libraries(NativeIpcMethodCatalogTests PRIVATE
        NativeIpcTransport
    )
    if(MSVC)
        target_compile_options(NativeIpcMethodCatalogTests PRIVATE /W4)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(NativeIpcMethodCatalogTests PRIVATE -Wall -Wextra)
    endif()
    set_target_properties(NativeIpcMethodCatalogTests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )
    add_test(NAME native_ipc_method_catalog COMMAND NativeIpcMethodCatalogTests)

    add_executable(NativeIpcRequestSessionTests
        tests/IpcRequestSessionTests.cpp
    )
    target_include_directories(NativeIpcRequestSessionTests PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party
    )
    target_link_libraries(NativeIpcRequestSessionTests PRIVATE
        NativeIpcTransport
    )
    if(MSVC)
        target_compile_options(NativeIpcRequestSessionTests PRIVATE /W4)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(NativeIpcRequestSessionTests PRIVATE -Wall -Wextra)
    endif()
    set_target_properties(NativeIpcRequestSessionTests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )
    add_test(NAME native_ipc_request_session COMMAND NativeIpcRequestSessionTests)

    add_executable(NativeIpcRequestProtocolTests
        tests/IpcRequestProtocolTests.cpp
    )
    target_include_directories(NativeIpcRequestProtocolTests PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party
    )
    target_link_libraries(NativeIpcRequestProtocolTests PRIVATE
        NativeIpcTransport
    )
    if(MSVC)
        target_compile_options(NativeIpcRequestProtocolTests PRIVATE /W4)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(NativeIpcRequestProtocolTests PRIVATE -Wall -Wextra)
    endif()
    set_target_properties(NativeIpcRequestProtocolTests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )
    add_test(NAME native_ipc_request_protocol COMMAND NativeIpcRequestProtocolTests)

    add_executable(NativeIpcHandshakeTests
        tests/IpcHandshakeSessionTests.cpp
    )
    target_include_directories(NativeIpcHandshakeTests PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party
    )
    target_link_libraries(NativeIpcHandshakeTests PRIVATE
        NativeIpcTransport
    )
    if(MSVC)
        target_compile_options(NativeIpcHandshakeTests PRIVATE /W4)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(NativeIpcHandshakeTests PRIVATE -Wall -Wextra)
    endif()
    set_target_properties(NativeIpcHandshakeTests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )
    add_test(NAME native_ipc_handshake COMMAND NativeIpcHandshakeTests)

    add_executable(NativeIpcFramedIoTests
        tests/IpcFramedConnectionTests.cpp
    )
    target_link_libraries(NativeIpcFramedIoTests PRIVATE
        NativeIpcTransport
    )
    if(MSVC)
        target_compile_options(NativeIpcFramedIoTests PRIVATE /W4)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(NativeIpcFramedIoTests PRIVATE -Wall -Wextra)
    endif()
    set_target_properties(NativeIpcFramedIoTests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )
    add_test(NAME native_ipc_framed_io COMMAND NativeIpcFramedIoTests)

    add_executable(NativeIpcTransportTests
        tests/NativeIpcTransportTests.cpp
    )
    target_link_libraries(NativeIpcTransportTests PRIVATE
        NativeIpcTransport
    )
    if(MSVC)
        target_compile_options(NativeIpcTransportTests PRIVATE /W4)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(NativeIpcTransportTests PRIVATE -Wall -Wextra)
    endif()
    set_target_properties(NativeIpcTransportTests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )
    add_test(NAME native_ipc_transport COMMAND NativeIpcTransportTests)

    add_executable(NativeAgentRuntimeTests
        tests/NativeAgentRuntimeTests.cpp
        mem/Address.cpp
        mem/ValueCodec.cpp
    )
    target_include_directories(NativeAgentRuntimeTests PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party
    )
    target_link_libraries(NativeAgentRuntimeTests PRIVATE
        NativeIpcTransport
    )
    if(MSVC)
        target_compile_options(NativeAgentRuntimeTests PRIVATE /W4)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(NativeAgentRuntimeTests PRIVATE -Wall -Wextra)
    endif()
    set_target_properties(NativeAgentRuntimeTests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )
    add_test(NAME native_agent_runtime COMMAND NativeAgentRuntimeTests)

    add_executable(NativeIpcProtocolTests
        tests/IpcProtocolTests.cpp
        ipc/IpcProtocol.cpp
    )
    target_include_directories(NativeIpcProtocolTests PRIVATE
        ${CMAKE_SOURCE_DIR}
    )
    if(MSVC)
        target_compile_options(NativeIpcProtocolTests PRIVATE /W4)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(NativeIpcProtocolTests PRIVATE -Wall -Wextra)
    endif()
    set_target_properties(NativeIpcProtocolTests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )
    add_test(NAME native_ipc_protocol COMMAND NativeIpcProtocolTests)

    add_executable(NativeAgentMemTests
        tests/MemServiceTests.cpp
        lua/LuaAPI.cpp
        lua/LuaAPI_Memory.cpp
        lua/LuaEngine.cpp
        mem/Address.cpp
        mem/LuaJsonTool.cpp
        mem/LuaOperationContext.cpp
        mem/MemService.cpp
        mem/ValueCodec.cpp
        socket/DeviceSession.cpp
        gui/ai/AgentRunContext.cpp
        gui/ai/AgentTaskExecutor.cpp
        gui/ai/AgentRunner.cpp
        gui/ai/AgentMutationAudit.cpp
        gui/ai/ToolCallSecurity.cpp
        gui/ai/Persistence.cpp
        gui/ai/ProtectedPersistence.cpp
        gui/ai/ProviderResponseLimits.cpp
        gui/ai/ContextBudget.cpp
        gui/ai/ProviderTrust.cpp
        gui/ai/ChatSession.cpp
        gui/ai/AgentController.cpp
        gui/ai/ToolExecutor.cpp
    )
    target_include_directories(NativeAgentMemTests PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/third_party
        ${LUAJIT_INCLUDE}
    )
    target_compile_definitions(NativeAgentMemTests PRIVATE
        HAVE_AI_CHAT
        HAVE_LUAJIT
    )
    if(LUAJIT_IS_STATIC)
        target_compile_definitions(NativeAgentMemTests PRIVATE LUAJIT_STATIC)
    endif()
    target_link_libraries(NativeAgentMemTests PRIVATE
        NativeMemJsonTools
        NativeBoundedJson
        ${LUAJIT_LIBRARY}
        crypt32
    )
    if(MSVC)
        target_compile_options(NativeAgentMemTests PRIVATE /W4)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(NativeAgentMemTests PRIVATE -Wall -Wextra)
    endif()
    set_target_properties(NativeAgentMemTests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )
    add_test(NAME native_agent_mem_service COMMAND NativeAgentMemTests)

    add_executable(NativeAppContextStateTests
        tests/AppContextStateTests.cpp
        gui/AppContext.cpp
    )
    target_include_directories(NativeAppContextStateTests PRIVATE
        ${CMAKE_SOURCE_DIR}
    )
    if(MSVC)
        target_compile_options(NativeAppContextStateTests PRIVATE /W4)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(NativeAppContextStateTests PRIVATE -Wall -Wextra)
    endif()
    set_target_properties(NativeAppContextStateTests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )
    add_test(NAME native_app_context_state COMMAND NativeAppContextStateTests)

    add_executable(NativeSocketClientTransportTests
        tests/SocketClientTransportTests.cpp
        socket/DeviceSession.cpp
    )
    target_include_directories(NativeSocketClientTransportTests PRIVATE
        ${CMAKE_SOURCE_DIR}
    )
    target_link_libraries(NativeSocketClientTransportTests PRIVATE ws2_32)
    if(MSVC)
        target_compile_options(NativeSocketClientTransportTests PRIVATE /W4)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(NativeSocketClientTransportTests PRIVATE -Wall -Wextra)
    endif()
    set_target_properties(NativeSocketClientTransportTests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )
    add_test(
        NAME native_socket_client_transport
        COMMAND NativeSocketClientTransportTests
    )

    add_executable(NativeMultiPortClientManagerTests
        tests/MultiPortClientManagerTests.cpp
        socket/DeviceSession.cpp
        socket/MultiPortClientManager.cpp
        socket/ServerHandshake.cpp
    )
    target_include_directories(NativeMultiPortClientManagerTests PRIVATE
        ${CMAKE_SOURCE_DIR}
    )
    target_link_libraries(NativeMultiPortClientManagerTests PRIVATE ws2_32)
    if(MSVC)
        target_compile_options(NativeMultiPortClientManagerTests PRIVATE /W4)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(NativeMultiPortClientManagerTests PRIVATE -Wall -Wextra)
    endif()
    set_target_properties(NativeMultiPortClientManagerTests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )
    add_test(
        NAME native_multi_port_client_manager
        COMMAND NativeMultiPortClientManagerTests
    )

    add_test(
        NAME native_agent_catalog
        COMMAND ${CMAKE_COMMAND}
            -DSOURCE_ROOT=${CMAKE_SOURCE_DIR}
            -P ${CMAKE_SOURCE_DIR}/tests/VerifyAgentCatalog.cmake
    )
    add_test(
        NAME native_agent_no_python_mcp
        COMMAND ${CMAKE_COMMAND}
            -DSOURCE_ROOT=${CMAKE_SOURCE_DIR}
            -P ${CMAKE_SOURCE_DIR}/tests/VerifyNoPythonMcp.cmake
    )
    add_test(
        NAME native_agent_no_legacy_http_ipc
        COMMAND ${CMAKE_COMMAND}
            -DSOURCE_ROOT=${CMAKE_SOURCE_DIR}
            -P ${CMAKE_SOURCE_DIR}/tests/VerifyNoLegacyHttpIpc.cmake
    )
    add_test(
        NAME native_agent_native_ipc_gate
        COMMAND ${CMAKE_COMMAND}
            -DSOURCE_ROOT=${CMAKE_SOURCE_DIR}
            -P ${CMAKE_SOURCE_DIR}/tests/VerifyNativeIpcGate.cmake
    )
    add_test(
        NAME native_agent_required_features
        COMMAND ${CMAKE_COMMAND}
            -DSOURCE_ROOT=${CMAKE_SOURCE_DIR}
            -P ${CMAKE_SOURCE_DIR}/tests/VerifyRequiredProductFeatures.cmake
    )
    add_test(
        NAME native_gui_mem_service_boundary
        COMMAND ${CMAKE_COMMAND}
            -DSOURCE_ROOT=${CMAKE_SOURCE_DIR}
            -P ${CMAKE_SOURCE_DIR}/tests/VerifyGuiMemServiceBoundary.cmake
    )
    add_test(
        NAME native_context_budget_gate
        COMMAND ${CMAKE_COMMAND}
            -DSOURCE_ROOT=${CMAKE_SOURCE_DIR}
            -P ${CMAKE_SOURCE_DIR}/tests/VerifyContextBudgetGate.cmake
    )
    add_test(
        NAME native_provider_trust_gate
        COMMAND ${CMAKE_COMMAND}
            -DSOURCE_ROOT=${CMAKE_SOURCE_DIR}
            -P ${CMAKE_SOURCE_DIR}/tests/VerifyProviderTrustGate.cmake
    )
    add_test(
        NAME native_session_protection_gate
        COMMAND ${CMAKE_COMMAND}
            -DSOURCE_ROOT=${CMAKE_SOURCE_DIR}
            -P ${CMAKE_SOURCE_DIR}/tests/VerifySessionProtectionGate.cmake
    )
endif()

# ==================== 分发说明 ====================
# 采用 /MT 静态 CRT + 静态第三方库（capstone/keystone/OpenSSL/LuaJIT），exe 完全自包含：
# 无需分发 capstone.dll / keystone.dll / libssl / libcrypto，也无需 VC 运行时 DLL 或 VC++ Redistributable。
# 只依赖 Windows 10/11 x64 自带的系统 DLL。Debug 与 Release 均静态，产物可直接拷贝到其他机器运行。
