#pragma once

#include "IpcMethodCatalog.h"
#include "mem/MemTypes.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace NativeIpc {

enum class IpcApprovalState {
  Pending,
  Approved,
  Denied,
  Invalidated,
  Expired,
  Cancelled,
  Consumed,
};

enum class IpcApprovalDecision {
  Approve,
  Deny,
};

struct IpcApprovalSubmission {
  uint64_t sessionId = 0;
  uint64_t requestId = 0;
  std::string clientName;
  std::string clientVersion;
  std::string method;
  Mem::OperationContext expected;
  std::chrono::steady_clock::time_point deadline =
      (std::chrono::steady_clock::time_point::max)();
};

// Deliberately excludes request params and results. UI and audit consumers get
// method metadata plus the target binding, never raw memory bytes or Lua/code.
struct IpcApprovalRecord {
  uint64_t approvalId = 0;
  uint64_t sessionId = 0;
  uint64_t requestId = 0;
  std::string clientName;
  std::string clientVersion;
  std::string method;
  IpcCapability capability = IpcCapability::Observe;
  IpcMethodTargetPolicy targetPolicy = IpcMethodTargetPolicy::None;
  uint64_t connectionGeneration = 0;
  std::optional<Mem::TargetSnapshot> target;
  IpcApprovalState state = IpcApprovalState::Pending;
  std::chrono::steady_clock::time_point createdAt{};
  std::chrono::steady_clock::time_point deadline =
      (std::chrono::steady_clock::time_point::max)();
};

struct IpcApprovalGrant {
  uint64_t approvalId = 0;
  uint64_t sessionId = 0;
  uint64_t requestId = 0;
  std::string method;
  IpcCapability capability = IpcCapability::Observe;
  IpcMethodTargetPolicy targetPolicy = IpcMethodTargetPolicy::None;
  uint64_t connectionGeneration = 0;
  std::optional<Mem::TargetSnapshot> target;
};

struct IpcApprovalResult {
  bool ok = false;
  std::string code;
  std::string message;
  std::optional<IpcApprovalRecord> record;
  std::optional<IpcApprovalGrant> grant;
};

class IIpcApprovalAuditSink {
public:
  virtual ~IIpcApprovalAuditSink() = default;
  // The sink must outlive the broker. Calls occur without the broker mutex.
  virtual void recordApproval(const IpcApprovalRecord &record) = 0;
};

struct IpcApprovalBrokerConfig {
  size_t maxPending = 16;
  size_t maxRecords = 256;
};

class IpcApprovalBroker final {
public:
  explicit IpcApprovalBroker(IpcApprovalBrokerConfig config = {},
                             IIpcApprovalAuditSink *auditSink = nullptr);

  IpcApprovalResult submit(const IpcApprovalSubmission &submission);
  IpcApprovalResult decide(uint64_t approvalId, IpcApprovalDecision decision,
                           const Mem::OperationContext &current);
  IpcApprovalResult consume(uint64_t approvalId,
                            const Mem::OperationContext &current);

  size_t invalidateStale(const Mem::OperationContext &current);
  size_t cancelSession(uint64_t sessionId);
  size_t cancelAll();
  size_t expire(std::chrono::steady_clock::time_point now =
                    std::chrono::steady_clock::now());

  std::vector<IpcApprovalRecord> snapshot() const;

private:
  static bool isLive(IpcApprovalState state);
  static bool isTerminal(IpcApprovalState state);
  static bool contextMatches(const IpcApprovalRecord &record,
                             const Mem::OperationContext &current);
  static IpcApprovalGrant makeGrant(const IpcApprovalRecord &record);

  IpcApprovalResult failure(const char *code, const char *message) const;
  void pruneTerminalLocked();
  void audit(const IpcApprovalRecord &record) const;
  void audit(const std::vector<IpcApprovalRecord> &records) const;

  const IpcApprovalBrokerConfig config_;
  IIpcApprovalAuditSink *const auditSink_;

  mutable std::mutex mutex_;
  std::deque<IpcApprovalRecord> records_;
  uint64_t nextApprovalId_ = 1;
};

} // namespace NativeIpc
