#pragma once
#ifdef HAVE_AI_CHAT

#include "AiLimits.h"
#include "../../utils/BoundedJson.h"

namespace AI {

inline constexpr utils::JsonComplexityLimits kToolArgumentJsonLimits = {
    Limits::kMaxToolArgumentsPerCallBytes,
    Limits::kMaxToolJsonDepth,
    Limits::kMaxToolArgumentJsonNodes,
    Limits::kMaxToolJsonContainerItems,
    Limits::kMaxToolArgumentsPerCallBytes,
    Limits::kMaxToolArgumentsPerCallBytes,
};

inline constexpr utils::JsonComplexityLimits kToolResultJsonLimits = {
    Limits::kMaxToolResultBytes,
    Limits::kMaxToolJsonDepth,
    Limits::kMaxToolResultJsonNodes,
    Limits::kMaxToolJsonContainerItems,
    Limits::kMaxToolResultBytes,
    Limits::kMaxToolResultBytes,
};

inline constexpr utils::JsonComplexityLimits kToolSchemaJsonLimits = {
    Limits::kMaxToolArgumentsPerCallBytes,
    Limits::kMaxToolJsonDepth,
    Limits::kMaxToolSchemaJsonNodes,
    Limits::kMaxToolJsonContainerItems,
    Limits::kMaxToolArgumentsPerCallBytes,
    Limits::kMaxToolArgumentsPerCallBytes,
};

inline constexpr utils::JsonComplexityLimits kProviderResponseJsonLimits = {
    Limits::kMaxHttpResponseBytes,
    Limits::kMaxProviderJsonDepth,
    Limits::kMaxProviderJsonNodes,
    Limits::kMaxProviderJsonContainerItems,
    Limits::kMaxHttpResponseBytes,
    Limits::kMaxHttpResponseBytes,
};

inline constexpr utils::JsonComplexityLimits kProviderEventJsonLimits = {
    Limits::kMaxSseEventBytes,
    Limits::kMaxToolJsonDepth,
    Limits::kMaxProviderEventJsonNodes,
    Limits::kMaxToolJsonContainerItems,
    Limits::kMaxSseEventBytes,
    Limits::kMaxSseEventBytes,
};

} // namespace AI

#endif // HAVE_AI_CHAT
