#include "IpcProtocol.h"

#include <utility>

namespace IpcProtocol {
namespace {

void appendU16(std::vector<uint8_t>& output, uint16_t value) {
    output.push_back(static_cast<uint8_t>(value & 0xffu));
    output.push_back(static_cast<uint8_t>((value >> 8u) & 0xffu));
}

void appendU32(std::vector<uint8_t>& output, uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        output.push_back(static_cast<uint8_t>((value >> shift) & 0xffu));
    }
}

void appendU64(std::vector<uint8_t>& output, uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        output.push_back(static_cast<uint8_t>((value >> shift) & 0xffu));
    }
}

uint16_t readU16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0]) |
           (static_cast<uint16_t>(data[1]) << 8u);
}

uint32_t readU32(const uint8_t* data) {
    uint32_t value = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
        value |= static_cast<uint32_t>(*data++) << shift;
    }
    return value;
}

uint64_t readU64(const uint8_t* data) {
    uint64_t value = 0;
    for (unsigned shift = 0; shift < 64; shift += 8) {
        value |= static_cast<uint64_t>(*data++) << shift;
    }
    return value;
}

bool requiresRequestId(MessageType type) {
    return type == MessageType::Request ||
           type == MessageType::Response ||
           type == MessageType::Cancel;
}

bool requiresZeroRequestId(MessageType type) {
    return type == MessageType::Hello || type == MessageType::HelloAck;
}

uint32_t payloadLimitForType(MessageType type, uint32_t configuredLimit) {
    const uint32_t protocolLimit = type == MessageType::Request
                                       ? kMaxRequestPayloadBytes
                                       : kMaxFramePayloadBytes;
    return configuredLimit < protocolLimit ? configuredLimit : protocolLimit;
}

bool isValidUtf8(const uint8_t* data, size_t size) {
    size_t index = 0;
    while (index < size) {
        const uint8_t lead = data[index++];
        if (lead <= 0x7fu) {
            continue;
        }

        size_t continuationCount = 0;
        uint32_t codePoint = 0;
        uint32_t minimum = 0;
        if (lead >= 0xc2u && lead <= 0xdfu) {
            continuationCount = 1;
            codePoint = lead & 0x1fu;
            minimum = 0x80u;
        } else if (lead >= 0xe0u && lead <= 0xefu) {
            continuationCount = 2;
            codePoint = lead & 0x0fu;
            minimum = 0x800u;
        } else if (lead >= 0xf0u && lead <= 0xf4u) {
            continuationCount = 3;
            codePoint = lead & 0x07u;
            minimum = 0x10000u;
        } else {
            return false;
        }

        if (continuationCount > size - index) {
            return false;
        }
        for (size_t i = 0; i < continuationCount; ++i) {
            const uint8_t continuation = data[index++];
            if ((continuation & 0xc0u) != 0x80u) {
                return false;
            }
            codePoint = (codePoint << 6u) | (continuation & 0x3fu);
        }

        if (codePoint < minimum || codePoint > 0x10ffffu ||
            (codePoint >= 0xd800u && codePoint <= 0xdfffu)) {
            return false;
        }
    }
    return true;
}

bool validateFrameMetadata(MessageType type,
                           uint64_t requestId,
                           std::string& error) {
    if (requiresRequestId(type) && requestId == 0) {
        error = "message type requires a non-zero request id";
        return false;
    }
    if (requiresZeroRequestId(type) && requestId != 0) {
        error = "handshake message requires request id zero";
        return false;
    }
    return true;
}

DecodeResult invalidResult(const char* message) {
    DecodeResult result;
    result.status = DecodeStatus::Invalid;
    result.error = message;
    return result;
}

} // namespace

bool IsKnownMessageType(uint16_t value) {
    return value >= static_cast<uint16_t>(MessageType::Hello) &&
           value <= static_cast<uint16_t>(MessageType::Error);
}

bool EncodeFrame(const Frame& frame,
                 std::vector<uint8_t>& output,
                 std::string& error,
                 uint32_t maxPayloadBytes) {
    output.clear();
    error.clear();

    const uint16_t rawType = static_cast<uint16_t>(frame.type);
    if (!IsKnownMessageType(rawType)) {
        error = "unknown message type";
        return false;
    }
    if (!validateFrameMetadata(frame.type, frame.requestId, error)) {
        return false;
    }
    const uint32_t payloadLimit =
        payloadLimitForType(frame.type, maxPayloadBytes);
    if (frame.payload.size() > payloadLimit) {
        error = "payload exceeds configured frame limit";
        return false;
    }
    const auto* payloadData =
        reinterpret_cast<const uint8_t*>(frame.payload.data());
    if (!isValidUtf8(payloadData, frame.payload.size())) {
        error = "payload is not valid UTF-8";
        return false;
    }

    output.reserve(kHeaderSize + frame.payload.size());
    appendU32(output, kMagic);
    appendU16(output, kVersionMajor);
    appendU16(output, kVersionMinor);
    appendU16(output, rawType);
    appendU16(output, 0);
    appendU64(output, frame.requestId);
    appendU32(output, static_cast<uint32_t>(frame.payload.size()));
    if (!frame.payload.empty()) {
        output.insert(output.end(), payloadData,
                      payloadData + frame.payload.size());
    }
    return true;
}

DecodeResult DecodeFrame(const uint8_t* data,
                         size_t size,
                         uint32_t maxPayloadBytes) {
    if (data == nullptr && size != 0) {
        return invalidResult("input pointer is null");
    }
    if (size < kHeaderSize) {
        return {};
    }

    const uint32_t magic = readU32(data);
    const uint16_t versionMajor = readU16(data + 4);
    const uint16_t versionMinor = readU16(data + 6);
    const uint16_t rawType = readU16(data + 8);
    const uint16_t flags = readU16(data + 10);
    const uint64_t requestId = readU64(data + 12);
    const uint32_t payloadLength = readU32(data + 20);

    if (magic != kMagic) {
        return invalidResult("invalid frame magic");
    }
    if (versionMajor != kVersionMajor || versionMinor != kVersionMinor) {
        return invalidResult("unsupported protocol version");
    }
    if (!IsKnownMessageType(rawType)) {
        return invalidResult("unknown message type");
    }
    if (flags != 0) {
        return invalidResult("unsupported frame flags");
    }
    const MessageType type = static_cast<MessageType>(rawType);
    if (payloadLength > payloadLimitForType(type, maxPayloadBytes)) {
        return invalidResult("payload exceeds configured frame limit");
    }

    std::string metadataError;
    if (!validateFrameMetadata(type, requestId, metadataError)) {
        DecodeResult result;
        result.status = DecodeStatus::Invalid;
        result.error = std::move(metadataError);
        return result;
    }

    const size_t frameSize = kHeaderSize + payloadLength;
    if (size < frameSize) {
        return {};
    }
    const uint8_t* payload = data + kHeaderSize;
    if (!isValidUtf8(payload, payloadLength)) {
        return invalidResult("payload is not valid UTF-8");
    }

    DecodeResult result;
    result.status = DecodeStatus::Complete;
    result.frame.type = type;
    result.frame.requestId = requestId;
    result.frame.payload.assign(reinterpret_cast<const char*>(payload),
                                payloadLength);
    result.consumed = frameSize;
    return result;
}

} // namespace IpcProtocol
