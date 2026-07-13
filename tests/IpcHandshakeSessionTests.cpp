#include "ipc/IpcHandshakeSession.h"
#include "ipc/NamedPipeServer.h"

#include <nlohmann/json.hpp>
#include <windows.h>

#include <chrono>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using json = nlohmann::json;
using namespace std::chrono_literals;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::wstring uniquePipeName(const wchar_t* suffix) {
    return std::wstring(L"\\\\.\\pipe\\AMem.NativeAgent.HandshakeTest.") +
           std::to_wstring(::GetCurrentProcessId()) + L"." + suffix;
}

HANDLE connectPipe(const std::wstring& name, bool overlapped) {
    const ULONGLONG deadline = ::GetTickCount64() + 5000;
    do {
        HANDLE pipe = ::CreateFileW(
            name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, overlapped ? FILE_FLAG_OVERLAPPED : 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            return pipe;
        }
        if (::GetLastError() != ERROR_PIPE_BUSY) {
            return INVALID_HANDLE_VALUE;
        }
        ::WaitNamedPipeW(name.c_str(), 100);
    } while (::GetTickCount64() < deadline);
    return INVALID_HANDLE_VALUE;
}

std::vector<uint8_t> encode(const IpcProtocol::Frame& frame) {
    std::vector<uint8_t> bytes;
    std::string error;
    expect(IpcProtocol::EncodeFrame(frame, bytes, error),
           "test frame should encode: " + error);
    return bytes;
}

void writeU16(std::vector<uint8_t>& bytes, size_t offset, uint16_t value) {
    bytes.at(offset) = static_cast<uint8_t>(value & 0xffu);
    bytes.at(offset + 1) =
        static_cast<uint8_t>((value >> 8u) & 0xffu);
}

void writeU32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        bytes.at(offset++) =
            static_cast<uint8_t>((value >> shift) & 0xffu);
    }
}

void writeAll(HANDLE pipe, const uint8_t* data, size_t size) {
    size_t offset = 0;
    while (offset < size) {
        DWORD written = 0;
        expect(::WriteFile(pipe, data + offset,
                           static_cast<DWORD>(size - offset), &written,
                           nullptr) != FALSE &&
                   written != 0,
               "raw handshake write should complete");
        offset += written;
    }
}

struct SharedHandshake {
    void set(const NativeIpc::HandshakeResult& value) {
        std::lock_guard<std::mutex> lock(mutex);
        result = value;
    }

    NativeIpc::HandshakeResult get() const {
        std::lock_guard<std::mutex> lock(mutex);
        return result;
    }

    mutable std::mutex mutex;
    NativeIpc::HandshakeResult result;
};

std::string validHelloPayload(
    std::vector<std::string> capabilities = {"Observe"}) {
    return json({{"client_name", "AMem.Tests"},
                 {"client_version", "1.0"},
                 {"requested_capabilities", capabilities}})
        .dump();
}

void testValidHelloGrantsObserveOnly() {
    HANDLE done = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE clientStop = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    expect(done != nullptr && clientStop != nullptr,
           "valid handshake events should be created");
    SharedHandshake shared;
    NativeIpc::NamedPipeServer server(
        uniquePipeName(L"valid"),
        [&](HANDLE pipe, HANDLE stopEvent) {
            NativeIpc::IpcFramedConnection connection(pipe, stopEvent);
            NativeIpc::IpcHandshakeSession handshake(connection);
            const auto result = handshake.perform();
            shared.set(result);
            ::SetEvent(done);
            if (result.status == NativeIpc::HandshakeStatus::Established) {
                connection.readFrame(
                    std::chrono::steady_clock::now() + 5s);
            }
        });
    std::wstring error;
    expect(server.start(error), "valid handshake server should start");
    HANDLE client = connectPipe(server.snapshot().pipeName, true);
    expect(client != INVALID_HANDLE_VALUE,
           "valid handshake client should connect");
    NativeIpc::IpcFramedConnection connection(client, clientStop);
    const auto sent = connection.writeFrame(
        {IpcProtocol::MessageType::Hello, 0,
         validHelloPayload(
             {"Observe", "TargetSelection", "TargetMutation",
              "HostExecution"})},
        std::chrono::steady_clock::now() + 5s,
        NativeIpc::kMaxHandshakePayloadBytes);
    expect(sent.status == NativeIpc::FrameIoStatus::Complete,
           "Hello should be written");
    const auto response = connection.readFrame(
        std::chrono::steady_clock::now() + 5s,
        NativeIpc::kMaxHandshakePayloadBytes);
    expect(response.status == NativeIpc::FrameIoStatus::Complete &&
               response.frame.type == IpcProtocol::MessageType::HelloAck &&
               response.frame.requestId == 0,
           "valid Hello should receive HelloAck");
    const json ack = json::parse(response.frame.payload);
    expect(ack["protocol_version"]["major"] == 1 &&
               ack["protocol_version"]["minor"] == 0 &&
               ack["granted_capabilities"] == json::array({"Observe"}) &&
               ack["denied_capabilities"] ==
                   json::array({"TargetSelection", "TargetMutation",
                                "HostExecution"}),
           "HelloAck should grant Observe and deny privileged capabilities");
    expect(::WaitForSingleObject(done, 5000) == WAIT_OBJECT_0,
           "valid handshake handler should finish");
    const auto result = shared.get();
    expect(result.status == NativeIpc::HandshakeStatus::Established &&
               result.clientName == "AMem.Tests" &&
               result.grantedCapabilities.size() == 1 &&
               result.deniedCapabilities.size() == 3,
           "server handshake result should preserve negotiated identity");

    ::CloseHandle(client);
    server.stop();
    ::CloseHandle(clientStop);
    ::CloseHandle(done);
}

