#include "ipc/IpcProtocol.h"

#include <cstdint>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::vector<uint8_t> encode(const IpcProtocol::Frame& frame,
                            uint32_t limit =
                                IpcProtocol::kMaxFramePayloadBytes) {
    std::vector<uint8_t> bytes;
    std::string error;
    expect(IpcProtocol::EncodeFrame(frame, bytes, error, limit),
           "frame encoding failed: " + error);
    return bytes;
}

void writeU16(std::vector<uint8_t>& bytes, size_t offset, uint16_t value) {
    bytes.at(offset) = static_cast<uint8_t>(value & 0xffu);
    bytes.at(offset + 1) = static_cast<uint8_t>((value >> 8u) & 0xffu);
}

void writeU32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        bytes.at(offset++) =
            static_cast<uint8_t>((value >> shift) & 0xffu);
    }
}

void expectInvalid(const std::vector<uint8_t>& bytes,
                   const std::string& expectedError,
                   uint32_t limit = IpcProtocol::kMaxFramePayloadBytes) {
    const auto decoded = IpcProtocol::DecodeFrame(bytes, limit);
    expect(decoded.status == IpcProtocol::DecodeStatus::Invalid,
           "frame should be invalid");
    expect(decoded.error.find(expectedError) != std::string::npos,
           "unexpected decode error: " + decoded.error);
    expect(decoded.consumed == 0,
           "invalid frame must not consume input");
}

void testWireEncodingAndRoundTrip() {
    const IpcProtocol::Frame frame{
        IpcProtocol::MessageType::Request,
        0x1122334455667788ull,
        "{}",
    };
    const auto bytes = encode(frame);
    const std::vector<uint8_t> expected = {
        0x41, 0x4d, 0x45, 0x4d,
        0x01, 0x00, 0x00, 0x00,
        0x03, 0x00, 0x00, 0x00,
        0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,
        0x02, 0x00, 0x00, 0x00,
        0x7b, 0x7d,
    };
    expect(bytes == expected,
           "frame wire layout must be stable little-endian bytes");

    const auto decoded = IpcProtocol::DecodeFrame(bytes);
    expect(decoded.status == IpcProtocol::DecodeStatus::Complete,
           "encoded frame should decode");
    expect(decoded.frame.type == frame.type &&
               decoded.frame.requestId == frame.requestId &&
               decoded.frame.payload == frame.payload,
           "frame round trip should preserve fields");
    expect(decoded.consumed == bytes.size(),
           "decoder should report exact frame size");

    const auto empty = encode(
        {IpcProtocol::MessageType::Hello, 0, {}});
    expect(empty.size() == IpcProtocol::kHeaderSize,
           "empty payload should encode as a header-only frame");
    const auto decodedEmpty = IpcProtocol::DecodeFrame(empty);
    expect(decodedEmpty.status == IpcProtocol::DecodeStatus::Complete &&
               decodedEmpty.frame.payload.empty(),
           "header-only frame should round trip");
}

void testMessageTypesAndRequestIds() {
    const std::vector<std::pair<IpcProtocol::MessageType, uint64_t>> cases = {
        {IpcProtocol::MessageType::Hello, 0},
        {IpcProtocol::MessageType::HelloAck, 0},
        {IpcProtocol::MessageType::Request, 1},
        {IpcProtocol::MessageType::Response, 1},
        {IpcProtocol::MessageType::Cancel, 1},
        {IpcProtocol::MessageType::Error, 0},
        {IpcProtocol::MessageType::Error, 1},
    };
    for (const auto& [type, requestId] : cases) {
        const IpcProtocol::Frame frame{type, requestId, "{\"ok\":true}"};
        const auto decoded = IpcProtocol::DecodeFrame(encode(frame));
        expect(decoded.status == IpcProtocol::DecodeStatus::Complete &&
                   decoded.frame.type == type &&
                   decoded.frame.requestId == requestId,
               "known message type should round trip");
    }

    std::vector<uint8_t> bytes;
    std::string error;
    expect(!IpcProtocol::EncodeFrame(
               {IpcProtocol::MessageType::Request, 0, "{}"},
               bytes, error) &&
               error.find("non-zero") != std::string::npos,
           "request must require a request id");
    expect(!IpcProtocol::EncodeFrame(
               {IpcProtocol::MessageType::Hello, 4, "{}"},
               bytes, error) &&
               error.find("zero") != std::string::npos,
           "handshake must reject a request id");
}

void testPartialAndMultipleFrames() {
    const auto first = encode(
        {IpcProtocol::MessageType::Request, 7, "{\"method\":\"status\"}"});
    for (size_t size = 0; size < first.size(); ++size) {
        const auto decoded = IpcProtocol::DecodeFrame(first.data(), size);
        expect(decoded.status == IpcProtocol::DecodeStatus::NeedMoreData,
               "partial frame should request more data");
        expect(decoded.consumed == 0,
               "partial frame must not consume input");
    }

    const auto second = encode(
        {IpcProtocol::MessageType::Response, 7, "{\"success\":true}"});
    std::vector<uint8_t> combined = first;
    combined.insert(combined.end(), second.begin(), second.end());
    const auto decodedFirst = IpcProtocol::DecodeFrame(combined);
    expect(decodedFirst.status == IpcProtocol::DecodeStatus::Complete &&
               decodedFirst.consumed == first.size(),
           "decoder should consume one frame at a time");
    const auto decodedSecond = IpcProtocol::DecodeFrame(
        combined.data() + decodedFirst.consumed,
        combined.size() - decodedFirst.consumed);
    expect(decodedSecond.status == IpcProtocol::DecodeStatus::Complete &&
               decodedSecond.frame.type ==
                   IpcProtocol::MessageType::Response,
           "trailing frame should remain independently decodable");
}

