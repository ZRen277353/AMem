#pragma once

#include "IpcMethodCatalog.h"
#include "IpcRequestSession.h"

#include "mem/IMemService.h"
#include "mem/MemJsonTools.h"

#include <mutex>
#include <string>

namespace NativeIpc {

class IpcApprovalBroker;
struct IpcApprovalGrant;

class IIpcHostMethodExecutor {
public:
  virtual ~IIpcHostMethodExecutor() = default;
  virtual bool supports(const std::string &method) const = 0;
  virtual std::string execute(const std::string &method,
                              const std::string &paramsJson,
                              const Mem::OperationContext &context) = 0;
};

struct IpcExternalSession {
    uint64_t sessionId = 0;
    std::string clientName;
    std::string clientVersion;
};

class IpcMemServiceDispatcher final : public IIpcRequestDispatcher {
public:
  explicit IpcMemServiceDispatcher(
      Mem::IMemService &service, IpcApprovalBroker *approvalBroker = nullptr,
      IpcExternalSession session = {},
      IIpcHostMethodExecutor *hostExecutor = nullptr);

  bool resolveCapability(const std::string &method,
                         IpcCapability &capability) const override;
  bool canSubmitForApproval(const std::string &method) const override;
  SessionValidation validateSession() const override;
  IpcDispatchResult execute(const IpcRequestDto &request,
                            const IpcRequestContext &context) override;

  Mem::OperationContext baselineContext() const;

private:
  IpcDispatchResult submitForApproval(const IpcRequestDto &request,
                                      const IpcRequestContext &context,
                                      const IpcMethodDescriptor &descriptor);
  IpcDispatchResult executeApproved(const IpcRequestDto &request,
                                    const IpcRequestContext &context,
                                    const IpcMethodDescriptor &descriptor,
                                    const IpcApprovalGrant &grant);
  IpcDispatchResult invokeMemService(const IpcRequestDto &request,
                                     const Mem::OperationContext &context);
  IpcDispatchResult invokeHost(const IpcRequestDto &request,
                               const Mem::OperationContext &context);
  bool supportsExecution(const IpcMethodDescriptor &descriptor) const;
  bool beginSelection(const Mem::OperationContext &expected,
                      SessionValidation &validation);
  IpcDispatchResult finishSelection(const IpcDispatchResult &result,
                                    const Mem::OperationContext &expected);
  SessionValidation
  validateCurrentLocked(const Mem::OperationContext &current) const;
  static IpcDispatchResult fromToolJson(const std::string &payload);
  static IpcDispatchResult
  validationFailure(const SessionValidation &validation,
                    RequestCompletion completion);

  Mem::IMemService &service_;
  Mem::MemJsonTools tools_;
  mutable std::mutex baselineMutex_;
  Mem::OperationContext baseline_;
  bool selectionInFlight_ = false;
  IpcApprovalBroker *const approvalBroker_;
  const IpcExternalSession session_;
  IIpcHostMethodExecutor *const hostExecutor_;
};

} // namespace NativeIpc
