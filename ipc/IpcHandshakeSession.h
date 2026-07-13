#pragma once

#include "IpcFramedConnection.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace NativeIpc {

constexpr uint32_t kMaxHandshakePayloadBytes =
    IpcProtocol::kMaxHandshakePayloadBytes;
constexpr size_t kMaxClientNameBytes = 128;
constexpr size_t kMaxClientVersionBytes = 64;

enum class IpcCapability {
    Observe,
    TargetSelection,
    TargetMutation,
    HostExecution,
};

const char* CapabilityName(IpcCapability capability);
bool ParseCapability(const std::string& name, IpcCapability& capability);

enum class HandshakeStatus {
    Established,
    Rejected,
    Closed,
    Cancelled,
    TimedOut,
    ProtocolError,
    IoError,
};

struct HandshakeConfig {
    std::chrono::milliseconds readTimeout{5000};
    std::chrono::milliseconds writeTimeout{5000};
    std::chrono::milliseconds rejectionDrainTimeout{1000};
};

struct HandshakeResult {
    HandshakeStatus status = HandshakeStatus::IoError;
    std::string clientName;
    std::string clientVersion;
    std::vector<IpcCapability> requestedCapabilities;
    std::vector<IpcCapability> grantedCapabilities;
    std::vector<IpcCapability> deniedCapabilities;
    std::wstring error;
};

class IpcHandshakeSession final {
public:
    explicit IpcHandshakeSession(IpcFramedConnection& connection,
                                 HandshakeConfig config = {})
        : connection_(connection), config_(config) {}

    // Established transfers the live connection to the caller's next session
    // stage. The owning pipe handler must not return immediately after it.
    HandshakeResult perform();

private:
    HandshakeResult reject(const char* code,
                           const std::string& message,
                           const std::wstring& internalError);
    HandshakeResult fromIoResult(const FrameIoResult& io) const;

    IpcFramedConnection& connection_;
    HandshakeConfig config_;
};

} // namespace NativeIpc
