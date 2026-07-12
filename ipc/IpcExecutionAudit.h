#pragma once

#include "IpcMethodCatalog.h"
#include "IpcRequestProtocol.h"
#include "mem/MemTypes.h"

#include <cstdint>
#include <optional>
#include <string>

namespace NativeIpc {

// Deliberately excludes request params, result JSON, and error messages.
// It records only the authorization binding and the final completion summary.
struct IpcExecutionAuditRecord {
  uint64_t approvalId = 0;
  uint64_t sessionId = 0;
  uint64_t requestId = 0;
  std::string clientName;
  std::string clientVersion;
  std::string method;
  IpcCapability capability = IpcCapability::Observe;
  IpcMethodTargetPolicy targetPolicy = IpcMethodTargetPolicy::None;
  uint64_t authorizedConnectionGeneration = 0;
  std::optional<Mem::TargetSnapshot> authorizedTarget;
  uint64_t observedConnectionGeneration = 0;
  std::optional<Mem::TargetSnapshot> observedTarget;
  bool success = false;
  RequestCompletion completion = RequestCompletion::CompletionUnknown;
  std::string errorCode;
};

class IIpcExecutionAuditSink {
public:
  virtual ~IIpcExecutionAuditSink() = default;
  // The sink must outlive the dispatcher. Calls are synchronous after grant
  // consumption and before the final response is returned.
  virtual bool recordExecution(const IpcExecutionAuditRecord &record,
                               std::string *error = nullptr) = 0;
};

} // namespace NativeIpc
