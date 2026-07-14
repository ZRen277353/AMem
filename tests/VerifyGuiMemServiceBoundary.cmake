if(NOT DEFINED SOURCE_ROOT)
    message(FATAL_ERROR "SOURCE_ROOT is required")
endif()

file(GLOB_RECURSE gui_sources LIST_DIRECTORIES false
    "${SOURCE_ROOT}/gui/*.h"
    "${SOURCE_ROOT}/gui/*.cpp")
file(GLOB_RECURSE lua_sources LIST_DIRECTORIES false
    "${SOURCE_ROOT}/lua/*.h"
    "${SOURCE_ROOT}/lua/*.cpp")
set(frontend_sources
    ${gui_sources}
    ${lua_sources}
    "${SOURCE_ROOT}/main.cpp")

set(protocol_markers
    "client_singleton\\.h"
    "SocketCommand::"
    "GetSocketMgr\\("
    "PORT_(MAIN|DEBUG|ERROR)"
    "FetchServerVersion\\("
    "GetMemType\\("
    "InitDriver\\("
    "FetchProcessList\\("
    "SetCurrentPid\\("
    "GetCurrentPid\\("
    "OpenProcessHandle\\("
    "CloseProcessHandle\\("
    "FetchModuleList\\("
    "ReadProcessMemoryBytes\\("
    "ReadBratchAddr\\("
    "WriteProcessMemoryBytes\\("
    "ResolveModuleOffsetChain\\("
    "ClearScanResult\\("
    "SetKernelBreakpoint\\("
    "RemoveKernelBreakpoint\\("
    "SuspendKernelBreakpoint\\("
    "ResumeKernelBreakpoint\\("
    "ReadKernelBreakpointInfo"
    "Freeze(Add|Remove|Update|Clear)\\("
    "Symbol(Init|GetList|Find)\\(")

foreach(source IN LISTS frontend_sources)
    file(READ "${source}" content)
    foreach(marker IN LISTS protocol_markers)
        string(REPLACE "\\" "/" normalized "${source}")
        if(normalized MATCHES "/lua/" AND
           marker STREQUAL "GetCurrentPid\\(")
            continue()
        endif()
        if(content MATCHES "${marker}")
            file(RELATIVE_PATH relative "${SOURCE_ROOT}" "${source}")
            message(FATAL_ERROR
                "front-end MemService boundary violation in ${relative}: ${marker}")
        endif()
    endforeach()

    if(NOT normalized MATCHES "/gui/ai/" AND
       NOT normalized MATCHES "/gui/Gui\\.cpp$" AND
       NOT normalized MATCHES "/main\\.cpp$")
        if(content MATCHES "getSystemMemService\\(")
            file(RELATIVE_PATH relative "${SOURCE_ROOT}" "${source}")
            message(FATAL_ERROR
                "front-end service dependencies must be injected in ${relative}")
        endif()
    endif()
endforeach()

file(READ "${SOURCE_ROOT}/lua/LuaEngine.h" lua_engine_header)
file(READ "${SOURCE_ROOT}/lua/LuaEngine.cpp" lua_engine_source)
file(READ "${SOURCE_ROOT}/mem/LuaJsonTool.cpp" lua_tool_source)
foreach(required IN ITEMS
        "Initialize(Mem::IMemService& memService)"
        "Mem::OperationContext* context = nullptr")
    string(FIND "${lua_engine_header}" "${required}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR
            "LuaEngine service/context injection contract is missing: ${required}")
    endif()
endforeach()
foreach(required IN ITEMS
        "LuaAPI::BindOperationContext(L, context)"
        "LuaAPI::BindOperationContext(L, previousContext)")
    string(FIND "${lua_engine_source}" "${required}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR
            "Lua execution context binding is missing: ${required}")
    endif()
endforeach()
string(FIND "${lua_tool_source}" "&scriptContext))" lua_context_position)
if(lua_context_position EQUAL -1)
    message(FATAL_ERROR
        "LuaJsonTool must bind its mutable approved OperationContext for execution")
endif()
file(READ "${SOURCE_ROOT}/lua/LuaAPI.cpp" lua_api_source)
foreach(required IN ITEMS
        "advanceLuaOperationTarget("
        "context.target = selectedTarget;")
    string(FIND "${lua_api_source}${lua_tool_source}" "${required}" position)
    if(position EQUAL -1)
        file(READ "${SOURCE_ROOT}/mem/LuaOperationContext.cpp" lua_context_source)
        string(FIND "${lua_context_source}" "${required}" context_position)
        if(context_position EQUAL -1)
            message(FATAL_ERROR
                "Lua controlled target selection is missing: ${required}")
        endif()
    endif()
endforeach()

foreach(app_context IN ITEMS
        "${SOURCE_ROOT}/gui/AppContext.h"
        "${SOURCE_ROOT}/gui/AppContext.cpp")
    file(READ "${app_context}" content)
    if(content MATCHES "socket/|PORT_|client_singleton|SocketCommand")
        message(FATAL_ERROR
            "AppContext must remain target/presentation state only: ${app_context}")
    endif()
endforeach()

file(READ "${SOURCE_ROOT}/mem/SystemMemService.cpp" system_service_source)
foreach(required IN ITEMS
        "auto requestLease = GetSocketMgr().AcquireRequestLease();"
        "auto mutation = AppContext::Get().beginTargetMutation("
        "SocketCommand::TransactionLease debugTransaction(PORT_DEBUG);"
        "SocketCommand::TransactionLease mainTransaction(PORT_MAIN);"
        "mutation->publish(pid, handle, name);")
    string(FIND "${system_service_source}" "${required}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR
            "transactional process switching contract is missing: ${required}")
    endif()
endforeach()

file(GLOB_RECURSE non_context_sources LIST_DIRECTORIES false
    "${SOURCE_ROOT}/gui/*.h"
    "${SOURCE_ROOT}/gui/*.cpp"
    "${SOURCE_ROOT}/socket/*.h"
    "${SOURCE_ROOT}/socket/*.cpp"
    "${SOURCE_ROOT}/mem/*.h"
    "${SOURCE_ROOT}/mem/*.cpp")
foreach(source IN LISTS non_context_sources)
    string(REPLACE "\\" "/" normalized "${source}")
    if(normalized MATCHES "/gui/AppContext\\.(h|cpp)$")
        continue()
    endif()
    file(READ "${source}" content)
    if(content MATCHES "(selectedPid|processHandle)\\.store\\(")
        file(RELATIVE_PATH relative "${SOURCE_ROOT}" "${source}")
        message(FATAL_ERROR
            "target state may only be published by AppContext: ${relative}")
    endif()
endforeach()

message(STATUS "Verified GUI/Lua/main -> IMemService boundary")