void testNonHelloFirstFrameRejected() {
    HANDLE done = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE clientStop = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SharedHandshake shared;
    NativeIpc::NamedPipeServer server(
        uniquePipeName(L"required"),
        [&](HANDLE pipe, HANDLE stopEvent) {
            NativeIpc::IpcFramedConnection connection(pipe, stopEvent);
            NativeIpc::IpcHandshakeSession handshake(connection);
            shared.set(handshake.perform());
            ::SetEvent(done);
        });
    std::wstring error;
    expect(server.start(error), "required-handshake server should start");
    HANDLE client = connectPipe(server.snapshot().pipeName, true);
    expect(client != INVALID_HANDLE_VALUE,
           "required-handshake client should connect");
    NativeIpc::IpcFramedConnection connection(client, clientStop);
    expect(connection
                   .writeFrame({IpcProtocol::MessageType::Request, 1, "{}"},
                               std::chrono::steady_clock::now() + 5s)
                   .status == NativeIpc::FrameIoStatus::Complete,
           "non-Hello first frame should be sent");
    const auto response = connection.readFrame(
        std::chrono::steady_clock::now() + 5s,
        NativeIpc::kMaxHandshakePayloadBytes);
    expect(response.status == NativeIpc::FrameIoStatus::Complete,
           "non-Hello rejection should be readable");
    const json payload = json::parse(response.frame.payload);
    expect(response.frame.type == IpcProtocol::MessageType::Error &&
               response.frame.requestId == 0 &&
               payload["code"] == "handshake_required",
           "non-Hello first frame should receive a structured error");
    ::CloseHandle(client);
    client = INVALID_HANDLE_VALUE;
    expect(::WaitForSingleObject(done, 5000) == WAIT_OBJECT_0 &&
               shared.get().status == NativeIpc::HandshakeStatus::Rejected,
           "server should reject the non-Hello session");

    server.stop();
    ::CloseHandle(clientStop);
    ::CloseHandle(done);
}

void runInvalidHelloCase(const wchar_t* suffix,
                         const std::string& helloPayload) {
    HANDLE done = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE clientStop = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SharedHandshake shared;
    NativeIpc::NamedPipeServer server(
        uniquePipeName(suffix),
        [&](HANDLE pipe, HANDLE stopEvent) {
            NativeIpc::IpcFramedConnection connection(pipe, stopEvent);
            NativeIpc::IpcHandshakeSession handshake(connection);
            shared.set(handshake.perform());
            ::SetEvent(done);
        });
    std::wstring error;
    expect(server.start(error), "invalid-Hello server should start");
    HANDLE client = connectPipe(server.snapshot().pipeName, true);
    expect(client != INVALID_HANDLE_VALUE,
           "invalid-Hello client should connect");
    NativeIpc::IpcFramedConnection connection(client, clientStop);
    expect(connection
                   .writeFrame({IpcProtocol::MessageType::Hello, 0,
                                helloPayload},
                               std::chrono::steady_clock::now() + 5s,
                               NativeIpc::kMaxHandshakePayloadBytes)
                   .status == NativeIpc::FrameIoStatus::Complete,
           "invalid Hello should still be framed");
    const auto response = connection.readFrame(
        std::chrono::steady_clock::now() + 5s,
        NativeIpc::kMaxHandshakePayloadBytes);
    expect(response.status == NativeIpc::FrameIoStatus::Complete,
           "invalid Hello rejection should be readable");
    const json payload = json::parse(response.frame.payload);
    expect(response.frame.type == IpcProtocol::MessageType::Error &&
               payload["code"] == "invalid_hello",
           "invalid Hello should receive invalid_hello");
    ::CloseHandle(client);
    client = INVALID_HANDLE_VALUE;
    expect(::WaitForSingleObject(done, 5000) == WAIT_OBJECT_0 &&
               shared.get().status == NativeIpc::HandshakeStatus::Rejected,
           "invalid Hello should reject the session");
    server.stop();
    ::CloseHandle(clientStop);
    ::CloseHandle(done);
}

