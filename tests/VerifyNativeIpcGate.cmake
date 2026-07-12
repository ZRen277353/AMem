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

function(forbid_text source label forbidden)
    string(FIND "${source}" "${forbidden}" position)
    if(NOT position EQUAL -1)
        message(FATAL_ERROR "Unexpected ${label}: ${forbidden}")
    endif()
endfunction()

require_text("${cmake_source}" "default-off native IPC option"
    "option(ENABLE_NATIVE_IPC \"Compile native Named Pipe transport\" OFF)")
require_text("${cmake_source}" "native IPC framed transport sources"
    "add_library(NativeIpcTransport STATIC\n        ipc/IpcFramedConnection.cpp\n        ipc/IpcHandshakeSession.cpp\n        ipc/IpcMemServiceDispatcher.cpp\n        ipc/IpcMethodCatalog.cpp\n        ipc/IpcProtocol.cpp\n        ipc/IpcRequestProtocol.cpp\n        ipc/IpcRequestSession.cpp\n        ipc/NamedPipeServer.cpp")
require_text("${cmake_source}" "owned native Agent runtime source"
    "ipc/NativeAgentRuntime.cpp")
require_text("${cmake_source}" "conditional native IPC product link"
    "if(ENABLE_NATIVE_IPC)\n    target_compile_definitions(ImGuiProject PRIVATE HAVE_NATIVE_IPC)\n    target_link_libraries(ImGuiProject NativeIpcTransport)")
forbid_text("${main_source}" "native IPC automatic startup"
    "NamedPipeServer")
forbid_text("${main_source}" "native IPC runtime gate"
    "HAVE_NATIVE_IPC")

message(STATUS
    "Verified native IPC is compile-time opt-in with no runtime start path")
