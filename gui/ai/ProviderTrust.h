#pragma once

#ifdef HAVE_AI_CHAT

#include "AIProvider.h"

#include <string>
#include <string_view>

namespace AI {

std::string canonicalEndpointForTrust(std::string_view endpoint);
bool isDefaultProviderEndpoint(const AIProvider& provider,
                               std::string_view endpoint);
bool isProviderEndpointTrusted(const AIProvider& provider,
                               const ProviderConfig& config);

} // namespace AI

#endif // HAVE_AI_CHAT