void testMalformedAndUnknownCapabilitiesRejected() {
    runInvalidHelloCase(L"malformed", "{");
    runInvalidHelloCase(
        L"unknown",
        validHelloPayload({"Observe", "UnknownCapability"}));
    runInvalidHelloCase(
        L"duplicate", validHelloPayload({"Observe", "Observe"}));
    runInvalidHelloCase(
        L"empty-name",
        json({{"client_name", ""},
              {"requested_capabilities", json::array({"Observe"})}})
            .dump());
    runInvalidHelloCase(
        L"unknown-field",
        json({{"client_name", "AMem.Tests"},
              {"requested_capabilities", json::array({"Observe"})},
              {"unexpected", true}})
            .dump());

    std::string deepHello =
        "{\"client_name\":\"AMem.Tests\",\"requested_capabilities\":[\"Observe\"],\"nested\":";
    deepHello.append(17u, '[');
    deepHello += "null";
    deepHello.append(17u, ']');
    deepHello.push_back('}');
    runInvalidHelloCase(L"deep-json", deepHello);
}

void testRejectedClientDrainIsBounded() {
    HANDLE done = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE clientStop = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SharedHandshake shared;
    NativeIpc::NamedPipeServer server(
        uniquePipeName(L"drain-deadline"),
        [&](HANDLE pipe, HANDLE stopEvent) {
            NativeIpc::IpcFramedConnection connection(pipe, stopEvent);
            NativeIpc::IpcHandshakeSession handshake(
                connection, NativeIpc::HandshakeConfig{5s, 5s, 50ms});
            shared.set(handshake.perform());
            ::SetEvent(done);
        });
    std::wstring error;
    expect(server.start(error), "drain-deadline server should start");
    HANDLE client = connectPipe(server.snapshot().pipeName, true);
    expect(client != INVALID_HANDLE_VALUE,
           "drain-deadline client should connect");
    NativeIpc::IpcFramedConnection connection(client, clientStop);
    expect(connection
                   .writeFrame({IpcProtocol::MessageType::Request, 3, "{}"},
                               std::chrono::steady_clock::now() + 5s)
                   .status == NativeIpc::FrameIoStatus::Complete,
           "rejected request should be sent");
    const auto response = connection.readFrame(
        std::chrono::steady_clock::now() + 5s,
        NativeIpc::kMaxHandshakePayloadBytes);
    expect(response.status == NativeIpc::FrameIoStatus::Complete &&
               response.frame.type == IpcProtocol::MessageType::Error,
           "rejected request should receive Error");
    expect(::WaitForSingleObject(done, 2000) == WAIT_OBJECT_0 &&
               shared.get().status == NativeIpc::HandshakeStatus::Rejected,
           "rejection drain must not wait indefinitely for peer close");
    ::CloseHandle(client);
    server.stop();
    ::CloseHandle(clientStop);
    ::CloseHandle(done);
}

void testUnsupportedVersionClosesWithoutResponse() {
    HANDLE done = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SharedHandshake shared;
    NativeIpc::NamedPipeServer server(
        uniquePipeName(L"version"),
        [&](HANDLE pipe, HANDLE stopEvent) {
            NativeIpc::IpcFramedConnection connection(pipe, stopEvent);
            NativeIpc::IpcHandshakeSession handshake(connection);
            shared.set(handshake.perform());
            ::SetEvent(done);
        });
    std::wstring error;
    expect(server.start(error), "version server should start");
    HANDLE client = connectPipe(server.snapshot().pipeName, false);
    expect(client != INVALID_HANDLE_VALUE,
           "version client should connect");
    auto bytes = encode({IpcProtocol::MessageType::Hello, 0,
                         validHelloPayload()});
    writeU16(bytes, 4, 2);
    writeAll(client, bytes.data(), bytes.size());
    expect(::WaitForSingleObject(done, 5000) == WAIT_OBJECT_0 &&
               shared.get().status ==
                   NativeIpc::HandshakeStatus::ProtocolError,
           "unsupported header version should terminate the handshake");
    DWORD available = 0;
    const BOOL peeked =
        ::PeekNamedPipe(client, nullptr, 0, nullptr, &available, nullptr);
    expect(!peeked || available == 0,
           "unsupported version must close without an Error frame");
    ::CloseHandle(client);
    server.stop();
    ::CloseHandle(done);
}

