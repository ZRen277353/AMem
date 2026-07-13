#include "ServerHandshake.h"

#include "socket_io_timeout.h"

#include <string_view>
#include <utility>
#include <vector>

namespace AmemServerHandshake {

bool ReadVersion(WindowsSocketClient& client, ServerVersionInfo& outInfo) {
    unsigned char command = CMD_GETVERSION;
    if (!client.Send(&command, sizeof(command))) {
        return false;
    }

    CeVersion version{};
    if (!client.Receive(&version, sizeof(version))) {
        return false;
    }

    ServerVersionInfo info;
    info.version = version.version;
    if (version.stringsize > 0) {
        std::vector<char> versionString(version.stringsize);
        if (!client.Receive(versionString.data(), versionString.size())) {
            return false;
        }
        info.versionString.assign(versionString.begin(), versionString.end());
    }

    outInfo = std::move(info);
    return true;
}

bool IsCompatible(const ServerVersionInfo& info) {
    constexpr std::string_view kIdentity = "CHEATENGINE";
    return info.version > 0 &&
           info.versionString.size() >= kIdentity.size() &&
           std::string_view(info.versionString).substr(0, kIdentity.size()) ==
               kIdentity;
}

bool Validate(WindowsSocketClient& client) {
    SocketIoTimeout::ScopedTimeout timeout(kTimeoutSeconds);
    ServerVersionInfo info;
    return ReadVersion(client, info) && IsCompatible(info);
}

} // namespace AmemServerHandshake
