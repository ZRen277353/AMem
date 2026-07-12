#pragma once

#include "IpcHandshakeSession.h"

#include <cstddef>
#include <string>

namespace NativeIpc {

enum class IpcMethodTargetPolicy {
    None,
    Bound,
    Selection,
};

struct IpcMethodDescriptor {
    const char* name = "";
    IpcCapability capability = IpcCapability::Observe;
    IpcMethodTargetPolicy targetPolicy = IpcMethodTargetPolicy::None;
    bool memServiceBacked = false;
    bool executableWithoutApproval = false;
};

const IpcMethodDescriptor* IpcMethodCatalogBegin();
const IpcMethodDescriptor* IpcMethodCatalogEnd();
size_t IpcMethodCatalogSize();
const IpcMethodDescriptor* FindIpcMethod(const std::string& name);

} // namespace NativeIpc
