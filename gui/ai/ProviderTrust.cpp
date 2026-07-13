#ifdef HAVE_AI_CHAT

#include "ProviderTrust.h"

#include <algorithm>
#include <cctype>
#include <cstddef>

namespace AI {

std::string canonicalEndpointForTrust(std::string_view endpoint) {
    size_t begin = 0;
    size_t end = endpoint.size();
    while (begin < end &&
           std::isspace(static_cast<unsigned char>(endpoint[begin]))) {
        ++begin;
    }
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(endpoint[end - 1]))) {
        --end;
    }

    std::string canonical(endpoint.substr(begin, end - begin));
    while (canonical.size() > 1 && canonical.back() == '/') {
        canonical.pop_back();
    }

    const size_t scheme = canonical.find("://");
    if (scheme == std::string::npos) {
        return canonical;
    }
    size_t authorityEnd = canonical.find('/', scheme + 3);
    if (authorityEnd == std::string::npos) {
        authorityEnd = canonical.size();
    }
    std::transform(canonical.begin(),
                   canonical.begin() +
                       static_cast<std::ptrdiff_t>(authorityEnd),
                   canonical.begin(),
                   [](unsigned char value) {
                       return static_cast<char>(std::tolower(value));
                   });
    return canonical;
}

bool isDefaultProviderEndpoint(const AIProvider& provider,
                               std::string_view endpoint) {
    return canonicalEndpointForTrust(endpoint) ==
        canonicalEndpointForTrust(provider.getDefaultBaseUrl());
}

bool isProviderEndpointTrusted(const AIProvider& provider,
                               const ProviderConfig& config) {
    if (isDefaultProviderEndpoint(provider, config.baseUrl)) {
        return true;
    }
    const std::string endpoint = canonicalEndpointForTrust(config.baseUrl);
    return !endpoint.empty() && endpoint ==
        canonicalEndpointForTrust(config.trustedBaseUrl);
}

} // namespace AI

#endif // HAVE_AI_CHAT
