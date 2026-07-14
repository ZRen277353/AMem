#include "ipc/IpcMethodCatalog.h"

#include <functional>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void testCanonicalNamesAreUnique() {
    expect(NativeIpc::IpcMethodCatalogSize() == 24,
           "IPC catalog must contain 24 canonical methods");
    std::set<std::string> names;
    for (auto method = NativeIpc::IpcMethodCatalogBegin();
         method != NativeIpc::IpcMethodCatalogEnd(); ++method) {
        expect(method->name != nullptr && method->name[0] != '\0',
               "method names must be non-empty");
        expect(names.insert(method->name).second,
               std::string("duplicate IPC method: ") + method->name);
        expect(NativeIpc::FindIpcMethod(method->name) == method,
               "catalog lookup must return the stable descriptor");
    }
    expect(NativeIpc::FindIpcMethod("read_memory") == nullptr,
           "retired aliases must not enter the native IPC catalog");
}

void testCapabilityClassification() {
    size_t observe = 0;
    size_t selection = 0;
    size_t mutation = 0;
    size_t host = 0;
    for (auto method = NativeIpc::IpcMethodCatalogBegin();
         method != NativeIpc::IpcMethodCatalogEnd(); ++method) {
        switch (method->capability) {
        case NativeIpc::IpcCapability::Observe:
            ++observe;
            break;
        case NativeIpc::IpcCapability::TargetSelection:
            ++selection;
            break;
        case NativeIpc::IpcCapability::TargetMutation:
            ++mutation;
            break;
        case NativeIpc::IpcCapability::HostExecution:
            ++host;
            break;
        }
    }
    expect(observe == 12 && selection == 1 && mutation == 9 && host == 2,
           "capability counts must remain explicit and reviewable");
    expect(NativeIpc::FindIpcMethod("process_open")->capability ==
               NativeIpc::IpcCapability::TargetSelection,
           "process_open must require TargetSelection");
    expect(NativeIpc::FindIpcMethod("lua_execute")->capability ==
               NativeIpc::IpcCapability::HostExecution,
           "Lua must require HostExecution");
}

void testTargetPolicies() {
    size_t none = 0;
    size_t bound = 0;
    size_t selection = 0;
    for (auto method = NativeIpc::IpcMethodCatalogBegin();
         method != NativeIpc::IpcMethodCatalogEnd(); ++method) {
        switch (method->targetPolicy) {
        case NativeIpc::IpcMethodTargetPolicy::None:
            ++none;
            break;
        case NativeIpc::IpcMethodTargetPolicy::Bound:
            ++bound;
            break;
        case NativeIpc::IpcMethodTargetPolicy::Selection:
            ++selection;
            break;
        }
    }
    expect(none == 3 && bound == 19 && selection == 2,
           "target-policy counts must match the canonical surface");
    expect(NativeIpc::FindIpcMethod("lua_execute")->targetPolicy ==
               NativeIpc::IpcMethodTargetPolicy::Selection,
           "Lua must be allowed to report a controlled final target");
}

void testApprovalAndServiceBoundaries() {
    size_t observeExecutable = 0;
    for (auto method = NativeIpc::IpcMethodCatalogBegin();
         method != NativeIpc::IpcMethodCatalogEnd(); ++method) {
        if (method->executableWithoutApproval) {
            ++observeExecutable;
            expect(method->capability == NativeIpc::IpcCapability::Observe &&
                       method->memServiceBacked,
                   "only MemService-backed Observe methods may execute");
        } else {
            expect(method->capability != NativeIpc::IpcCapability::Observe,
                   "Observe methods must not accidentally require approval");
        }
    }
    expect(observeExecutable == 12,
           "all 12 Observe methods should share the native adapter");
    expect(!NativeIpc::FindIpcMethod("lua_execute")->memServiceBacked,
           "Lua remains outside MemService");
}

} // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"canonical unique names", &testCanonicalNamesAreUnique},
        {"capability classification", &testCapabilityClassification},
        {"target policies", &testTargetPolicies},
        {"approval and service boundaries", &testApprovalAndServiceBoundaries},
    };

    int failures = 0;
    for (const auto& test : tests) {
        try {
            test.second();
            std::cout << "[PASS] " << test.first << '\n';
        } catch (const std::exception& exception) {
            ++failures;
            std::cerr << "[FAIL] " << test.first << ": "
                      << exception.what() << '\n';
        }
    }
    if (failures != 0) {
        std::cerr << failures << " test group(s) failed\n";
        return 1;
    }
    std::cout << tests.size() << " test groups passed\n";
    return 0;
}
