cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED SOURCE_ROOT)
    message(FATAL_ERROR "SOURCE_ROOT is required")
endif()

file(READ "${SOURCE_ROOT}/CMakeLists.txt" cmake_source)
file(READ "${SOURCE_ROOT}/main.cpp" main_source)

function(require_text source label expected)
    string(FIND "${source}" "${expected}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "Missing ${label}: ${expected}")
    endif()
endfunction()

require_text("${cmake_source}" "default-off option"
    "option(ENABLE_LEGACY_HTTP_IPC \"Enable legacy unauthenticated HTTP IPC\" OFF)")
require_text("${cmake_source}" "conditional IPC source"
    "if(ENABLE_LEGACY_HTTP_IPC)\n    list(APPEND IPC_SOURCES ipc/IpcServer.cpp)")
require_text("${cmake_source}" "conditional compile definition"
    "if(ENABLE_LEGACY_HTTP_IPC)\n    target_compile_definitions(ImGuiProject PRIVATE HAVE_LEGACY_HTTP_IPC)")
require_text("${main_source}" "guarded IPC include"
    "#ifdef HAVE_LEGACY_HTTP_IPC\n#include \"ipc/IpcServer.h\"\n#endif")
require_text("${main_source}" "guarded IPC start"
    "#ifdef HAVE_LEGACY_HTTP_IPC\n    // Temporary migration-only endpoint. Default builds exclude this path.\n    IpcServer::GetInstance().Start(28100);\n#endif")
require_text("${main_source}" "guarded IPC stop"
    "#ifdef HAVE_LEGACY_HTTP_IPC\n    IpcServer::GetInstance().Stop();\n#endif")

message(STATUS "Verified legacy HTTP IPC is explicit and default-off")