void testHeaderValidationBeforePayloadRead() {
    const auto valid = encode(
        {IpcProtocol::MessageType::Request, 9, "{}"});

    auto bytes = valid;
    bytes[0] ^= 0xffu;
    expectInvalid(bytes, "magic");

    bytes = valid;
    writeU16(bytes, 4, 2);
    expectInvalid(bytes, "version");

    bytes = valid;
    writeU16(bytes, 6, 1);
    expectInvalid(bytes, "version");

    bytes = valid;
    writeU16(bytes, 8, 99);
    expectInvalid(bytes, "message type");

    bytes = valid;
    writeU16(bytes, 10, 1);
    expectInvalid(bytes, "flags");

    bytes = valid;
    for (size_t i = 12; i < 20; ++i) {
        bytes[i] = 0;
    }
    expectInvalid(bytes, "request id");

    bytes = valid;
    writeU32(bytes, 20, IpcProtocol::kMaxRequestPayloadBytes + 1);
    bytes.resize(IpcProtocol::kHeaderSize);
    expectInvalid(bytes, "limit", IpcProtocol::kMaxRequestPayloadBytes);
}

void testPayloadLimitsAndUtf8() {
    std::vector<uint8_t> bytes;
    std::string error;
    const std::string tooLarge(17, 'x');
    expect(!IpcProtocol::EncodeFrame(
               {IpcProtocol::MessageType::Request, 1, tooLarge},
               bytes, error, 16) &&
               bytes.empty() && error.find("limit") != std::string::npos,
           "encoder should reject oversized payloads");

    const std::string overRequestLimit(
        IpcProtocol::kMaxRequestPayloadBytes + 1u, 'x');
    expect(!IpcProtocol::EncodeFrame(
               {IpcProtocol::MessageType::Request, 1, overRequestLimit},
               bytes, error) &&
               bytes.empty() && error.find("limit") != std::string::npos,
           "request payload should enforce the 1 MiB protocol limit");
    expect(IpcProtocol::EncodeFrame(
               {IpcProtocol::MessageType::Response, 1, overRequestLimit},
               bytes, error),
           "response payload may exceed the request limit");
    writeU16(bytes, 8,
             static_cast<uint16_t>(IpcProtocol::MessageType::Request));
    expectInvalid(bytes, "limit");

    const std::string overFrameLimit(
        IpcProtocol::kMaxFramePayloadBytes + 1u, 'x');
    expect(!IpcProtocol::EncodeFrame(
               {IpcProtocol::MessageType::Response, 1, overFrameLimit},
               bytes, error, UINT32_MAX) &&
               bytes.empty() && error.find("limit") != std::string::npos,
           "configured limits must not raise the 4 MiB protocol limit");

    const std::string validUtf8("\xe4\xb8\xad", 3);
    expect(IpcProtocol::EncodeFrame(
               {IpcProtocol::MessageType::Request, 1, validUtf8},
               bytes, error),
           "valid multi-byte UTF-8 should encode");

    const std::vector<std::string> invalidPayloads = {
        std::string("\xc0\xaf", 2),
        std::string("\xed\xa0\x80", 3),
        std::string("\xf4\x90\x80\x80", 4),
        std::string("\xe4\xb8", 2),
    };
    for (const auto& payload : invalidPayloads) {
        expect(!IpcProtocol::EncodeFrame(
                   {IpcProtocol::MessageType::Request, 1, payload},
                   bytes, error) &&
                   error.find("UTF-8") != std::string::npos,
               "encoder should reject invalid UTF-8");
    }

    bytes = encode({IpcProtocol::MessageType::Request, 1, "x"});
    bytes.back() = 0xffu;
    expectInvalid(bytes, "UTF-8");
}

} // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"wire encoding and round trip", &testWireEncodingAndRoundTrip},
        {"message types and request ids", &testMessageTypesAndRequestIds},
        {"partial and multiple frames", &testPartialAndMultipleFrames},
        {"header validation", &testHeaderValidationBeforePayloadRead},
        {"payload limits and UTF-8", &testPayloadLimitsAndUtf8},
    };

    int failures = 0;
    for (const auto& test : tests) {
        try {
            test.second();
            std::cout << "[PASS] " << test.first << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[FAIL] " << test.first << ": "
                      << error.what() << '\n';
        }
    }
    if (failures != 0) {
        std::cerr << failures << " test group(s) failed\n";
        return 1;
    }
    std::cout << tests.size() << " test groups passed\n";
    return 0;
}
