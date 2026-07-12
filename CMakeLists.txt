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

# AI 聊天集成选项（Requirement 13.4）
option(ENABLE_AI_CHAT "Enable AI chat integration" ON)

# 旧 loopback HTTP IPC 仅用于迁移验证。默认构建不编译也不监听 28100。
option(ENABLE_LEGACY_HTTP_IPC "Enable legacy unauthenticated HTTP IPC" OFF)

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
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/lib)

# 查找 Capstone 库（完全静态）：优先官方安装器静态版（C:/Program Files/capstone，6.x，/MT），
# 整体链入 exe，不分发 DLL；也接受 vcpkg 静态三元组 x64-windows-static（5.x，/MT）。
# 切勿使用 vcpkg x64-windows 动态版（/MD 导入库 + DLL）——会与 /MT 冲突且需分发 capstone.dll。
set(capstone_ROOT "C:/Program Files/capstone" CACHE PATH "Capstone installation directory")

find_path(CAPSTONE_INCLUDE_DIR NAMES capstone/capstone.h
    HINTS "${capstone_ROOT}/include" "C:/vcpkg/installed/x64-windows-static/include" PATH_SUFFIXES include)
find_library(CAPSTONE_LIBRARY NAMES capstone capstone_static
    HINTS "${capstone_ROOT}/lib" "C:/vcpkg/installed/x64-windows-static/lib" PATH_SUFFIXES lib lib64)

if(CAPSTONE_INCLUDE_DIR AND CAPSTONE_LIBRARY)
    set(HAVE_CAPSTONE TRUE)
    set(CAPSTONE_LIBRARIES ${CAPSTONE_LIBRARY})
    set(CAPSTONE_INCLUDE_DIRS ${CAPSTONE_INCLUDE_DIR})
else()
    set(HAVE_CAPSTONE FALSE)
    message(WARNING "Capstone not found! Disassembly features will be disabled.")
endif()

# 查找 Keystone 库（完全静态，vcpkg x64-windows-static，/MT）
set(keystone_ROOT "C:/vcpkg/installed/x64-windows-static" CACHE PATH "Keystone installation directory")

find_path(KEYSTONE_INCLUDE_DIR keystone/keystone.h
    HINTS "${keystone_ROOT}/include" "${keystone_ROOT}" PATH_SUFFIXES include)
find_library(KEYSTONE_LIBRARY NAMES keystone
    HINTS "${keystone_ROOT}/lib" "${keystone_ROOT}" PATH_SUFFIXES lib lib64)

if(KEYSTONE_INCLUDE_DIR AND KEYSTONE_LIBRARY)
    set(HAVE_KEYSTONE TRUE)
    set(KEYSTONE_LIBRARIES ${KEYSTONE_LIBRARY})
    set(KEYSTONE_INCLUDE_DIRS ${KEYSTONE_INCLUDE_DIR})
else()
    set(HAVE_KEYSTONE FALSE)
    message(WARNING "Keystone not found! Assembly features will be disabled.")
endif()

