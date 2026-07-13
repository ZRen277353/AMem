#include "IpcApprovalAudit.h"
#include "../utils/BoundedJson.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <system_error>
#include <utility>

namespace NativeIpc {
namespace {

using json = nlohmann::json;

constexpr size_t kMaxRecordBytes = 16u * 1024u;
constexpr int kCurrentSchemaVersion = 2;
constexpr const char *kApprovalEvent = "approval_transition";
constexpr const char *kExecutionEvent = "execution_outcome";
constexpr utils::JsonComplexityLimits kAuditJsonLimits = {
    kMaxRecordBytes,
    12u,
    1024u,
    128u,
    kMaxRecordBytes,
    kMaxRecordBytes,
};

int64_t nowUnixMilliseconds() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch())
      .count();
}

const char *stateName(IpcApprovalState state) {
  switch (state) {
  case IpcApprovalState::Pending:
    return "pending";
  case IpcApprovalState::Approved:
    return "approved";
  case IpcApprovalState::Denied:
    return "denied";
  case IpcApprovalState::Invalidated:
    return "invalidated";
  case IpcApprovalState::Expired:
    return "expired";
  case IpcApprovalState::Cancelled:
    return "cancelled";
  case IpcApprovalState::Consumed:
    return "consumed";
  }
  return "unknown";
}

const char *targetPolicyName(IpcMethodTargetPolicy policy) {
  switch (policy) {
  case IpcMethodTargetPolicy::None:
    return "none";
  case IpcMethodTargetPolicy::Bound:
    return "bound";
  case IpcMethodTargetPolicy::Selection:
    return "selection";
  }
  return "none";
}

json targetJson(const Mem::TargetSnapshot &target) {
  return {{"pid", target.pid},
          {"handle", target.processHandle},
          {"process_revision", target.processRevision},
          {"connection_generation", target.connectionGeneration}};
}

std::optional<Mem::TargetSnapshot> targetFromJson(const json &record,
                                                   const char *field) {
  const auto target = record.find(field);
  if (target == record.end() || !target->is_object()) {
    return std::nullopt;
  }
  Mem::TargetSnapshot value;
  value.pid = target->value("pid", 0);
  value.processHandle = target->value("handle", uint64_t{0});
  value.processRevision = target->value("process_revision", uint64_t{0});
  value.connectionGeneration =
      target->value("connection_generation", uint64_t{0});
  return value;
}

int64_t deadlineRemainingMilliseconds(
    std::chrono::steady_clock::time_point deadline) {
  if (deadline == (std::chrono::steady_clock::time_point::max)()) {
    return (std::numeric_limits<int64_t>::max)();
  }
  const auto now = std::chrono::steady_clock::now();
  if (deadline <= now) {
    return 0;
  }
  return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
      .count();
}

IpcApprovalAuditEntry entryFromJson(const json &record) {
  IpcApprovalAuditEntry entry;
  entry.timestampMs = record.value("timestamp_ms", int64_t{0});
  entry.eventType = record.value("event_type", std::string{});
  if (entry.eventType.empty() && record.value("schema_version", 0) == 1) {
    entry.eventType = kApprovalEvent;
  }
  entry.approvalId = record.value("approval_id", uint64_t{0});
  entry.sessionId = record.value("session_id", uint64_t{0});
  entry.requestId = record.value("request_id", uint64_t{0});
  entry.clientName = record.value("client_name", std::string{});
  entry.clientVersion = record.value("client_version", std::string{});
  entry.method = record.value("method", std::string{});
  entry.capability = record.value("capability", std::string{});
  entry.targetPolicy = record.value("target_policy", std::string{});
  entry.state = record.value("state", std::string{});
  entry.connectionGeneration =
      record.value("connection_generation", uint64_t{0});
  entry.observedConnectionGeneration =
      record.value("observed_connection_generation", uint64_t{0});
  entry.deadlineRemainingMs =
      record.value("deadline_remaining_ms", int64_t{0});
  entry.target = targetFromJson(record, "target");
  entry.observedTarget = targetFromJson(record, "observed_target");
  const auto success = record.find("success");
  if (success != record.end() && success->is_boolean()) {
    entry.success = success->get<bool>();
  }
  entry.completion = record.value("completion", std::string{});
  entry.errorCode = record.value("error_code", std::string{});
  return entry;
}

