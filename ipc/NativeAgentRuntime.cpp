#include "NativeAgentRuntime.h"

#include "IpcApprovalBroker.h"
#include "IpcFramedConnection.h"
#include "IpcMemServiceDispatcher.h"

#include <exception>
#include <utility>

namespace NativeIpc {

NativeAgentRuntime::NativeAgentRuntime(Mem::IMemService &service,
                                       std::wstring pipeName,
                                       HandshakeConfig handshakeConfig,
                                       RequestSessionConfig requestConfig,
                                       IpcApprovalBroker *approvalBroker)
    : service_(service), approvalBroker_(approvalBroker),
      handshakeConfig_(handshakeConfig), requestConfig_(requestConfig),
      server_(std::move(pipeName), [this](HANDLE pipe, HANDLE stopEvent) {
        handleClient(pipe, stopEvent);
      }) {}

NativeAgentRuntime::~NativeAgentRuntime() { stop(); }

bool NativeAgentRuntime::start(std::wstring &error) {
  std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);
  const ServerState current = server_.snapshot().state;
  if (current == ServerState::Listening || current == ServerState::Connected) {
    error.clear();
    return true;
  }

  resetForStart();
  if (server_.start(error)) {
    return true;
  }

  std::lock_guard<std::mutex> stateLock(stateMutex_);
  phase_ = RuntimePhase::Failed;
  lastError_ = error;
  return false;
}

void NativeAgentRuntime::stop() {
  std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);
  {
    std::lock_guard<std::mutex> stateLock(stateMutex_);
    stopping_ = true;
    phase_ = RuntimePhase::Stopping;
  }

  server_.stop();

  std::lock_guard<std::mutex> stateLock(stateMutex_);
  stopping_ = false;
  phase_ = RuntimePhase::Idle;
  activeClientName_.clear();
  activeClientVersion_.clear();
  grantedCapabilities_.clear();
  activeSessionId_ = 0;
}

NativeAgentRuntimeSnapshot NativeAgentRuntime::snapshot() const {
  // NamedPipeServer invokes handlers without its state lock. Taking the
  // server snapshot first keeps the lock order one-way.
  ServerSnapshot serverSnapshot = server_.snapshot();
  std::lock_guard<std::mutex> stateLock(stateMutex_);

  NativeAgentRuntimeSnapshot result;
  result.server = std::move(serverSnapshot);
  result.phase = phase_;
  result.establishedSessions = establishedSessions_;
  result.completedSessions = completedSessions_;
  result.activeSessionId = activeSessionId_;
  result.lastSessionId = lastSessionId_;
  result.activeClientName = activeClientName_;
  result.activeClientVersion = activeClientVersion_;
  result.grantedCapabilities = grantedCapabilities_;
  result.lastHandshakeStatus = lastHandshakeStatus_;
  result.lastSessionStatus = lastSessionStatus_;
  result.lastRequestFrames = lastRequestFrames_;
  result.lastDispatchedRequests = lastDispatchedRequests_;
  result.lastResponsesSent = lastResponsesSent_;
  result.lastCancellationsObserved = lastCancellationsObserved_;
  result.lastError = lastError_;
  return result;
}

void NativeAgentRuntime::handleClient(HANDLE pipe, HANDLE stopEvent) {
  uint64_t sessionId = 0;
  {
    std::lock_guard<std::mutex> stateLock(stateMutex_);
    phase_ = stopping_ ? RuntimePhase::Stopping : RuntimePhase::Handshaking;
    activeClientName_.clear();
    activeClientVersion_.clear();
    grantedCapabilities_.clear();
  }

  try {
    IpcFramedConnection connection(pipe, stopEvent);
    IpcHandshakeSession handshake(connection, handshakeConfig_);
    HandshakeResult handshakeResult = handshake.perform();
    sessionId = recordHandshake(handshakeResult);
    if (handshakeResult.status != HandshakeStatus::Established) {
      finishClient(0);
      return;
    }
    if (sessionId == 0) {
      recordHandlerFailure(L"native Agent session id space exhausted");
      finishClient(0);
      return;
    }

    IpcMemServiceDispatcher dispatcher(
        service_, approvalBroker_,
        {sessionId, handshakeResult.clientName,
         handshakeResult.clientVersion});
    IpcRequestSession session(connection, dispatcher,
                              handshakeResult.grantedCapabilities,
                              requestConfig_);
    recordSession(sessionId, session.run());
    finishClient(sessionId);
  } catch (const std::exception &) {
    recordHandlerFailure(L"native Agent client handler threw an exception");
    finishClient(sessionId);
  } catch (...) {
    recordHandlerFailure(
        L"native Agent client handler failed with an unknown error");
    finishClient(sessionId);
  }
}

uint64_t NativeAgentRuntime::recordHandshake(const HandshakeResult &result) {
  std::lock_guard<std::mutex> stateLock(stateMutex_);
  lastHandshakeStatus_ = result.status;
  lastError_ = result.error;
  if (result.status != HandshakeStatus::Established) {
    return 0;
  }
  if (nextSessionId_ == 0) {
    return 0;
  }

  const uint64_t sessionId = nextSessionId_++;
  ++establishedSessions_;
  activeSessionId_ = sessionId;
  phase_ = stopping_ ? RuntimePhase::Stopping : RuntimePhase::Serving;
  activeClientName_ = result.clientName;
  activeClientVersion_ = result.clientVersion;
  grantedCapabilities_ = result.grantedCapabilities;
  return sessionId;
}

void NativeAgentRuntime::recordSession(uint64_t sessionId,
                                       const RequestSessionResult &result) {
  std::lock_guard<std::mutex> stateLock(stateMutex_);
  lastSessionId_ = sessionId;
  lastSessionStatus_ = result.status;
  lastRequestFrames_ = result.requestFrames;
  lastDispatchedRequests_ = result.dispatchedRequests;
  lastResponsesSent_ = result.responsesSent;
  lastCancellationsObserved_ = result.cancellationsObserved;
  lastError_ = result.error;
}

void NativeAgentRuntime::recordHandlerFailure(const std::wstring &error) {
  std::lock_guard<std::mutex> stateLock(stateMutex_);
  phase_ = RuntimePhase::Failed;
  lastError_ = error;
}

void NativeAgentRuntime::finishClient(uint64_t sessionId) {
  if (approvalBroker_ != nullptr && sessionId != 0) {
    approvalBroker_->cancelSession(sessionId);
  }
  std::lock_guard<std::mutex> stateLock(stateMutex_);
  ++completedSessions_;
  if (sessionId != 0) {
    lastSessionId_ = sessionId;
  }
  activeClientName_.clear();
  activeClientVersion_.clear();
  grantedCapabilities_.clear();
  activeSessionId_ = 0;
  if (!stopping_ && phase_ != RuntimePhase::Failed) {
    phase_ = RuntimePhase::Idle;
  }
}

void NativeAgentRuntime::resetForStart() {
  std::lock_guard<std::mutex> stateLock(stateMutex_);
  phase_ = RuntimePhase::Idle;
  stopping_ = false;
  establishedSessions_ = 0;
  completedSessions_ = 0;
  activeSessionId_ = 0;
  lastSessionId_ = 0;
  activeClientName_.clear();
  activeClientVersion_.clear();
  grantedCapabilities_.clear();
  lastHandshakeStatus_.reset();
  lastSessionStatus_.reset();
  lastRequestFrames_ = 0;
  lastDispatchedRequests_ = 0;
  lastResponsesSent_ = 0;
  lastCancellationsObserved_ = 0;
  lastError_.clear();
}

} // namespace NativeIpc
