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
// retain this object for as long as the server is enabled.
class NativeAgentRuntime final {
public:
  NativeAgentRuntime(Mem::IMemService &service,
                     std::wstring pipeName = kDefaultPipeName,
                     HandshakeConfig handshakeConfig = {},
                     RequestSessionConfig requestConfig = {});
  ~NativeAgentRuntime();

  NativeAgentRuntime(const NativeAgentRuntime &) = delete;
  NativeAgentRuntime &operator=(const NativeAgentRuntime &) = delete;

  bool start(std::wstring &error);
  void stop();
  NativeAgentRuntimeSnapshot snapshot() const;

private:
  void handleClient(HANDLE pipe, HANDLE stopEvent);
  void recordHandshake(const HandshakeResult &result);
  void recordSession(const RequestSessionResult &result);
  void recordHandlerFailure(const std::wstring &error);
  void finishClient();
  void resetForStart();

  Mem::IMemService &service_;
  const HandshakeConfig handshakeConfig_;
  const RequestSessionConfig requestConfig_;
  NamedPipeServer server_;

  mutable std::mutex lifecycleMutex_;
  mutable std::mutex stateMutex_;
  RuntimePhase phase_ = RuntimePhase::Idle;
  bool stopping_ = false;
  uint64_t establishedSessions_ = 0;
  uint64_t completedSessions_ = 0;
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