json recordJson(const IpcApprovalRecord &record) {
  json value = {{"schema_version", kCurrentSchemaVersion},
                {"event_type", kApprovalEvent},
                {"timestamp_ms", nowUnixMilliseconds()},
                {"approval_id", record.approvalId},
                {"session_id", record.sessionId},
                {"request_id", record.requestId},
                {"client_name", record.clientName},
                {"client_version", record.clientVersion},
                {"method", record.method},
                {"capability", CapabilityName(record.capability)},
                {"target_policy", targetPolicyName(record.targetPolicy)},
                {"state", stateName(record.state)},
                {"connection_generation", record.connectionGeneration},
                {"deadline_remaining_ms",
                 deadlineRemainingMilliseconds(record.deadline)}};
  if (record.target) {
    value["target"] = targetJson(*record.target);
  }
  return value;
}

json recordJson(const IpcExecutionAuditRecord &record) {
  json value = {
      {"schema_version", kCurrentSchemaVersion},
      {"event_type", kExecutionEvent},
      {"timestamp_ms", nowUnixMilliseconds()},
      {"approval_id", record.approvalId},
      {"session_id", record.sessionId},
      {"request_id", record.requestId},
      {"client_name", record.clientName},
      {"client_version", record.clientVersion},
      {"method", record.method},
      {"capability", CapabilityName(record.capability)},
      {"target_policy", targetPolicyName(record.targetPolicy)},
      {"connection_generation", record.authorizedConnectionGeneration},
      {"observed_connection_generation",
       record.observedConnectionGeneration},
      {"success", record.success},
      {"completion", RequestCompletionName(record.completion)},
  };
  if (record.authorizedTarget) {
    value["target"] = targetJson(*record.authorizedTarget);
  }
  if (record.observedTarget) {
    value["observed_target"] = targetJson(*record.observedTarget);
  }
  if (!record.errorCode.empty()) {
    value["error_code"] = record.errorCode;
  }
  return value;
}

} // namespace

IpcApprovalAuditLog::IpcApprovalAuditLog(std::string filepath,
                                         size_t maxFileBytes,
                                         size_t maxRecentEntries)
    : filepath_(std::move(filepath)),
      maxFileBytes_((std::max)(maxFileBytes, kMaxRecordBytes)),
      maxRecentEntries_((std::max)(size_t{1}, maxRecentEntries)) {
  loadRecent();
}

bool IpcApprovalAuditLog::recordApproval(const IpcApprovalRecord &record,
                                         std::string *error) {
  return append(record, error);
}

bool IpcApprovalAuditLog::recordExecution(
    const IpcExecutionAuditRecord &record, std::string *error) {
  const json value = recordJson(record);
  return appendJson(value.dump(-1, ' ', false,
                               json::error_handler_t::replace),
                    entryFromJson(value), error);
}

bool IpcApprovalAuditLog::append(const IpcApprovalRecord &record,
                                 std::string *error) {
  const json value = recordJson(record);
  return appendJson(value.dump(-1, ' ', false,
                               json::error_handler_t::replace),
                    entryFromJson(value), error);
}

bool IpcApprovalAuditLog::appendJson(const std::string &serialized,
                                     const IpcApprovalAuditEntry &entry,
                                     std::string *error) {
  std::string line = serialized;
  if (line.size() > kMaxRecordBytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    return failLocked("native IPC audit record exceeds 16 KiB", error);
  }
  line.push_back('\n');

  std::lock_guard<std::mutex> lock(mutex_);
  if (filepath_.empty()) {
    return failLocked("native IPC audit filepath is empty", error);
  }

  const std::filesystem::path path(filepath_);
  std::error_code ec;
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
      return failLocked("cannot create native IPC audit directory: " +
                            ec.message(),
                        error);
    }
  }

  uintmax_t currentSize = 0;
  if (std::filesystem::exists(path, ec) && !ec) {
    currentSize = std::filesystem::file_size(path, ec);
    if (ec) {
      return failLocked("cannot inspect native IPC audit log: " + ec.message(),
                        error);
    }
  }
  if (currentSize > maxFileBytes_) {
    std::filesystem::remove(path, ec);
    if (ec) {
      return failLocked("cannot replace oversized native IPC audit log: " +
                            ec.message(),
                        error);
    }
    currentSize = 0;
  }
  if (currentSize > 0 &&
      line.size() > maxFileBytes_ - static_cast<size_t>(currentSize)) {
    const std::filesystem::path backup = path.string() + ".1";
    std::filesystem::remove(backup, ec);
    if (ec) {
      return failLocked("cannot remove native IPC audit backup: " +
                            ec.message(),
                        error);
    }
    std::filesystem::rename(path, backup, ec);
    if (ec) {
      return failLocked("cannot rotate native IPC audit log: " + ec.message(),
                        error);
    }
  }

  std::ofstream output(path, std::ios::binary | std::ios::app);
  if (!output) {
    return failLocked("cannot open native IPC audit log", error);
  }
  output.write(line.data(), static_cast<std::streamsize>(line.size()));
  output.flush();
  if (!output) {
    return failLocked("cannot write native IPC audit log", error);
  }

  ++successfulWrites_;
  lastError_.clear();
  rememberLocked(entry);
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