void testHandshakeLimitRejectedAtHeader() {
    HANDLE done = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SharedHandshake shared;
    NativeIpc::NamedPipeServer server(
        uniquePipeName(L"limit"),
        [&](HANDLE pipe, HANDLE stopEvent) {
            NativeIpc::IpcFramedConnection connection(pipe, stopEvent);
            NativeIpc::IpcHandshakeSession handshake(connection);
            shared.set(handshake.perform());
            ::SetEvent(done);
        });
    std::wstring error;
    expect(server.start(error), "limit server should start");
    HANDLE client = connectPipe(server.snapshot().pipeName, false);
    expect(client != INVALID_HANDLE_VALUE,
           "limit client should connect");
    auto bytes = encode({IpcProtocol::MessageType::Hello, 0, "{}"});
    writeU32(bytes, 20, NativeIpc::kMaxHandshakePayloadBytes + 1u);
    writeAll(client, bytes.data(), IpcProtocol::kHeaderSize);
    expect(::WaitForSingleObject(done, 5000) == WAIT_OBJECT_0 &&
               shared.get().status ==
                   NativeIpc::HandshakeStatus::ProtocolError &&
               shared.get().error.find(L"limit") != std::wstring::npos,
           "oversized Hello should fail from header bytes alone");
    ::CloseHandle(client);
    server.stop();
    ::CloseHandle(done);
}

void testHandshakeReadTimeout() {
    HANDLE done = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SharedHandshake shared;
    NativeIpc::NamedPipeServer server(
        uniquePipeName(L"timeout"),
        [&](HANDLE pipe, HANDLE stopEvent) {
            NativeIpc::IpcFramedConnection connection(pipe, stopEvent);
            NativeIpc::IpcHandshakeSession handshake(
                connection, NativeIpc::HandshakeConfig{50ms, 5s, 1s});
            shared.set(handshake.perform());
            ::SetEvent(done);
        });
    std::wstring error;
    expect(server.start(error), "timeout server should start");
    HANDLE client = connectPipe(server.snapshot().pipeName, false);
    expect(client != INVALID_HANDLE_VALUE,
           "timeout client should connect");
    expect(::WaitForSingleObject(done, 5000) == WAIT_OBJECT_0 &&
               shared.get().status == NativeIpc::HandshakeStatus::TimedOut,
           "silent client should hit the handshake deadline");
    ::CloseHandle(client);
    server.stop();
    ::CloseHandle(done);
}

void testStopCancelsHandshakeAndJoins() {
    HANDLE started = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SharedHandshake shared;
    NativeIpc::NamedPipeServer server(
        uniquePipeName(L"cancel"),
        [&](HANDLE pipe, HANDLE stopEvent) {
            NativeIpc::IpcFramedConnection connection(pipe, stopEvent);
            NativeIpc::IpcHandshakeSession handshake(connection);
            ::SetEvent(started);
            shared.set(handshake.perform());
        });
    std::wstring error;
    expect(server.start(error), "cancel handshake server should start");
    HANDLE client = connectPipe(server.snapshot().pipeName, false);
    expect(client != INVALID_HANDLE_VALUE,
           "cancel handshake client should connect");
    expect(::WaitForSingleObject(started, 5000) == WAIT_OBJECT_0,
           "server should enter handshake read");
    server.stop();
    expect(shared.get().status == NativeIpc::HandshakeStatus::Cancelled &&
               server.snapshot().state == NativeIpc::ServerState::Stopped,
           "server stop should cancel and join handshake I/O");
    ::CloseHandle(client);
    ::CloseHandle(started);
}

} // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"valid Observe-only negotiation", &testValidHelloGrantsObserveOnly},
        {"non-Hello first frame", &testNonHelloFirstFrameRejected},
        {"malformed and unknown capabilities",
         &testMalformedAndUnknownCapabilitiesRejected},
        {"bounded rejection drain", &testRejectedClientDrainIsBounded},
        {"unsupported protocol version",
         &testUnsupportedVersionClosesWithoutResponse},
        {"handshake payload limit", &testHandshakeLimitRejectedAtHeader},
        {"handshake read timeout", &testHandshakeReadTimeout},
        {"handshake stop cancellation", &testStopCancelsHandshakeAndJoins},
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
