#include "IpcHandshakeSession.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <utility>

namespace NativeIpc {
namespace {

using json = nlohmann::json;

std::wstring widenAscii(const std::string& value) {
    return std::wstring(value.begin(), value.end());
}

bool hasAsciiControl(const std::string& value) {
    return std::any_of(value.begin(), value.end(), [](unsigned char value) {
        return value < 0x20u || value == 0x7fu;
    });
}

bool parseHelloPayload(const std::string& payload,
                       HandshakeResult& result,
                       std::string& error) {
    const json hello = json::parse(payload, nullptr, false);
    if (hello.is_discarded() || !hello.is_object()) {
        error = "hello payload must be a JSON object";
        return false;
    }

    const auto nameIt = hello.find("client_name");
    if (nameIt == hello.end() || !nameIt->is_string()) {
        error = "client_name must be a string";
        return false;
    }
    result.clientName = nameIt->get<std::string>();
    if (result.clientName.empty() ||
        result.clientName.size() > kMaxClientNameBytes ||
        hasAsciiControl(result.clientName)) {
        error = "client_name must be 1..128 bytes without control characters";
        return false;
    }

    const auto versionIt = hello.find("client_version");
    if (versionIt != hello.end()) {
        if (!versionIt->is_string()) {
            error = "client_version must be a string";
            return false;
        }
        result.clientVersion = versionIt->get<std::string>();
        if (result.clientVersion.size() > kMaxClientVersionBytes ||
            hasAsciiControl(result.clientVersion)) {
            error = "client_version exceeds 64 bytes or contains controls";
            return false;
        }
    }

    const auto capabilitiesIt = hello.find("requested_capabilities");
    if (capabilitiesIt == hello.end() || !capabilitiesIt->is_array()) {
        error = "requested_capabilities must be an array";
        return false;
    }
    if (capabilitiesIt->size() > 4) {
        error = "requested_capabilities exceeds the capability count";
        return false;
    }

    for (const auto& item : *capabilitiesIt) {
        if (!item.is_string()) {
            error = "each requested capability must be a string";
            return false;
        }
        IpcCapability capability = IpcCapability::Observe;
        const std::string name = item.get<std::string>();
        if (!ParseCapability(name, capability)) {
            error = "unknown capability: " + name;
            return false;
        }
        if (std::find(result.requestedCapabilities.begin(),
                      result.requestedCapabilities.end(), capability) !=
            result.requestedCapabilities.end()) {
            error = "duplicate capability: " + name;
            return false;
        }
        result.requestedCapabilities.push_back(capability);
        if (capability == IpcCapability::Observe) {
            result.grantedCapabilities.push_back(capability);
        } else {
            result.deniedCapabilities.push_back(capability);
        }
    }
    return true;
}

json capabilityArray(const std::vector<IpcCapability>& capabilities) {
    json result = json::array();
    for (IpcCapability capability : capabilities) {
        result.push_back(CapabilityName(capability));
    }
    return result;
}

} // namespace

const char* CapabilityName(IpcCapability capability) {
    switch (capability) {
    case IpcCapability::Observe:
        return "Observe";
    case IpcCapability::TargetSelection:
        return "TargetSelection";
    case IpcCapability::TargetMutation:
        return "TargetMutation";
    case IpcCapability::HostExecution:
        return "HostExecution";
    }
    return "Unknown";
}

bool ParseCapability(const std::string& name, IpcCapability& capability) {
    constexpr std::array<std::pair<const char*, IpcCapability>, 4> values = {{
        {"Observe", IpcCapability::Observe},
        {"TargetSelection", IpcCapability::TargetSelection},
        {"TargetMutation", IpcCapability::TargetMutation},
        {"HostExecution", IpcCapability::HostExecution},
    }};
    for (const auto& value : values) {
        if (name == value.first) {
            capability = value.second;
            return true;
        }
    }
    return false;
}

HandshakeResult IpcHandshakeSession::perform() {
    const PipeDeadline readDeadline =
        std::chrono::steady_clock::now() + config_.readTimeout;
    const FrameIoResult first =
        connection_.readFrame(readDeadline, kMaxHandshakePayloadBytes);
    if (first.status != FrameIoStatus::Complete) {
        return fromIoResult(first);
    }
    if (first.frame.type != IpcProtocol::MessageType::Hello) {
        return reject("handshake_required",
                      "the first frame must be Hello",
                      L"client sent a non-Hello first frame");
    }

    HandshakeResult result;
    std::string validationError;
    if (!parseHelloPayload(first.frame.payload, result, validationError)) {
        return reject("invalid_hello", validationError,
                      widenAscii(validationError));
    }

    const json ack = {
        {"server_name", "AMem.NativeAgent"},
        {"protocol_version",
         {{"major", IpcProtocol::kVersionMajor},
          {"minor", IpcProtocol::kVersionMinor}}},
        {"granted_capabilities",
         capabilityArray(result.grantedCapabilities)},
        {"denied_capabilities", capabilityArray(result.deniedCapabilities)},
    };
    const FrameIoResult written = connection_.writeFrame(
        {IpcProtocol::MessageType::HelloAck, 0, ack.dump()},
        std::chrono::steady_clock::now() + config_.writeTimeout,
        kMaxHandshakePayloadBytes);
    if (written.status != FrameIoStatus::Complete) {
        HandshakeResult writeResult = fromIoResult(written);
        writeResult.clientName = std::move(result.clientName);
        writeResult.clientVersion = std::move(result.clientVersion);
        writeResult.requestedCapabilities =
            std::move(result.requestedCapabilities);
        writeResult.grantedCapabilities =
            std::move(result.grantedCapabilities);
        writeResult.deniedCapabilities =
            std::move(result.deniedCapabilities);
        return writeResult;
    }

    result.status = HandshakeStatus::Established;
    return result;
}

HandshakeResult IpcHandshakeSession::reject(
    const char* code,
    const std::string& message,
    const std::wstring& internalError) {
    const json payload = {{"code", code}, {"message", message}};
    const FrameIoResult written = connection_.writeFrame(
        {IpcProtocol::MessageType::Error, 0, payload.dump()},
        std::chrono::steady_clock::now() + config_.writeTimeout,
        kMaxHandshakePayloadBytes);
    if (written.status != FrameIoStatus::Complete) {
        return fromIoResult(written);
    }

    const FrameIoResult drained = connection_.readFrame(
        std::chrono::steady_clock::now() + config_.rejectionDrainTimeout,
        kMaxHandshakePayloadBytes);
    if (drained.status == FrameIoStatus::Cancelled) {
        return fromIoResult(drained);
    }
    HandshakeResult result;
    result.status = HandshakeStatus::Rejected;
    result.error = internalError;
    return result;
}

HandshakeResult IpcHandshakeSession::fromIoResult(
    const FrameIoResult& io) const {
    HandshakeResult result;
    result.error = io.error;
    switch (io.status) {
    case FrameIoStatus::Complete:
        result.status = HandshakeStatus::IoError;
        result.error = L"unexpected complete I/O result";
        break;
    case FrameIoStatus::Closed:
        result.status = HandshakeStatus::Closed;
        break;
    case FrameIoStatus::Cancelled:
        result.status = HandshakeStatus::Cancelled;
        break;
    case FrameIoStatus::TimedOut:
        result.status = HandshakeStatus::TimedOut;
        break;
    case FrameIoStatus::ProtocolError:
        result.status = HandshakeStatus::ProtocolError;
        break;
    case FrameIoStatus::IoError:
        result.status = HandshakeStatus::IoError;
        break;
    }
    return result;
}

} // namespace NativeIpc
