#include "SystemNativeAgentRuntime.h"

#include "NativeAgentRuntime.h"
#include "mem/SystemMemService.h"

#include <memory>
#include <mutex>

namespace NativeIpc {
namespace {

class SystemRuntimeOwner final {
public:
  NativeAgentRuntime &get() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!runtime_) {
      runtime_ =
          std::make_unique<NativeAgentRuntime>(Mem::getSystemMemService());
    }
    return *runtime_;
  }

  void shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (runtime_) {
      runtime_->stop();
    }
  }

private:
  std::mutex mutex_;
  std::unique_ptr<NativeAgentRuntime> runtime_;
};

SystemRuntimeOwner &owner() {
  static SystemRuntimeOwner value;
  return value;
}

} // namespace

NativeAgentRuntime &GetSystemNativeAgentRuntime() { return owner().get(); }

void ShutdownSystemNativeAgentRuntime() { owner().shutdown(); }

} // namespace NativeIpc
