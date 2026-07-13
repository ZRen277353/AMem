#pragma once

#include "client.hpp"

#include <string>

struct ServerVersionInfo {
    int version = 0;
    std::string versionString;
};

namespace AmemServerHandshake {

inline constexpr int kTimeoutSeconds = 5;

// This is the single low-level CMD_GETVERSION implementation. Both the
// connection handshake and normal status queries reuse it.
bool ReadVersion(WindowsSocketClient& client, ServerVersionInfo& outInfo);

bool IsCompatible(const ServerVersionInfo& info);
bool Validate(WindowsSocketClient& client);

} // namespace AmemServerHandshake
