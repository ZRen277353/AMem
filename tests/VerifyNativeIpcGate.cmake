cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED SOURCE_ROOT)
    message(FATAL_ERROR "SOURCE_ROOT is required")
endif()

file(READ "${SOURCE_ROOT}/CMakeLists.txt" cmake_source)
file(READ "${SOURCE_ROOT}/main.cpp" main_source)
file(READ "${SOURCE_ROOT}/gui/NativeAgentIpcWindow.cpp" control_source)
file(READ "${SOURCE_ROOT}/ipc/SystemNativeAgentRuntime.cpp" owner_source)
file(READ "${SOURCE_ROOT}/ipc/IpcHandshakeSession.cpp" handshake_source)
file(READ "${SOURCE_ROOT}/ipc/NativeAgentRuntime.cpp" runtime_source)
file(READ "${SOURCE_ROOT}/ipc/IpcRequestSession.cpp" request_session_source)
file(READ "${SOURCE_ROOT}/ipc/IpcMemServiceDispatcher.cpp" dispatcher_source)
file(READ "${SOURCE_ROOT}/ipc/IpcApprovalAudit.h" approval_audit_header)
file(READ "${SOURCE_ROOT}/ipc/IpcApprovalBroker.cpp" approval_broker_source)
file(READ "${SOURCE_ROOT}/gui/ai/ToolDefinitions.cpp" tool_source)
file(READ "${SOURCE_ROOT}/mem/LuaJsonTool.cpp" lua_tool_source)

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
    "add_library(NativeIpcTransport STATIC\n        ipc/IpcApprovalAudit.cpp\n        ipc/IpcApprovalBroker.cpp\n        ipc/IpcFramedConnection.cpp\n        ipc/IpcHandshakeSession.cpp\n        ipc/IpcMemServiceDispatcher.cpp\n        ipc/IpcMethodCatalog.cpp\n        ipc/IpcProtocol.cpp\n        ipc/IpcRequestProtocol.cpp\n        ipc/IpcRequestSession.cpp\n        ipc/NamedPipeServer.cpp")
require_text("${cmake_source}" "owned native Agent runtime source"
    "ipc/NativeAgentRuntime.cpp")
require_text("${cmake_source}" "explicit native IPC GUI control target"
    "add_library(NativeIpcGuiControl STATIC\n        gui/NativeAgentIpcWindow.cpp\n        ipc/SystemNativeAgentRuntime.cpp")
require_text("${cmake_source}" "conditional native IPC product link"
    "if(ENABLE_NATIVE_IPC)\n    target_compile_definitions(ImGuiProject PRIVATE HAVE_NATIVE_IPC)\n    target_link_libraries(ImGuiProject NativeIpcTransport NativeIpcGuiControl)")
require_text("${main_source}" "native IPC joined shutdown gate"
    "#ifdef HAVE_NATIVE_IPC\n    // Stop the opt-in pipe and its approved work before device teardown.\n    NativeIpc::ShutdownSystemNativeAgentRuntime();\n#endif")
forbid_text("${main_source}" "native IPC automatic startup"
    "GetSystemNativeAgentRuntime().start")
require_text("${control_source}" "user-initiated native IPC startup"
    "if (runtime.start(error))")
require_text("${control_source}" "visible per-request privileged approval"
    "textRow(\"特权能力\", \"逐请求审批\")")
require_text("${control_source}" "bounded GUI approval decision"
    "NativeIpc::DecideSystemIpcApproval(")
require_text("${control_source}" "approval-aware runtime stop"
    "NativeIpc::StopSystemNativeAgentRuntime();")
forbid_text("${control_source}" "raw approval params in GUI"
    "params")
forbid_text("${control_source}" "raw approval result in GUI"
    "resultJson")
require_text("${owner_source}" "stop-time approval invalidation"
    "approvalBroker->cancelAll();")
require_text("${owner_source}" "runtime receives the system broker"
    "RequestSessionConfig{}, approvalBroker_.get(), hostExecutor_.get())")
require_text("${owner_source}" "runtime receives shared Lua host execution"
    "Mem::executeLuaJson(service_, paramsJson, context)")
require_text("${owner_source}" "persistent approval audit injection"
    "IpcApprovalBrokerConfig{},\n                                              approvalAudit_.get())")
require_text("${control_source}" "visible approval audit status"
    "GetSystemIpcApprovalAuditSnapshot()")
forbid_text("${approval_audit_header}" "approval audit raw params"
    "params")
forbid_text("${approval_audit_header}" "approval audit raw results"
    "resultJson")
require_text("${approval_broker_source}" "durable consumed transition"
    "const bool durable = audit(changed);")
require_text("${approval_broker_source}" "fail-closed consumed grant"
    "changed.state == IpcApprovalState::Consumed && durable")
require_text("${approval_broker_source}" "stable consumed audit failure"
    "approval_audit_failed")
require_text("${runtime_source}" "session-scoped approval cancellation"
    "approvalBroker_->cancelSession(sessionId);")
forbid_text("${runtime_source}" "runtime privileged submission"
    "approvalBroker_->submit")
require_text("${request_session_source}" "server-owned approval submission gate"
    "dispatcher_.canSubmitForApproval(")
require_text("${dispatcher_source}" "privileged request submission"
    "approvalBroker_->submit(submission)")
require_text("${dispatcher_source}" "privileged approval consumption"
    "approvalBroker_->consume(")
require_text("${dispatcher_source}" "grant-bound privileged execution"
    "return executeApproved(request, context, descriptor,")
forbid_text("${dispatcher_source}" "obsolete disabled execution response"
    "approval_execution_disabled")
require_text("${tool_source}" "in-app shared Lua host tool"
    "Mem::executeLuaJson(")
require_text("${lua_tool_source}" "shared Lua target revalidation"
    "return contextErrorJson(context, service.captureContext(true));")
require_text("${handshake_source}" "Observe-only grant remains fixed"
    "if (capability == IpcCapability::Observe) {\n            result.grantedCapabilities.push_back(capability);")

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