IpcApprovalAuditSnapshot IpcApprovalAuditLog::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  IpcApprovalAuditSnapshot result;
  result.filepath = filepath_;
  result.successfulWrites = successfulWrites_;
  result.failedWrites = failedWrites_;
  result.lastError = lastError_;
  result.recent = recent_;
  return result;
}

void IpcApprovalAuditLog::loadRecent() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (filepath_.empty()) {
    return;
  }
  loadFileLocked(filepath_ + ".1");
  loadFileLocked(filepath_);
}

void IpcApprovalAuditLog::loadFileLocked(const std::string &filepath) {
  const std::filesystem::path path(filepath);
  std::error_code ec;
  if (!std::filesystem::exists(path, ec) || ec) {
    return;
  }
  const uintmax_t size = std::filesystem::file_size(path, ec);
  if (ec) {
    return;
  }

  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return;
  }
  if (size > maxFileBytes_) {
    input.seekg(static_cast<std::streamoff>(size - maxFileBytes_));
    input.ignore((std::numeric_limits<std::streamsize>::max)(), '\n');
  }

  std::string line;
  line.reserve((std::min)(kMaxRecordBytes, size_t{1024}));
  bool oversized = false;
  char item = 0;
  while (input.get(item)) {
    if (item == '\n') {
      if (!oversized && !line.empty()) {
        loadLineLocked(line);
      }
      line.clear();
      oversized = false;
      continue;
    }
    if (!oversized) {
      if (line.size() < kMaxRecordBytes) {
        line.push_back(item);
      } else {
        line.clear();
        oversized = true;
      }
    }
  }
  if (!oversized && !line.empty()) {
    loadLineLocked(line);
  }
}

void IpcApprovalAuditLog::loadLineLocked(const std::string &line) {
  json value;
  std::string parseError;
  if (utils::parseBoundedJson(
          line, kAuditJsonLimits, value, parseError)) {
    const int schemaVersion = value.value("schema_version", 0);
    if (!value.is_object() || (schemaVersion != 1 && schemaVersion != 2)) {
      return;
    }
    IpcApprovalAuditEntry entry = entryFromJson(value);
    const bool commonValid =
        entry.approvalId != 0 && entry.sessionId != 0 &&
        entry.requestId != 0 && !entry.method.empty();
    const bool approvalValid =
        entry.eventType == kApprovalEvent && !entry.state.empty();
    const bool executionValid =
        entry.eventType == kExecutionEvent && entry.success.has_value() &&
        !entry.completion.empty() &&
        (*entry.success || !entry.errorCode.empty());
    if (!commonValid || (!approvalValid && !executionValid)) {
      return;
    }
    rememberLocked(std::move(entry));
  }
}

void IpcApprovalAuditLog::rememberLocked(IpcApprovalAuditEntry entry) {
  recent_.push_back(std::move(entry));
  if (recent_.size() > maxRecentEntries_) {
    recent_.erase(
        recent_.begin(),
        recent_.begin() + static_cast<std::ptrdiff_t>(recent_.size() -
                                                     maxRecentEntries_));
  }
}

bool IpcApprovalAuditLog::failLocked(const std::string &message,
                                     std::string *error) {
  ++failedWrites_;
  lastError_ = message;
  if (error != nullptr) {
    *error = message;
  }
  return false;
}

} // namespace NativeIpc