# ==================== AI Chat Integration ====================
# Requirements: 13.1, 13.2, 13.3, 13.4, 13.5
set(HAVE_AI_CHAT FALSE)
if(ENABLE_AI_CHAT)
    set(HTTPLIB_HEADER "${CMAKE_SOURCE_DIR}/third_party/httplib/httplib.h")
    set(NLOHMANN_JSON_HEADER "${CMAKE_SOURCE_DIR}/third_party/nlohmann/json.hpp")

    if(EXISTS "${HTTPLIB_HEADER}" AND EXISTS "${NLOHMANN_JSON_HEADER}")
        # cpp-httplib 需要 OpenSSL 才能支持 HTTPS（CPPHTTPLIB_OPENSSL_SUPPORT）
        # 完全静态：使用 vcpkg 静态 OpenSSL（x64-windows-static，/MT），链入 exe 不分发 DLL。
        # 不用 x64-windows-static-md（那是 /MD，会与本项目 /MT 冲突）。
        # 注意：不要设 OPENSSL_USE_STATIC_LIBS —— clang（GNU 驱动）下 MSVC=FALSE，
        # FindOpenSSL 会把查找后缀硬改成 .a，永远匹配不到 vcpkg 的 libssl.lib/libcrypto.lib；
        # 该三元组目录下只有静态库，按默认 .lib 后缀即可找到正确文件。
        if(NOT OPENSSL_ROOT_DIR AND EXISTS "C:/vcpkg/installed/x64-windows-static/include/openssl/ssl.h")
            set(OPENSSL_ROOT_DIR "C:/vcpkg/installed/x64-windows-static")
            set(AMEM_OPENSSL_STATIC TRUE)
        endif()
        find_package(OpenSSL QUIET)

        if(OPENSSL_FOUND)
            set(HAVE_AI_CHAT TRUE)
            message(STATUS "AI chat enabled: cpp-httplib + nlohmann/json + OpenSSL found")
        else()
            message(WARNING "AI chat features will be disabled: OpenSSL not found (required for HTTPS). Install via 'vcpkg install openssl:x64-windows'.")
        endif()
    else()
        if(NOT EXISTS "${HTTPLIB_HEADER}")
            message(WARNING "AI chat features will be disabled: cpp-httplib header not found at ${HTTPLIB_HEADER}")
        endif()
        if(NOT EXISTS "${NLOHMANN_JSON_HEADER}")
            message(WARNING "AI chat features will be disabled: nlohmann/json header not found at ${NLOHMANN_JSON_HEADER}")
        endif()
    endif()
else()
    message(WARNING "AI chat features explicitly disabled via ENABLE_AI_CHAT=OFF")
endif()

# AI 聊天模块源文件列表（部分文件由后续任务实现，使用 EXISTS 过滤以避免配置期错误）
set(AI_CHAT_SOURCES_CANDIDATES
    gui/ai/ProviderRegistry.cpp
    gui/ai/HttpClient.cpp
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
    gui/ai/AgentMemTools.cpp
    gui/ai/AgentMemToolsSystem.cpp
    gui/ai/ChatWindow.cpp
    gui/ai/ChatWindowSettings.cpp
)
set(AI_CHAT_SOURCES "")
foreach(_ai_src IN LISTS AI_CHAT_SOURCES_CANDIDATES)
    if(EXISTS "${CMAKE_SOURCE_DIR}/${_ai_src}")
        list(APPEND AI_CHAT_SOURCES "${_ai_src}")
    endif()
endforeach()

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

if(HAVE_AI_CHAT)
    list(APPEND GUI_SOURCES ${AI_CHAT_SOURCES})
endif()

set(SOCKET_SOURCES
    socket/DeviceSession.cpp
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
    mem/MemService.cpp
    mem/SystemMemService.cpp
    mem/ValueCodec.cpp
)

set(RENDERER_SOURCES
    renderer/DX12Renderer.cpp
    renderer/StyleSetup.cpp
)

set(IPC_SOURCES)
if(ENABLE_LEGACY_HTTP_IPC)
    list(APPEND IPC_SOURCES ipc/IpcServer.cpp)
    message(WARNING
        "Legacy HTTP IPC enabled: unauthenticated loopback port 28100 will be exposed")
else()
    message(STATUS "Legacy HTTP IPC disabled")
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
    ${IPC_SOURCES}
    ${LUA_SOURCES}
)

if(ENABLE_LEGACY_HTTP_IPC)
    target_compile_definitions(ImGuiProject PRIVATE HAVE_LEGACY_HTTP_IPC)
endif()

# 包含目录
target_include_directories(ImGuiProject PRIVATE
    imgui imgui/backends gui lua mem renderer utils ipc
    ${CMAKE_SOURCE_DIR}/third_party
    ${CMAKE_BINARY_DIR}/generated
)

if(HAVE_CAPSTONE)
    target_include_directories(ImGuiProject PRIVATE ${CAPSTONE_INCLUDE_DIRS})
