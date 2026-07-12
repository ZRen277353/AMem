#include "ipc/IpcApprovalBroker.h"
#include "ipc/NativeAgentRuntime.h"

#include <nlohmann/json.hpp>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using json = nlohmann::json;
using namespace std::chrono_literals;

void expect(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

template <typename Predicate>
bool waitUntil(Predicate predicate, DWORD timeout = 5000) {
  const ULONGLONG deadline = ::GetTickCount64() + timeout;
  do {
    if (predicate()) {
      return true;
    }
    ::Sleep(1);
  } while (::GetTickCount64() < deadline);
  return predicate();
}

std::wstring uniquePipeName(const wchar_t *suffix) {
  return std::wstring(L"\\\\.\\pipe\\AMem.NativeAgent.RuntimeTest.") +
         std::to_wstring(::GetCurrentProcessId()) + L"." + suffix;
}

HANDLE connectPipe(const std::wstring &name) {
  const ULONGLONG deadline = ::GetTickCount64() + 5000;
  do {
    HANDLE pipe =
        ::CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                      OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (pipe != INVALID_HANDLE_VALUE) {
      return pipe;
    }
    if (::GetLastError() != ERROR_PIPE_BUSY) {
      return INVALID_HANDLE_VALUE;
    }
    ::WaitNamedPipeW(name.c_str(), 100);
  } while (::GetTickCount64() < deadline);
  return INVALID_HANDLE_VALUE;
}

class StubMemService final : public Mem::IMemService {
public:
  StubMemService() {
    current_.connectionGeneration = 7;
    current_.target = Mem::TargetSnapshot{42, 420, 2, 7};
    current_.deadline = (std::chrono::steady_clock::time_point::max)();
  }

  Mem::OperationContext captureContext(bool includeTarget) const override {
    std::lock_guard<std::mutex> lock(contextMutex_);
    Mem::OperationContext result = current_;
    if (!includeTarget) {
      result.target.reset();
    }
    result.cancellation.reset();
    result.deadline = (std::chrono::steady_clock::time_point::max)();
    return result;
  }

  Mem::Result<Mem::Status>
  status(const Mem::OperationContext &context) override {
    ++statusCalls_;
    statusStarted_.store(true, std::memory_order_release);
    while (blockStatus_.load(std::memory_order_acquire) &&
           (!context.cancellation ||
            !context.cancellation->load(std::memory_order_acquire))) {
      ::Sleep(1);
    }
    if (context.cancellation &&
        context.cancellation->load(std::memory_order_acquire)) {
      cancellationObserved_.store(true, std::memory_order_release);
      return Mem::Result<Mem::Status>::failure(
          Mem::ErrorCode::CancelRequested, "runtime test observed cancellation",
          false, 1);
    }

    const Mem::OperationContext current = captureContext(true);
    Mem::Status value;
    value.connected = true;
    value.connectionGeneration = current.connectionGeneration;
    value.target = *current.target;
    value.processName = "com.example.game";
    return Mem::Result<Mem::Status>::success(std::move(value), 1);
  }

  Mem::Result<Mem::ProcessPage>
  listProcesses(const Mem::OperationContext &context,
                const Mem::ProcessListRequest &) override {
    return unsupported<Mem::ProcessPage>(context);
  }

#define STUB_METHOD(method, resultType, requestType)                           \
  Mem::Result<Mem::resultType> method(const Mem::OperationContext &context,    \
                                      const Mem::requestType &) override {     \
    return unsupported<Mem::resultType>(context);                              \
  }

  STUB_METHOD(initializeDriver, DriverInitializationReceipt,
              DriverInitializeRequest)
  STUB_METHOD(openProcess, OpenProcessResult, OpenProcessRequest)
  STUB_METHOD(listModules, ModulePage, ModuleListRequest)
  STUB_METHOD(resolveModule, ResolvedModule, ModuleResolveRequest)
  STUB_METHOD(resolvePointer, PointerResolution, PointerResolveRequest)
  STUB_METHOD(disassemble, DisassemblyBlock, DisassemblyRequest)
  STUB_METHOD(resolveSymbol, ResolvedSymbol, SymbolResolveRequest)
  STUB_METHOD(listSymbols, SymbolPage, SymbolListRequest)
  STUB_METHOD(loadSymbolTable, SymbolTable, SymbolTableRequest)
  STUB_METHOD(setBreakpoint, BreakpointMutationReceipt, BreakpointSetRequest)
  STUB_METHOD(removeBreakpoint, BreakpointMutationReceipt,
              BreakpointAddressRequest)
  STUB_METHOD(suspendBreakpoint, BreakpointMutationReceipt,
              BreakpointAddressRequest)
  STUB_METHOD(resumeBreakpoint, BreakpointMutationReceipt,
              BreakpointAddressRequest)
  STUB_METHOD(breakpointHitBatch, BreakpointHitBatch, BreakpointHitBatchRequest)
  STUB_METHOD(scanResults, ScanResultPage, ScanResultsRequest)
  STUB_METHOD(clearScan, ScanClearResult, ScanClearRequest)
  STUB_METHOD(removeScanResults, ScanRemoveResult, ScanRemoveRequest)
  STUB_METHOD(readMemory, MemoryBlock, MemoryReadRequest)
  STUB_METHOD(readValue, ScalarValue, ValueReadRequest)
  STUB_METHOD(writeValue, WriteReceipt, ValueWriteRequest)

#undef STUB_METHOD

  Mem::Result<Mem::WriteReceipt>
  writeMemory(const Mem::OperationContext &context,
              const Mem::MemoryWriteRequest &) override {
    ++writeCalls_;
    return unsupported<Mem::WriteReceipt>(context);
  }

  Mem::Result<Mem::ScanSummary>
  startScan(const Mem::OperationContext &context, const Mem::ScanStartRequest &,
            const Mem::ScanProgressSink & = {}) override {
    return unsupported<Mem::ScanSummary>(context);
  }

  Mem::Result<Mem::ScanSummary>
  refineScan(const Mem::OperationContext &context,
             const Mem::ScanRefineRequest &,
             const Mem::ScanProgressSink & = {}) override {
    return unsupported<Mem::ScanSummary>(context);
  }

  void invalidateTarget() {
    std::lock_guard<std::mutex> lock(contextMutex_);
    ++current_.target->processRevision;
  }

  void setBlockStatus(bool block) {
    blockStatus_.store(block, std::memory_order_release);
    statusStarted_.store(false, std::memory_order_release);
    cancellationObserved_.store(false, std::memory_order_release);
  }

  bool statusStarted() const {
    return statusStarted_.load(std::memory_order_acquire);
  }

  bool cancellationObserved() const {
    return cancellationObserved_.load(std::memory_order_acquire);
  }

  int statusCalls() const {
    return statusCalls_.load(std::memory_order_acquire);
  }

  int writeCalls() const {
    return writeCalls_.load(std::memory_order_acquire);
  }

private:
  template <typename T>
  Mem::Result<T> unsupported(const Mem::OperationContext &) {
    return Mem::Result<T>::failure(Mem::ErrorCode::Unsupported,
                                   "unsupported by runtime test service", false,
                                   1);
  }

  mutable std::mutex contextMutex_;
  Mem::OperationContext current_;
  std::atomic<bool> blockStatus_{false};
  std::atomic<bool> statusStarted_{false};
  std::atomic<bool> cancellationObserved_{false};
  std::atomic<int> statusCalls_{0};
  std::atomic<int> writeCalls_{0};
};

class RuntimeClient final {
public:
  explicit RuntimeClient(const std::wstring &pipeName) {
    stopEvent_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    expect(stopEvent_ != nullptr, "client stop event should be created");
    pipe_ = connectPipe(pipeName);
    expect(pipe_ != INVALID_HANDLE_VALUE, "runtime client should connect");
    connection_ =
        std::make_unique<NativeIpc::IpcFramedConnection>(pipe_, stopEvent_);
  }

  ~RuntimeClient() {
    close();
    if (stopEvent_ != nullptr) {
      ::CloseHandle(stopEvent_);
    }
  }

  RuntimeClient(const RuntimeClient &) = delete;
  RuntimeClient &operator=(const RuntimeClient &) = delete;

  json hello(const std::string &clientName = "AMem.RuntimeTests",
             const json &capabilities = json::array({"Observe"})) {
    send(IpcProtocol::MessageType::Hello, 0,
         json({{"client_name", clientName},
               {"client_version", "1.0"},
               {"requested_capabilities", capabilities}})
             .dump(),
         NativeIpc::kMaxHandshakePayloadBytes);
    const IpcProtocol::Frame response =
        read(NativeIpc::kMaxHandshakePayloadBytes);
    expect(response.type == IpcProtocol::MessageType::HelloAck,
           "valid Hello should receive HelloAck");
    return json::parse(response.payload);
  }

  void send(IpcProtocol::MessageType type, uint64_t requestId,
            const std::string &payload,
            uint32_t maxPayload = IpcProtocol::kMaxFramePayloadBytes) {
    expect(connection_ != nullptr, "client connection should be open");
    const auto result = connection_->writeFrame(
        {type, requestId, payload}, std::chrono::steady_clock::now() + 5s,
        maxPayload);
    expect(result.status == NativeIpc::FrameIoStatus::Complete,
           "client frame should be written");
  }

  void request(uint64_t requestId, const std::string &method,
               const json &params = json::object()) {
    send(IpcProtocol::MessageType::Request, requestId,
         json({{"method", method}, {"params", params}, {"timeout_ms", 5000}})
             .dump());
  }

  IpcProtocol::Frame
  read(uint32_t maxPayload = IpcProtocol::kMaxFramePayloadBytes,
       const char *operation = "runtime response") {
    expect(connection_ != nullptr, "client connection should be open");
    const auto result = connection_->readFrame(
        std::chrono::steady_clock::now() + 5s, maxPayload);
    expect(result.status == NativeIpc::FrameIoStatus::Complete,
           std::string(operation) + " should be readable, status=" +
               std::to_string(static_cast<int>(result.status)) +
               ", error=" +
               std::string(result.error.begin(), result.error.end()));
    return result.frame;
  }

  void close() {
    connection_.reset();
    if (pipe_ != INVALID_HANDLE_VALUE) {
      ::CloseHandle(pipe_);
      pipe_ = INVALID_HANDLE_VALUE;
    }
  }

private:
  HANDLE stopEvent_ = nullptr;
  HANDLE pipe_ = INVALID_HANDLE_VALUE;
  std::unique_ptr<NativeIpc::IpcFramedConnection> connection_;
};

NativeIpc::RequestSessionConfig fastSessionConfig() {
  NativeIpc::RequestSessionConfig config;
  config.validationInterval = 10ms;
  return config;
}

void startRuntime(NativeIpc::NativeAgentRuntime &runtime) {
  std::wstring error;
  expect(runtime.start(error), "native Agent runtime should start");
  const auto snapshot = runtime.snapshot();
  expect(snapshot.server.state == NativeIpc::ServerState::Listening &&
             snapshot.phase == NativeIpc::RuntimePhase::Idle &&
             !snapshot.server.pipeName.empty(),
         "started runtime should expose listening status");
}

json responseJson(const IpcProtocol::Frame &frame,
                  IpcProtocol::MessageType type, uint64_t requestId) {
  expect(frame.type == type && frame.requestId == requestId,
         "runtime response envelope mismatch");
  return json::parse(frame.payload);
}

NativeIpc::IpcApprovalSubmission approvalSubmission(StubMemService &service,
                                                    uint64_t sessionId,
                                                    uint64_t requestId = 100) {
  NativeIpc::IpcApprovalSubmission value;
  value.sessionId = sessionId;
  value.requestId = requestId;
  value.clientName = "AMem.RuntimeTests";
  value.clientVersion = "1.0";
  value.method = "memory_write";
  value.expected = service.captureContext(true);
  value.deadline = std::chrono::steady_clock::now() + 5s;
  return value;
}

void testObserveRequestPrivilegeBoundaryAndSnapshot() {
  StubMemService service;
  NativeIpc::NativeAgentRuntime runtime(service, uniquePipeName(L"observe"), {},
                                        fastSessionConfig());
  startRuntime(runtime);

  RuntimeClient client(runtime.snapshot().server.pipeName);
  const json ack = client.hello("AMem.SnapshotClient",
                                json::array({"Observe", "TargetMutation"}));
  expect(ack["granted_capabilities"] == json::array({"Observe"}) &&
             ack["denied_capabilities"] == json::array({"TargetMutation"}),
         "runtime must remain Observe-only");
  expect(waitUntil([&] {
           const auto snapshot = runtime.snapshot();
           return snapshot.phase == NativeIpc::RuntimePhase::Serving &&
                  snapshot.activeClientName == "AMem.SnapshotClient";
         }),
         "runtime should expose bounded active client diagnostics");

  client.request(1, "status");
  const json status =
      responseJson(client.read(), IpcProtocol::MessageType::Response, 1);
  expect(status["ok"] == true && service.statusCalls() == 1,
         "Observe request should execute through MemService");

  constexpr const char *secret = "TOP_SECRET_REQUEST_BYTES";
  client.request(2, "memory_write", {{"address", "0x1000"}, {"data", secret}});
  const json denied =
      responseJson(client.read(), IpcProtocol::MessageType::Error, 2);
  expect(denied["code"] == "capability_denied",
         "privileged request should be rejected before dispatch");

  client.close();
  expect(waitUntil([&] { return runtime.snapshot().completedSessions == 1; }),
         "closed client session should complete");
  const auto snapshot = runtime.snapshot();
  expect(snapshot.establishedSessions == 1 && snapshot.completedSessions == 1 &&
             snapshot.lastHandshakeStatus ==
                 NativeIpc::HandshakeStatus::Established &&
             snapshot.lastSessionStatus ==
                 NativeIpc::RequestSessionStatus::Closed &&
             snapshot.lastRequestFrames == 2 &&
             snapshot.lastDispatchedRequests == 1 &&
             snapshot.lastResponsesSent == 2,
         "runtime snapshot should retain only bounded session counters");
  expect(snapshot.activeClientName.empty() &&
             snapshot.activeClientVersion.empty() &&
             snapshot.grantedCapabilities.empty() &&
             snapshot.lastError.find(L"TOP_SECRET") == std::wstring::npos,
         "completed snapshot must clear identity and exclude request data");
  runtime.stop();
}

void testSequentialClientSessions() {
  StubMemService service;
  NativeIpc::NativeAgentRuntime runtime(service, uniquePipeName(L"sequential"),
                                        {}, fastSessionConfig());
  startRuntime(runtime);

  for (uint64_t index = 1; index <= 2; ++index) {
    RuntimeClient client(runtime.snapshot().server.pipeName);
    client.hello();
    client.request(index, "status");
    expect(responseJson(client.read(), IpcProtocol::MessageType::Response,
                        index)["ok"] == true,
           "sequential Observe request should succeed");
    client.close();
    expect(waitUntil(
               [&] { return runtime.snapshot().completedSessions == index; }),
           "sequential session should finish before rollover");
  }

  const auto snapshot = runtime.snapshot();
  expect(snapshot.server.acceptedConnections == 2 &&
             snapshot.establishedSessions == 2 &&
             snapshot.completedSessions == 2 && service.statusCalls() == 2,
         "runtime should count sequential sessions consistently");
  runtime.stop();
}

void testRejectedHandshakeDiagnostics() {
  StubMemService service;
  NativeIpc::HandshakeConfig handshakeConfig;
  handshakeConfig.rejectionDrainTimeout = 100ms;
  NativeIpc::NativeAgentRuntime runtime(service, uniquePipeName(L"reject"),
                                        handshakeConfig, fastSessionConfig());
  startRuntime(runtime);

  RuntimeClient client(runtime.snapshot().server.pipeName);
  client.send(
      IpcProtocol::MessageType::Hello, 0,
      json({{"requested_capabilities", json::array({"Observe"})}}).dump(),
      NativeIpc::kMaxHandshakePayloadBytes);
  const json error =
      responseJson(client.read(NativeIpc::kMaxHandshakePayloadBytes),
                   IpcProtocol::MessageType::Error, 0);
  expect(error["code"] == "invalid_hello",
         "malformed Hello should be rejected structurally");
  client.close();
  expect(waitUntil([&] { return runtime.snapshot().completedSessions == 1; }),
         "rejected handshake should complete its client handler");
  const auto snapshot = runtime.snapshot();
  expect(snapshot.establishedSessions == 0 &&
             snapshot.lastHandshakeStatus ==
                 NativeIpc::HandshakeStatus::Rejected &&
             !snapshot.lastSessionStatus.has_value(),
         "runtime should retain rejected handshake status only");
  runtime.stop();
}

void testTargetInvalidationClosesSession() {
  StubMemService service;
  NativeIpc::NativeAgentRuntime runtime(service, uniquePipeName(L"invalidate"),
                                        {}, fastSessionConfig());
  startRuntime(runtime);

  RuntimeClient client(runtime.snapshot().server.pipeName);
  client.hello();
  client.request(1, "status");
  expect(responseJson(client.read(), IpcProtocol::MessageType::Response,
                      1)["ok"] == true,
         "baseline request should establish dispatcher state");
  service.invalidateTarget();
  const json invalidated =
      responseJson(client.read(), IpcProtocol::MessageType::Error, 0);
  expect(invalidated["code"] == "target_changed",
         "target revision change should invalidate the session");
  expect(waitUntil([&] { return runtime.snapshot().completedSessions == 1; }),
         "invalidated session should close and join");
  expect(runtime.snapshot().lastSessionStatus ==
             NativeIpc::RequestSessionStatus::Invalidated,
         "runtime should retain invalidation status");
  runtime.stop();
}

void testStopDuringHandshakeJoins() {
  StubMemService service;
  NativeIpc::NativeAgentRuntime runtime(
      service, uniquePipeName(L"stop-handshake"), {}, fastSessionConfig());
  startRuntime(runtime);
  RuntimeClient client(runtime.snapshot().server.pipeName);
  expect(waitUntil([&] {
           return runtime.snapshot().phase ==
                  NativeIpc::RuntimePhase::Handshaking;
         }),
         "connected silent client should enter handshaking");

  runtime.stop();
  const auto snapshot = runtime.snapshot();
  expect(snapshot.server.state == NativeIpc::ServerState::Stopped &&
             snapshot.phase == NativeIpc::RuntimePhase::Idle &&
             snapshot.completedSessions == 1 &&
             snapshot.lastHandshakeStatus ==
                 NativeIpc::HandshakeStatus::Cancelled,
         "Stop should cancel handshake and join the server thread");
}

void testStopDuringActiveRequestJoinsWorker() {
  StubMemService service;
  service.setBlockStatus(true);
  NativeIpc::NativeAgentRuntime runtime(
      service, uniquePipeName(L"stop-request"), {}, fastSessionConfig());
  startRuntime(runtime);
  RuntimeClient client(runtime.snapshot().server.pipeName);
  client.hello();
  client.request(1, "status");
  expect(waitUntil([&] { return service.statusStarted(); }),
         "blocking MemService request should start");

  runtime.stop();
  const auto snapshot = runtime.snapshot();
  expect(service.cancellationObserved() &&
             snapshot.server.state == NativeIpc::ServerState::Stopped &&
             snapshot.completedSessions == 1 &&
             snapshot.lastSessionStatus ==
                 NativeIpc::RequestSessionStatus::Cancelled &&
             snapshot.lastCancellationsObserved == 1,
         "Stop should cancel active dispatch and join its worker");
}

void testRuntimeRestartResetsInstanceDiagnostics() {
  StubMemService service;
  NativeIpc::NativeAgentRuntime runtime(service, uniquePipeName(L"restart"), {},
                                        fastSessionConfig());
  startRuntime(runtime);
  uint64_t firstSessionId = 0;
  {
    RuntimeClient client(runtime.snapshot().server.pipeName);
    client.hello();
    expect(waitUntil([&] {
             firstSessionId = runtime.snapshot().activeSessionId;
             return firstSessionId != 0;
           }),
           "first runtime epoch should allocate a session id");
    client.close();
    expect(waitUntil([&] { return runtime.snapshot().completedSessions == 1; }),
           "first runtime instance should complete a session");
  }
  runtime.stop();

  startRuntime(runtime);
  const auto restarted = runtime.snapshot();
  expect(restarted.server.acceptedConnections == 0 &&
             restarted.establishedSessions == 0 &&
             restarted.completedSessions == 0 &&
             !restarted.lastHandshakeStatus.has_value() &&
             !restarted.lastSessionStatus.has_value() &&
             restarted.activeSessionId == 0 && restarted.lastSessionId == 0,
         "restart should begin a fresh diagnostics epoch");
  {
    RuntimeClient client(runtime.snapshot().server.pipeName);
    client.hello();
    expect(waitUntil([&] {
             return runtime.snapshot().activeSessionId > firstSessionId;
           }),
           "server session ids must not be reused across restart");
    client.close();
    expect(waitUntil([&] { return runtime.snapshot().completedSessions == 1; }),
           "restarted session should complete");
  }
  runtime.stop();
  runtime.stop();
}

void testSessionCloseCancelsBoundApproval() {
  StubMemService service;
  NativeIpc::IpcApprovalBroker broker;
  NativeIpc::NativeAgentRuntime runtime(service,
                                        uniquePipeName(L"approval-close"), {},
                                        fastSessionConfig(), &broker);
  startRuntime(runtime);
  RuntimeClient client(runtime.snapshot().server.pipeName);
  client.hello();
  expect(waitUntil([&] { return runtime.snapshot().activeSessionId != 0; }),
         "established client should receive a server session id");
  const uint64_t sessionId = runtime.snapshot().activeSessionId;
  const auto submitted = broker.submit(approvalSubmission(service, sessionId));
  expect(submitted.ok, "test approval should bind active runtime session");

  client.close();
  expect(waitUntil([&] { return runtime.snapshot().completedSessions == 1; }),
         "closed approval session should finish");
  const auto records = broker.snapshot();
  const auto snapshot = runtime.snapshot();
  expect(records.size() == 1 &&
             records.front().state == NativeIpc::IpcApprovalState::Cancelled &&
             snapshot.activeSessionId == 0 &&
             snapshot.lastSessionId == sessionId,
         "session close must cancel approval and retain bounded session id");
  runtime.stop();
}

void testSessionInvalidationCancelsBoundApproval() {
  StubMemService service;
  NativeIpc::IpcApprovalBroker broker;
  NativeIpc::NativeAgentRuntime runtime(service,
                                        uniquePipeName(L"approval-invalidate"),
                                        {}, fastSessionConfig(), &broker);
  startRuntime(runtime);
  RuntimeClient client(runtime.snapshot().server.pipeName);
  client.hello();
  client.request(1, "status");
  expect(responseJson(client.read(), IpcProtocol::MessageType::Response,
                      1)["ok"] == true,
         "approval invalidation baseline request should succeed");
  const uint64_t sessionId = runtime.snapshot().activeSessionId;
  expect(sessionId != 0 &&
             broker.submit(approvalSubmission(service, sessionId)).ok,
         "pending approval should bind established session");

  service.invalidateTarget();
  expect(responseJson(client.read(), IpcProtocol::MessageType::Error,
                      0)["code"] == "target_changed",
         "target change should close request session");
  expect(waitUntil([&] { return runtime.snapshot().completedSessions == 1; }),
         "invalidated approval session should finish");
  expect(broker.snapshot().front().state ==
             NativeIpc::IpcApprovalState::Cancelled,
         "request-session invalidation must cancel its broker approvals");
  runtime.stop();
}

void testPrivilegedRequestSubmitsAndRemainsNonExecutable() {
  StubMemService service;
  NativeIpc::IpcApprovalBroker broker;
  NativeIpc::NativeAgentRuntime runtime(
      service, uniquePipeName(L"approval-submit"), {}, fastSessionConfig(),
      &broker);
  startRuntime(runtime);
  RuntimeClient client(runtime.snapshot().server.pipeName);
  const json ack = client.hello(
      "AMem.ApprovalClient",
      json::array({"Observe", "TargetMutation"}));
  expect(ack["granted_capabilities"] == json::array({"Observe"}) &&
             ack["denied_capabilities"] ==
                 json::array({"TargetMutation"}),
         "submission path must not widen Hello grants");

  client.request(80, "memory_write",
                 {{"address", "0x1000"}, {"data", "SECRET_BYTES"}});
  expect(waitUntil([&] {
           const auto records = broker.snapshot();
           return records.size() == 1 &&
                  records.front().state ==
                      NativeIpc::IpcApprovalState::Pending;
         }),
         "privileged request should submit while reader remains active");
  const auto pending = broker.snapshot().front();
  expect(pending.sessionId == runtime.snapshot().activeSessionId &&
             pending.requestId == 80 &&
             pending.clientName == "AMem.ApprovalClient" &&
             service.writeCalls() == 0,
         "approval record must bind server session without executing params");
  client.send(IpcProtocol::MessageType::Cancel, 80, "{}");
  const json cancelled =
      responseJson(client.read(IpcProtocol::kMaxFramePayloadBytes,
                               "cancelled approval response"),
                   IpcProtocol::MessageType::Response, 80);
  expect(cancelled["error"]["code"] == "cancelled" &&
             cancelled["completion"] == "cancelled_before_start" &&
             broker.find(pending.approvalId)->state ==
                 NativeIpc::IpcApprovalState::Cancelled &&
             service.writeCalls() == 0,
         "client Cancel must revoke pending approval without a device call");

  client.request(81, "memory_write",
                 {{"address", "0x1000"}, {"data", "OTHER_SECRET"}});
  expect(waitUntil([&] { return broker.snapshot().size() == 2; }),
         "session should accept another request after cancellation response");
  const auto approved = broker.snapshot().back();
  expect(broker
             .decide(approved.approvalId,
                     NativeIpc::IpcApprovalDecision::Approve,
                     service.captureContext(true))
             .ok,
         "GUI-equivalent decision should approve matching context");
  const json disabled =
      responseJson(client.read(IpcProtocol::kMaxFramePayloadBytes,
                               "approved-disabled response"),
                   IpcProtocol::MessageType::Response, 81);
  expect(disabled["error"]["code"] == "approval_execution_disabled" &&
             disabled["completion"] == "rejected_before_start" &&
             broker.find(approved.approvalId)->state ==
                 NativeIpc::IpcApprovalState::Cancelled &&
             service.writeCalls() == 0,
         "approved request must remain non-executable in submission slice");

  client.request(82, "status");
  try {
    expect(responseJson(client.read(IpcProtocol::kMaxFramePayloadBytes,
                                    "post-approval Observe response"),
                        IpcProtocol::MessageType::Response, 82)["ok"] == true,
           "Observe request should remain usable after approval decisions");
  } catch (const std::exception &error) {
    const auto failed = runtime.snapshot();
    throw std::runtime_error(
        std::string(error.what()) + ", runtime_phase=" +
        std::to_string(static_cast<int>(failed.phase)) +
        ", completed_sessions=" +
        std::to_string(failed.completedSessions) + ", last_session_status=" +
        (failed.lastSessionStatus.has_value()
             ? std::to_string(static_cast<int>(*failed.lastSessionStatus))
             : std::string("none")) +
        ", last_error=" +
        std::string(failed.lastError.begin(), failed.lastError.end()));
  }
  client.close();
  expect(waitUntil([&] { return runtime.snapshot().completedSessions == 1; }),
         "approval submission session should close cleanly");
  runtime.stop();
}

} // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests = {
      {"Observe request, privilege boundary, and snapshot",
       &testObserveRequestPrivilegeBoundaryAndSnapshot},
      {"sequential client sessions", &testSequentialClientSessions},
      {"rejected handshake diagnostics", &testRejectedHandshakeDiagnostics},
      {"target invalidation closes session",
       &testTargetInvalidationClosesSession},
      {"Stop during handshake joins", &testStopDuringHandshakeJoins},
      {"Stop during active request joins worker",
       &testStopDuringActiveRequestJoinsWorker},
      {"runtime restart resets diagnostics",
       &testRuntimeRestartResetsInstanceDiagnostics},
      {"session close cancels bound approval",
       &testSessionCloseCancelsBoundApproval},
      {"session invalidation cancels bound approval",
       &testSessionInvalidationCancelsBoundApproval},
      {"privileged request submits without execution",
       &testPrivilegedRequestSubmitsAndRemainsNonExecutable},
  };

  int failures = 0;
  for (const auto &test : tests) {
    try {
      test.second();
      std::cout << "[PASS] " << test.first << '\n';
    } catch (const std::exception &error) {
      ++failures;
      std::cerr << "[FAIL] " << test.first << ": " << error.what() << '\n';
    }
  }
  if (failures != 0) {
    std::cerr << failures << " test group(s) failed\n";
    return 1;
  }
  std::cout << tests.size() << " test groups passed\n";
  return 0;
}
