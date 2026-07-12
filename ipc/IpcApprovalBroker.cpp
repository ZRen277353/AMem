#include "IpcApprovalBroker.h"

#include "IpcHandshakeSession.h"

#include <algorithm>

namespace NativeIpc {
namespace {

constexpr size_t kMaxApprovalClientNameBytes = kMaxClientNameBytes;
constexpr size_t kMaxApprovalClientVersionBytes = kMaxClientVersionBytes;

bool hasAsciiControl(const std::string &value) {
  return std::any_of(value.begin(), value.end(), [](unsigned char item) {
    return item < 0x20u || item == 0x7fu;
  });
}

bool validExpectedContext(const IpcMethodDescriptor &method,
                          const Mem::OperationContext &expected) {
  if (expected.connectionGeneration == 0) {
    return false;
  }
  if (method.targetPolicy == IpcMethodTargetPolicy::None) {
    return true;
  }
  if (!expected.target ||
      expected.target->connectionGeneration != expected.connectionGeneration) {
    return false;
  }
  return method.targetPolicy != IpcMethodTargetPolicy::Bound ||
         expected.target->isAttached();
}

} // namespace

IpcApprovalBroker::IpcApprovalBroker(IpcApprovalBrokerConfig config,
                                     IIpcApprovalAuditSink *auditSink)
    : config_{(std::max)(size_t{1}, config.maxPending),
              (std::max)((std::max)(size_t{1}, config.maxPending),
                         config.maxRecords)},
      auditSink_(auditSink) {}

IpcApprovalResult
IpcApprovalBroker::submit(const IpcApprovalSubmission &submission) {
  const auto now = std::chrono::steady_clock::now();
  if (submission.sessionId == 0 || submission.requestId == 0) {
    return failure("invalid_identity",
                   "session and request ids must be non-zero");
  }
  if (submission.clientName.empty() ||
      submission.clientName.size() > kMaxApprovalClientNameBytes ||
      submission.clientVersion.size() > kMaxApprovalClientVersionBytes ||
      hasAsciiControl(submission.clientName) ||
      hasAsciiControl(submission.clientVersion)) {
    return failure("invalid_client", "client identity is missing or too long");
  }
  if (submission.deadline == (std::chrono::steady_clock::time_point::max)() ||
      submission.deadline <= now) {
    return failure("invalid_deadline",
                   "approval requires a finite future deadline");
  }

  const IpcMethodDescriptor *method = FindIpcMethod(submission.method);
  if (method == nullptr) {
    return failure("method_not_found", "approval method is not registered");
  }
  if (method->executableWithoutApproval ||
      method->capability == IpcCapability::Observe) {
    return failure("approval_not_required",
                   "Observe methods cannot enter the approval broker");
  }
  if (!validExpectedContext(*method, submission.expected)) {
    return failure("invalid_context",
                   "approval target or connection context is invalid");
  }

  IpcApprovalRecord created;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto duplicate =
        std::find_if(records_.begin(), records_.end(), [&](const auto &record) {
          return record.sessionId == submission.sessionId &&
                 record.requestId == submission.requestId;
        });
    if (duplicate != records_.end()) {
      return failure("duplicate_approval",
                     "request already has an approval record");
    }
    pruneTerminalLocked();

    const size_t pending = static_cast<size_t>(
        std::count_if(records_.begin(), records_.end(),
                      [](const auto &record) { return isLive(record.state); }));
    if (pending >= config_.maxPending) {
      return failure("approval_queue_full", "approval queue is full");
    }
    if (records_.size() >= config_.maxRecords) {
      return failure("approval_history_full",
                     "approval history has no terminal record to prune");
    }
    if (nextApprovalId_ == 0) {
      return failure("approval_id_exhausted", "approval id space exhausted");
    }

    created.approvalId = nextApprovalId_++;
    created.sessionId = submission.sessionId;
    created.requestId = submission.requestId;
    created.clientName = submission.clientName;
    created.clientVersion = submission.clientVersion;
    created.method = method->name;
    created.capability = method->capability;
    created.targetPolicy = method->targetPolicy;
    created.connectionGeneration = submission.expected.connectionGeneration;
    created.target = submission.expected.target;
    created.createdAt = now;
    created.deadline = submission.deadline;
    records_.push_back(created);
  }

  audit(created);
  IpcApprovalResult result;
  result.ok = true;
  result.record = std::move(created);
  return result;
}

IpcApprovalResult
IpcApprovalBroker::decide(uint64_t approvalId, IpcApprovalDecision decision,
                          const Mem::OperationContext &current) {
  IpcApprovalRecord changed;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found =
        std::find_if(records_.begin(), records_.end(), [&](const auto &record) {
          return record.approvalId == approvalId;
        });
    if (found == records_.end()) {
      return failure("approval_not_found", "approval record was not found");
    }
    if (found->state != IpcApprovalState::Pending) {
      return failure("approval_not_pending", "approval is no longer pending");
    }
    if (std::chrono::steady_clock::now() >= found->deadline) {
      found->state = IpcApprovalState::Expired;
    } else if (decision == IpcApprovalDecision::Deny) {
      found->state = IpcApprovalState::Denied;
    } else if (!contextMatches(*found, current)) {
      found->state = IpcApprovalState::Invalidated;
    } else {
      found->state = IpcApprovalState::Approved;
    }
    changed = *found;
  }

  audit(changed);
  IpcApprovalResult result;
  result.ok = changed.state == IpcApprovalState::Approved ||
              changed.state == IpcApprovalState::Denied;
  if (!result.ok) {
    result.code = changed.state == IpcApprovalState::Expired
                      ? "approval_expired"
                      : "approval_invalidated";
    result.message = changed.state == IpcApprovalState::Expired
                         ? "approval deadline expired"
                         : "approval context changed";
  }
  result.record = std::move(changed);
  return result;
}