endif()

if(HAVE_KEYSTONE)
    target_include_directories(ImGuiProject PRIVATE ${KEYSTONE_INCLUDE_DIRS})
endif()

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

# 链接 Capstone 库（静态 /MT，整体链入 exe，不分发 DLL）
if(HAVE_CAPSTONE)
    target_link_libraries(ImGuiProject ${CAPSTONE_LIBRARIES})
    target_compile_definitions(ImGuiProject PRIVATE HAVE_CAPSTONE)
endif()

# 链接 Keystone 库（静态 /MT，整体链入 exe，不分发 DLL）
if(HAVE_KEYSTONE)
    target_link_libraries(ImGuiProject ${KEYSTONE_LIBRARIES})
    target_compile_definitions(ImGuiProject PRIVATE HAVE_KEYSTONE)
endif()

# 链接 AI 聊天依赖（Requirements 13.1, 13.3）
if(HAVE_AI_CHAT)
    target_compile_definitions(ImGuiProject PRIVATE
        HAVE_AI_CHAT
        CPPHTTPLIB_OPENSSL_SUPPORT
    )
    target_include_directories(ImGuiProject PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party/httplib
        ${OPENSSL_INCLUDE_DIR}
    )
    # crypt32 for Windows DPAPI (CryptProtectData/CryptUnprotectData in ApiKeyStore)
    target_link_libraries(ImGuiProject OpenSSL::SSL OpenSSL::Crypto crypt32)

    # 静态 OpenSSL（libssl/libcrypto）在 Windows 上额外依赖的系统库。
    # keystone 静态库自带的 /DEFAULTLIB 指令（ole32/uuid/advapi32 等）由链接器自动采纳，此处只补 OpenSSL 所需。
    if(AMEM_OPENSSL_STATIC)
        target_link_libraries(ImGuiProject ws2_32 advapi32 user32 gdi32 bcrypt)
    endif()
endif()

if(BUILD_TESTING)
    add_executable(NativeAgentMemTests
        tests/MemServiceTests.cpp
        mem/Address.cpp
        mem/MemService.cpp
        mem/ValueCodec.cpp
        socket/DeviceSession.cpp
        gui/ai/AgentMemTools.cpp
        gui/ai/AgentRunContext.cpp
        gui/ai/AgentTaskExecutor.cpp
        gui/ai/AgentRunner.cpp
        gui/ai/AgentMutationAudit.cpp
        gui/ai/ToolCallSecurity.cpp
        gui/ai/ChatSession.cpp
        gui/ai/AgentController.cpp
        gui/ai/ToolExecutor.cpp
    )
    target_include_directories(NativeAgentMemTests PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/third_party
    )
    target_compile_definitions(NativeAgentMemTests PRIVATE HAVE_AI_CHAT)
    if(MSVC)
        target_compile_options(NativeAgentMemTests PRIVATE /W4)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(NativeAgentMemTests PRIVATE -Wall -Wextra)
    endif()
    set_target_properties(NativeAgentMemTests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )
    add_test(NAME native_agent_mem_service COMMAND NativeAgentMemTests)
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
        NAME native_agent_legacy_ipc_gate
        COMMAND ${CMAKE_COMMAND}
            -DSOURCE_ROOT=${CMAKE_SOURCE_DIR}
            -P ${CMAKE_SOURCE_DIR}/tests/VerifyLegacyHttpIpcGate.cmake
    )
endif()

# ==================== 分发说明 ====================
# 采用 /MT 静态 CRT + 静态第三方库（capstone/keystone/OpenSSL/LuaJIT），exe 完全自包含：
# 无需分发 capstone.dll / keystone.dll / libssl / libcrypto，也无需 VC 运行时 DLL 或 VC++ Redistributable。
# 只依赖 Windows 10/11 x64 自带的系统 DLL。Debug 与 Release 均静态，产物可直接拷贝到其他机器运行。
