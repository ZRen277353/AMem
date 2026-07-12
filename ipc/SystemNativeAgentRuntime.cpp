#include "SystemNativeAgentRuntime.h"

#include "IpcMemServiceDispatcher.h"
#include "NativeAgentRuntime.h"
#include "mem/LuaJsonTool.h"
#include "mem/SystemMemService.h"

#include <memory>
#include <mutex>

namespace NativeIpc {
namespace {

class SystemHostMethodExecutor final : public IIpcHostMethodExecutor {
public:
  explicit SystemHostMethodExecutor(Mem::IMemService &service)
      : service_(service) {}

  bool supports(const std::string &method) const override {
#ifdef HAVE_LUAJIT
    return method == "lua_execute";
#else
    (void)method;
    return false;
#endif
  }

  std::string execute(const std::string &method, const std::string &paramsJson,
                      const Mem::OperationContext &context) override {
#ifdef HAVE_LUAJIT
    if (method == "lua_execute") {
      return Mem::executeLuaJson(service_, paramsJson, context);
    }
#else
    (void)method;
    (void)paramsJson;
    (void)context;
#endif
    return R"({"success":false,"error":{"code":"unsupported","message":"host method is unavailable","retryable":false},"completion":"rejected_before_start"})";
  }

private:
  Mem::IMemService &service_;
};

class SystemRuntimeOwner final {
public:
  NativeAgentRuntime &getRuntime() {
    std::lock_guard<std::mutex> lock(mutex_);
    ensureApprovalServicesLocked();
    if (!runtime_) {
      runtime_ = std::make_unique<NativeAgentRuntime>(
          Mem::getSystemMemService(), kDefaultPipeName, HandshakeConfig{},
          RequestSessionConfig{}, approvalBroker_.get(), hostExecutor_.get(),
          approvalAudit_.get());
    }
    return *runtime_;
  }

  IpcApprovalBroker &getApprovalBroker() {
    std::lock_guard<std::mutex> lock(mutex_);
    ensureApprovalServicesLocked();
    return *approvalBroker_;
  }

  IpcApprovalAuditSnapshot getApprovalAuditSnapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    ensureApprovalServicesLocked();
    return approvalAudit_->snapshot();
  }

  void stop() {
    IpcApprovalBroker *approvalBroker = nullptr;
    NativeAgentRuntime *runtime = nullptr;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      approvalBroker = approvalBroker_.get();
      runtime = runtime_.get();
    }
    if (approvalBroker != nullptr) {
      approvalBroker->cancelAll();
    }
    if (runtime != nullptr) {
      runtime->stop();
    }
  }

private:
  void ensureApprovalServicesLocked() {
    if (!approvalAudit_) {
      approvalAudit_ = std::make_unique<IpcApprovalAuditLog>();
    }
    if (!approvalBroker_) {
      approvalBroker_ =
          std::make_unique<IpcApprovalBroker>(IpcApprovalBrokerConfig{},
                                              approvalAudit_.get());
    }
    if (!hostExecutor_) {
      hostExecutor_ = std::make_unique<SystemHostMethodExecutor>(
          Mem::getSystemMemService());
    }
  }

  std::mutex mutex_;
  std::unique_ptr<IpcApprovalAuditLog> approvalAudit_;
  std::unique_ptr<IpcApprovalBroker> approvalBroker_;
  std::unique_ptr<SystemHostMethodExecutor> hostExecutor_;
  std::unique_ptr<NativeAgentRuntime> runtime_;
};

SystemRuntimeOwner &owner() {
  static SystemRuntimeOwner value;
  return value;
}

} // namespace

NativeAgentRuntime &GetSystemNativeAgentRuntime() {
  return owner().getRuntime();
}

IpcApprovalBroker &GetSystemIpcApprovalBroker() {
  return owner().getApprovalBroker();
}

IpcApprovalAuditSnapshot GetSystemIpcApprovalAuditSnapshot() {
  return owner().getApprovalAuditSnapshot();
}

std::vector<IpcApprovalRecord> RefreshSystemIpcApprovals() {
  IpcApprovalBroker &broker = GetSystemIpcApprovalBroker();
  broker.expire();
  broker.invalidateStale(Mem::getSystemMemService().captureContext(true));
  return broker.snapshot();
}

IpcApprovalResult DecideSystemIpcApproval(uint64_t approvalId,
                                          IpcApprovalDecision decision) {
  return GetSystemIpcApprovalBroker().decide(
      approvalId, decision, Mem::getSystemMemService().captureContext(true));
}

void StopSystemNativeAgentRuntime() { owner().stop(); }

void ShutdownSystemNativeAgentRuntime() { owner().stop(); }

} // namespace NativeIpc
