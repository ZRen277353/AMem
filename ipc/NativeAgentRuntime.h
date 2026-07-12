#pragma once

#include "IpcHandshakeSession.h"
#include "IpcRequestSession.h"
#include "NamedPipeServer.h"
#include "mem/IMemService.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace NativeIpc {

class IpcApprovalBroker;
class IIpcHostMethodExecutor;
class IIpcExecutionAuditSink;

enum class RuntimePhase {
  Idle,
  Handshaking,
  Serving,
  Stopping,
  Failed,
};

struct NativeAgentRuntimeSnapshot {
  ServerSnapshot server;
  RuntimePhase phase = RuntimePhase::Idle;
  uint64_t establishedSessions = 0;
  uint64_t completedSessions = 0;
  uint64_t activeSessionId = 0;
  uint64_t lastSessionId = 0;

  std::string activeClientName;
  std::string activeClientVersion;
  std::vector<IpcCapability> grantedCapabilities;

  std::optional<HandshakeStatus> lastHandshakeStatus;
  std::optional<RequestSessionStatus> lastSessionStatus;
  size_t lastRequestFrames = 0;
  size_t lastDispatchedRequests = 0;
  size_t lastResponsesSent = 0;
  size_t lastCancellationsObserved = 0;
  std::wstring lastError;
};

// Compile-only runtime composition. Product code must opt in explicitly and
// retain this object for as long as the server is enabled. An injected
// approval broker must outlive the runtime.
class NativeAgentRuntime final {
public:
  NativeAgentRuntime(Mem::IMemService &service,
                     std::wstring pipeName = kDefaultPipeName,
                     HandshakeConfig handshakeConfig = {},
                     RequestSessionConfig requestConfig = {},
                     IpcApprovalBroker *approvalBroker = nullptr,
                     IIpcHostMethodExecutor *hostExecutor = nullptr,
                     IIpcExecutionAuditSink *executionAuditSink = nullptr);
  ~NativeAgentRuntime();

  NativeAgentRuntime(const NativeAgentRuntime &) = delete;
  NativeAgentRuntime &operator=(const NativeAgentRuntime &) = delete;

  bool start(std::wstring &error);
  void stop();
  NativeAgentRuntimeSnapshot snapshot() const;

private:
  void handleClient(HANDLE pipe, HANDLE stopEvent);
  uint64_t recordHandshake(const HandshakeResult &result);
  void recordSession(uint64_t sessionId, const RequestSessionResult &result);
  void recordHandlerFailure(const std::wstring &error);
  void finishClient(uint64_t sessionId);
  void resetForStart();

  Mem::IMemService &service_;
  IpcApprovalBroker *const approvalBroker_;
  IIpcHostMethodExecutor *const hostExecutor_;
  IIpcExecutionAuditSink *const executionAuditSink_;
  const HandshakeConfig handshakeConfig_;
  const RequestSessionConfig requestConfig_;
  NamedPipeServer server_;

  mutable std::mutex lifecycleMutex_;
  mutable std::mutex stateMutex_;
  RuntimePhase phase_ = RuntimePhase::Idle;
  bool stopping_ = false;
  uint64_t establishedSessions_ = 0;
  uint64_t completedSessions_ = 0;
  uint64_t nextSessionId_ = 1;
  uint64_t activeSessionId_ = 0;
  uint64_t lastSessionId_ = 0;
  std::string activeClientName_;
  std::string activeClientVersion_;
  std::vector<IpcCapability> grantedCapabilities_;
  std::optional<HandshakeStatus> lastHandshakeStatus_;
  std::optional<RequestSessionStatus> lastSessionStatus_;
  size_t lastRequestFrames_ = 0;
  size_t lastDispatchedRequests_ = 0;
  size_t lastResponsesSent_ = 0;
  size_t lastCancellationsObserved_ = 0;
  std::wstring lastError_;
};

} // namespace NativeIpc