IpcApprovalResult
IpcApprovalBroker::consume(uint64_t approvalId,
                           const Mem::OperationContext &current) {
  IpcApprovalRecord changed;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found =
        std::find_if(records_.begin(), records_.end(), [&](const auto &record) {
          return record.approvalId == approvalId;
        });
    if (found == records_.end()) {
      return failure("approval_not_found", "approval record was not found");
    }
    if (found->state != IpcApprovalState::Approved) {
      return failure("approval_not_approved",
                     "approval cannot be consumed in its current state");
    }
    if (std::chrono::steady_clock::now() >= found->deadline) {
      found->state = IpcApprovalState::Expired;
    } else if (!contextMatches(*found, current)) {
      found->state = IpcApprovalState::Invalidated;
    } else {
      found->state = IpcApprovalState::Consumed;
    }
    changed = *found;
  }

  audit(changed);
  IpcApprovalResult result;
  result.ok = changed.state == IpcApprovalState::Consumed;
  if (result.ok) {
    result.grant = makeGrant(changed);
  } else {
    result.code = changed.state == IpcApprovalState::Expired
                      ? "approval_expired"
                      : "approval_invalidated";
    result.message = changed.state == IpcApprovalState::Expired
                         ? "approval deadline expired before execution"
                         : "approval context changed before execution";
  }
  result.record = std::move(changed);
  return result;
}

size_t
IpcApprovalBroker::invalidateStale(const Mem::OperationContext &current) {
  std::vector<IpcApprovalRecord> changed;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto &record : records_) {
      if (isLive(record.state) && !contextMatches(record, current)) {
        record.state = IpcApprovalState::Invalidated;
        changed.push_back(record);
      }
    }
  }
  audit(changed);
  return changed.size();
}

size_t IpcApprovalBroker::cancelSession(uint64_t sessionId) {
  std::vector<IpcApprovalRecord> changed;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto &record : records_) {
      if (record.sessionId == sessionId && isLive(record.state)) {
        record.state = IpcApprovalState::Cancelled;
        changed.push_back(record);
      }
    }
  }
  audit(changed);
  return changed.size();
}

size_t IpcApprovalBroker::cancelAll() {
  std::vector<IpcApprovalRecord> changed;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto &record : records_) {
      if (isLive(record.state)) {
        record.state = IpcApprovalState::Cancelled;
        changed.push_back(record);
      }
    }
  }
  audit(changed);
  return changed.size();
}

size_t IpcApprovalBroker::expire(std::chrono::steady_clock::time_point now) {
  std::vector<IpcApprovalRecord> changed;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto &record : records_) {
      if (isLive(record.state) && now >= record.deadline) {
        record.state = IpcApprovalState::Expired;
        changed.push_back(record);
      }
    }
  }
  audit(changed);
  return changed.size();
}

std::vector<IpcApprovalRecord> IpcApprovalBroker::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return {records_.begin(), records_.end()};
}

bool IpcApprovalBroker::isLive(IpcApprovalState state) {
  return state == IpcApprovalState::Pending ||
         state == IpcApprovalState::Approved;
}

bool IpcApprovalBroker::isTerminal(IpcApprovalState state) {
  return !isLive(state);
}

bool IpcApprovalBroker::contextMatches(const IpcApprovalRecord &record,
                                       const Mem::OperationContext &current) {
  if (current.connectionGeneration != record.connectionGeneration) {
    return false;
  }
  if (record.targetPolicy == IpcMethodTargetPolicy::None) {
    return true;
  }
  return record.target && current.target && *record.target == *current.target;
}

IpcApprovalGrant IpcApprovalBroker::makeGrant(const IpcApprovalRecord &record) {
  IpcApprovalGrant grant;
  grant.approvalId = record.approvalId;
  grant.sessionId = record.sessionId;
  grant.requestId = record.requestId;
  grant.method = record.method;
  grant.capability = record.capability;
  grant.targetPolicy = record.targetPolicy;
  grant.connectionGeneration = record.connectionGeneration;
  grant.target = record.target;
  return grant;
}

IpcApprovalResult IpcApprovalBroker::failure(const char *code,
                                             const char *message) const {
  IpcApprovalResult result;
  result.code = code;
  result.message = message;
  return result;
}

void IpcApprovalBroker::pruneTerminalLocked() {
  while (records_.size() >= config_.maxRecords) {
    const auto terminal =
        std::find_if(records_.begin(), records_.end(), [](const auto &record) {
          return isTerminal(record.state);
        });
    if (terminal == records_.end()) {
      return;
    }
    records_.erase(terminal);
  }
}

void IpcApprovalBroker::audit(const IpcApprovalRecord &record) const {
  if (auditSink_ == nullptr) {
    return;
  }
  try {
    auditSink_->recordApproval(record);
  } catch (...) {
    // Audit failures never turn a denied/invalidated request into approval.
  }
}

void IpcApprovalBroker::audit(
    const std::vector<IpcApprovalRecord> &records) const {
  for (const auto &record : records) {
    audit(record);
  }
}

} // namespace NativeIpc
