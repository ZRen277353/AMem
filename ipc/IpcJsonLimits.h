#pragma once

#include "IpcProtocol.h"
#include "../utils/BoundedJson.h"

namespace NativeIpc {

inline constexpr utils::JsonComplexityLimits kHandshakeJsonLimits = {
    IpcProtocol::kMaxHandshakePayloadBytes,
    16u,
    1024u,
    512u,
    IpcProtocol::kMaxHandshakePayloadBytes,
    IpcProtocol::kMaxHandshakePayloadBytes,
};

inline constexpr utils::JsonComplexityLimits kRequestJsonLimits = {
    IpcProtocol::kMaxRequestPayloadBytes,
    32u,
    16384u,
    4096u,
    IpcProtocol::kMaxRequestPayloadBytes,
    IpcProtocol::kMaxRequestPayloadBytes,
};

inline constexpr utils::JsonComplexityLimits kResponseJsonLimits = {
    IpcProtocol::kMaxResponsePayloadBytes,
    32u,
    65536u,
    4096u,
    IpcProtocol::kMaxResponsePayloadBytes,
    IpcProtocol::kMaxResponsePayloadBytes,
};

} // namespace NativeIpc
