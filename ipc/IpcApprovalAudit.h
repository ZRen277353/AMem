#pragma once

#include "IpcApprovalBroker.h"
#include "IpcExecutionAudit.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace NativeIpc {

struct IpcApprovalAuditEntry {
  int64_t timestampMs = 0;
  std::string eventType;
  uint64_t approvalId = 0;
  uint64_t sessionId = 0;
  uint64_t requestId = 0;
  std::string clientName;
  std::string clientVersion;
  std::string method;
  std::string capability;
  std::string targetPolicy;
  std::string state;
  uint64_t connectionGeneration = 0;
  std::optional<Mem::TargetSnapshot> target;
  uint64_t observedConnectionGeneration = 0;
  std::optional<Mem::TargetSnapshot> observedTarget;
  int64_t deadlineRemainingMs = 0;
  std::optional<bool> success;
  std::string completion;
  std::string errorCode;
  bool autoApproved = false;
};

struct IpcApprovalAuditSnapshot {
  std::string filepath;
  uint64_t successfulWrites = 0;
  uint64_t failedWrites = 0;
  std::string lastError;
  std::vector<IpcApprovalAuditEntry> recent;
};

class IpcApprovalAuditLog final : public IIpcApprovalAuditSink,
                                  public IIpcExecutionAuditSink {
public:
  explicit IpcApprovalAuditLog(
      std::string filepath = "native_ipc_approval_audit.jsonl",
      size_t maxFileBytes = 4u * 1024u * 1024u,
      size_t maxRecentEntries = 100);

  bool recordApproval(const IpcApprovalRecord &record,
                      std::string *error = nullptr) override;
  bool recordExecution(const IpcExecutionAuditRecord &record,
                       std::string *error = nullptr) override;
  bool append(const IpcApprovalRecord &record, std::string *error = nullptr);
  IpcApprovalAuditSnapshot snapshot() const;

private:
  void loadRecent();
  void loadFileLocked(const std::string &filepath);
  void loadLineLocked(const std::string &line);
  void rememberLocked(IpcApprovalAuditEntry entry);
  bool appendJson(const std::string &line,
                  const IpcApprovalAuditEntry &entry,
                  std::string *error);
  bool failLocked(const std::string &message, std::string *error);

  const std::string filepath_;
  const size_t maxFileBytes_;
  const size_t maxRecentEntries_;

  mutable std::mutex mutex_;
  uint64_t successfulWrites_ = 0;
  uint64_t failedWrites_ = 0;
  std::string lastError_;
  std::vector<IpcApprovalAuditEntry> recent_;
};

} // namespace NativeIpc
