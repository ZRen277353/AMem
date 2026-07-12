#include "SystemNativeAgentRuntime.h"

#include "NativeAgentRuntime.h"
#include "mem/SystemMemService.h"

#include <memory>
#include <mutex>

namespace NativeIpc {
namespace {

class SystemRuntimeOwner final {
public:
  NativeAgentRuntime &getRuntime() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!runtime_) {
      runtime_ =
          std::make_unique<NativeAgentRuntime>(Mem::getSystemMemService());
    }
    return *runtime_;
  }

  IpcApprovalBroker &getApprovalBroker() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!approvalBroker_) {
      approvalBroker_ = std::make_unique<IpcApprovalBroker>();
    }
    return *approvalBroker_;
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
  std::mutex mutex_;
  std::unique_ptr<NativeAgentRuntime> runtime_;
  std::unique_ptr<IpcApprovalBroker> approvalBroker_;
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
