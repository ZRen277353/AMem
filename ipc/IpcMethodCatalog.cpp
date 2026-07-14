#include "IpcMethodCatalog.h"

#include <algorithm>
#include <array>

namespace NativeIpc {
namespace {

constexpr std::array<IpcMethodDescriptor, 24> kMethods = {{
    {"status", IpcCapability::Observe, IpcMethodTargetPolicy::None, true,
     true},
    {"driver_initialize", IpcCapability::HostExecution,
     IpcMethodTargetPolicy::None, true, false},
    {"memory_read", IpcCapability::Observe,
     IpcMethodTargetPolicy::Bound, true, true},
    {"memory_read_value", IpcCapability::Observe,
     IpcMethodTargetPolicy::Bound, true, true},
    {"memory_write", IpcCapability::TargetMutation,
     IpcMethodTargetPolicy::Bound, true, false},
    {"memory_write_value", IpcCapability::TargetMutation,
     IpcMethodTargetPolicy::Bound, true, false},
    {"scan_start", IpcCapability::TargetMutation,
     IpcMethodTargetPolicy::Bound, true, false},
    {"scan_refine", IpcCapability::TargetMutation,
     IpcMethodTargetPolicy::Bound, true, false},
    {"scan_results", IpcCapability::Observe,
     IpcMethodTargetPolicy::Bound, true, true},
    {"scan_clear", IpcCapability::TargetMutation,
     IpcMethodTargetPolicy::Bound, true, false},
    {"module_list", IpcCapability::Observe,
     IpcMethodTargetPolicy::Bound, true, true},
    {"module_resolve", IpcCapability::Observe,
     IpcMethodTargetPolicy::Bound, true, true},
    {"process_list", IpcCapability::Observe,
     IpcMethodTargetPolicy::None, true, true},
    {"process_open", IpcCapability::TargetSelection,
     IpcMethodTargetPolicy::Selection, true, false},
    {"pointer_resolve", IpcCapability::Observe,
     IpcMethodTargetPolicy::Bound, true, true},
    {"disassemble", IpcCapability::Observe,
     IpcMethodTargetPolicy::Bound, true, true},
    {"breakpoint_set", IpcCapability::TargetMutation,
     IpcMethodTargetPolicy::Bound, true, false},
    {"breakpoint_remove", IpcCapability::TargetMutation,
     IpcMethodTargetPolicy::Bound, true, false},
    {"breakpoint_hits", IpcCapability::Observe,
     IpcMethodTargetPolicy::Bound, true, true},
    {"breakpoint_suspend", IpcCapability::TargetMutation,
     IpcMethodTargetPolicy::Bound, true, false},
    {"breakpoint_resume", IpcCapability::TargetMutation,
     IpcMethodTargetPolicy::Bound, true, false},
    {"symbol_resolve", IpcCapability::Observe,
     IpcMethodTargetPolicy::Bound, true, true},
    {"symbol_list", IpcCapability::Observe,
     IpcMethodTargetPolicy::Bound, true, true},
    {"lua_execute", IpcCapability::HostExecution,
     IpcMethodTargetPolicy::Selection, false, false},
}};

} // namespace

const IpcMethodDescriptor* IpcMethodCatalogBegin() {
    return kMethods.data();
}

const IpcMethodDescriptor* IpcMethodCatalogEnd() {
    return kMethods.data() + kMethods.size();
}

size_t IpcMethodCatalogSize() {
    return kMethods.size();
}

const IpcMethodDescriptor* FindIpcMethod(const std::string& name) {
    const auto found = std::find_if(
        kMethods.begin(), kMethods.end(), [&name](const auto& method) {
            return name == method.name;
        });
    return found == kMethods.end() ? nullptr : &*found;
}

} // namespace NativeIpc
