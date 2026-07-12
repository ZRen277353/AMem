#pragma once

#include "IpcMethodCatalog.h"
#include "IpcRequestSession.h"

#include "mem/IMemService.h"
#include "mem/MemJsonTools.h"

#include <string>

namespace NativeIpc {

class IpcApprovalBroker;

struct IpcExternalSession {
    uint64_t sessionId = 0;
    std::string clientName;
    std::string clientVersion;
};

class IpcMemServiceDispatcher final : public IIpcRequestDispatcher {
public:
    explicit IpcMemServiceDispatcher(
        Mem::IMemService& service,
        IpcApprovalBroker* approvalBroker = nullptr,
        IpcExternalSession session = {});

    bool resolveCapability(const std::string& method,
                           IpcCapability& capability) const override;
    bool canSubmitForApproval(const std::string& method) const override;
    SessionValidation validateSession() const override;
    IpcDispatchResult execute(const IpcRequestDto& request,
                              const IpcRequestContext& context) override;

    const Mem::OperationContext& baselineContext() const {
        return baseline_;
    }

private:
    IpcDispatchResult submitForApproval(
        const IpcRequestDto& request,
        const IpcRequestContext& context);
    IpcDispatchResult invokeObserve(
        const IpcRequestDto& request,
        const Mem::OperationContext& context);
    static IpcDispatchResult fromToolJson(const std::string& payload);
    static IpcDispatchResult validationFailure(
        const SessionValidation& validation,
        RequestCompletion completion);

    Mem::IMemService& service_;
    Mem::MemJsonTools tools_;
    Mem::OperationContext baseline_;
    IpcApprovalBroker* const approvalBroker_;
    const IpcExternalSession session_;
};

} // namespace NativeIpc
