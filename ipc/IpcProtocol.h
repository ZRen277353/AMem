#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace IpcProtocol {

constexpr uint32_t kMagic = 0x4D454D41u; // "AMEM" on the wire.
constexpr uint16_t kVersionMajor = 1;
constexpr uint16_t kVersionMinor = 0;
constexpr size_t kHeaderSize = 24;
constexpr uint32_t kMaxHandshakePayloadBytes = 16u * 1024u;
constexpr uint32_t kMaxRequestPayloadBytes = 1u * 1024u * 1024u;
constexpr uint32_t kMaxResponsePayloadBytes = 4u * 1024u * 1024u;
constexpr uint32_t kMaxFramePayloadBytes = kMaxResponsePayloadBytes;

enum class MessageType : uint16_t {
    Hello = 1,
    HelloAck = 2,
    Request = 3,
    Response = 4,
    Cancel = 5,
    Error = 6,
};

enum class DecodeStatus {
    Complete,
    NeedMoreData,
    Invalid,
};

struct Frame {
    MessageType type = MessageType::Error;
    uint64_t requestId = 0;
    std::string payload;
};

struct FrameHeader {
    MessageType type = MessageType::Error;
    uint64_t requestId = 0;
    uint32_t payloadLength = 0;
};

struct HeaderDecodeResult {
    DecodeStatus status = DecodeStatus::NeedMoreData;
    FrameHeader header;
    std::string error;
};

struct DecodeResult {
    DecodeStatus status = DecodeStatus::NeedMoreData;
    Frame frame;
    size_t consumed = 0;
    std::string error;
};

bool IsKnownMessageType(uint16_t value);

HeaderDecodeResult DecodeHeader(
    const uint8_t* data,
    size_t size,
    uint32_t maxPayloadBytes = kMaxFramePayloadBytes);

bool EncodeFrame(const Frame& frame,
                 std::vector<uint8_t>& output,
                 std::string& error,
                 uint32_t maxPayloadBytes = kMaxFramePayloadBytes);

DecodeResult DecodeFrame(const uint8_t* data,
                         size_t size,
                         uint32_t maxPayloadBytes = kMaxFramePayloadBytes);

inline DecodeResult DecodeFrame(
    const std::vector<uint8_t>& data,
    uint32_t maxPayloadBytes = kMaxFramePayloadBytes) {
    return DecodeFrame(data.data(), data.size(), maxPayloadBytes);
}

inline HeaderDecodeResult DecodeHeader(
    const std::vector<uint8_t>& data,
    uint32_t maxPayloadBytes = kMaxFramePayloadBytes) {
    return DecodeHeader(data.data(), data.size(), maxPayloadBytes);
}

} // namespace IpcProtocol
