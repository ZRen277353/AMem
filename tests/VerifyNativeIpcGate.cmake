cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED SOURCE_ROOT)
    message(FATAL_ERROR "SOURCE_ROOT is required")
endif()

file(READ "${SOURCE_ROOT}/CMakeLists.txt" cmake_source)
file(READ "${SOURCE_ROOT}/main.cpp" main_source)
file(READ "${SOURCE_ROOT}/gui/NativeAgentIpcWindow.cpp" control_source)

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
    "add_library(NativeIpcTransport STATIC\n        ipc/IpcApprovalBroker.cpp\n        ipc/IpcFramedConnection.cpp\n        ipc/IpcHandshakeSession.cpp\n        ipc/IpcMemServiceDispatcher.cpp\n        ipc/IpcMethodCatalog.cpp\n        ipc/IpcProtocol.cpp\n        ipc/IpcRequestProtocol.cpp\n        ipc/IpcRequestSession.cpp\n        ipc/NamedPipeServer.cpp")
require_text("${cmake_source}" "owned native Agent runtime source"
    "ipc/NativeAgentRuntime.cpp")
require_text("${cmake_source}" "explicit native IPC GUI control target"
    "add_library(NativeIpcGuiControl STATIC\n        gui/NativeAgentIpcWindow.cpp\n        ipc/SystemNativeAgentRuntime.cpp")
require_text("${cmake_source}" "conditional native IPC product link"
    "if(ENABLE_NATIVE_IPC)\n    target_compile_definitions(ImGuiProject PRIVATE HAVE_NATIVE_IPC)\n    target_link_libraries(ImGuiProject NativeIpcTransport NativeIpcGuiControl)")
require_text("${main_source}" "native IPC joined shutdown gate"
    "#ifdef HAVE_NATIVE_IPC\n    // Stop the explicit Observe-only pipe before device lifecycle teardown.\n    NativeIpc::ShutdownSystemNativeAgentRuntime();\n#endif")
forbid_text("${main_source}" "native IPC automatic startup"
    "GetSystemNativeAgentRuntime().start")
require_text("${control_source}" "user-initiated native IPC startup"
    "if (runtime.start(error))")
require_text("${control_source}" "visible Observe-only capability"
    "textRow(\"特权能力\", \"禁用\")")

string(FIND "${main_source}"
    "NativeIpc::ShutdownSystemNativeAgentRuntime();" shutdown_position)
string(FIND "${main_source}" "DisconnectMultiPort();" disconnect_position)
if(shutdown_position EQUAL -1 OR disconnect_position EQUAL -1 OR
   shutdown_position GREATER disconnect_position)
    message(FATAL_ERROR
        "Native IPC shutdown must precede device disconnect")
endif()

message(STATUS
    "Verified native IPC is compile-time opt-in with explicit Observe-only GUI control")
