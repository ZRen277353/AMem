#include "ipc/IpcFramedConnection.h"
#include "ipc/NamedPipeServer.h"

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

using namespace std::chrono_literals;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::wstring uniquePipeName(const wchar_t* suffix) {
    return std::wstring(L"\\\\.\\pipe\\AMem.NativeAgent.FrameTest.") +
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
        const DWORD chunk = static_cast<DWORD>(size - offset);
        expect(::WriteFile(pipe, data + offset, chunk, &written, nullptr) !=
                   FALSE &&
                   written != 0,
               "raw pipe write should complete");
        offset += written;
    }
}

struct SharedResult {
    void set(const NativeIpc::FrameIoResult& value) {
        std::lock_guard<std::mutex> lock(mutex);
        result = value;
    }

    NativeIpc::FrameIoResult get() const {
        std::lock_guard<std::mutex> lock(mutex);
        return result;
    }

    mutable std::mutex mutex;
    NativeIpc::FrameIoResult result;
};

void testLargeRoundTripAndExactWrite() {
    HANDLE handlerDone = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE clientStop = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    expect(handlerDone != nullptr && clientStop != nullptr,
           "round-trip events should be created");

    SharedResult serverRead;
    SharedResult serverWrite;
    const std::string responsePayload(2u * 1024u * 1024u, 'r');
    NativeIpc::NamedPipeServer server(
        uniquePipeName(L"roundtrip"),
        [&](HANDLE pipe, HANDLE stopEvent) {
            NativeIpc::IpcFramedConnection connection(pipe, stopEvent);
            const auto request = connection.readFrame(
                std::chrono::steady_clock::now() + 5s);
            serverRead.set(request);
            if (request.status == NativeIpc::FrameIoStatus::Complete) {
                serverWrite.set(connection.writeFrame(
                    {IpcProtocol::MessageType::Response,
                     request.frame.requestId, responsePayload},
                    std::chrono::steady_clock::now() + 5s));
            }
            ::SetEvent(handlerDone);
        });
    std::wstring error;
    expect(server.start(error), "round-trip server should start");

    HANDLE client = connectPipe(server.snapshot().pipeName, true);
    expect(client != INVALID_HANDLE_VALUE,
           "round-trip client should connect");
    NativeIpc::IpcFramedConnection connection(client, clientStop);
    const std::string requestPayload(128u * 1024u, 'q');
    const auto sent = connection.writeFrame(
        {IpcProtocol::MessageType::Request, 42, requestPayload},
        std::chrono::steady_clock::now() + 5s);
    expect(sent.status == NativeIpc::FrameIoStatus::Complete,
           "large request should write exactly");
    const auto response = connection.readFrame(
        std::chrono::steady_clock::now() + 5s);
    expect(response.status == NativeIpc::FrameIoStatus::Complete &&
               response.frame.type == IpcProtocol::MessageType::Response &&
               response.frame.requestId == 42 &&
               response.frame.payload == responsePayload,
           "large response should round trip exactly");
    expect(::WaitForSingleObject(handlerDone, 5000) == WAIT_OBJECT_0,
           "round-trip handler should finish");
    expect(serverRead.get().frame.payload == requestPayload &&
               serverWrite.get().status ==
                   NativeIpc::FrameIoStatus::Complete,
           "server should receive and write complete frames");

    ::CloseHandle(client);
    server.stop();
    ::CloseHandle(clientStop);
    ::CloseHandle(handlerDone);
}

void testFragmentedFrameRead() {
    HANDLE done = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SharedResult shared;
    NativeIpc::NamedPipeServer server(
        uniquePipeName(L"fragmented"),
        [&](HANDLE pipe, HANDLE stopEvent) {
            NativeIpc::IpcFramedConnection connection(pipe, stopEvent);
            shared.set(connection.readFrame(
                std::chrono::steady_clock::now() + 5s));
            ::SetEvent(done);
        });
    std::wstring error;
    expect(server.start(error), "fragment server should start");
    HANDLE client = connectPipe(server.snapshot().pipeName, false);
    expect(client != INVALID_HANDLE_VALUE,
           "fragment client should connect");

    const auto bytes = encode(
        {IpcProtocol::MessageType::Request, 7, "fragmented-payload"});
    writeAll(client, bytes.data(), 7);
    ::Sleep(10);
    writeAll(client, bytes.data() + 7, bytes.size() - 7);
    expect(::WaitForSingleObject(done, 5000) == WAIT_OBJECT_0,
           "fragmented read should finish");
    const auto result = shared.get();
    expect(result.status == NativeIpc::FrameIoStatus::Complete &&
               result.frame.requestId == 7 &&
               result.frame.payload == "fragmented-payload",
           "exact read should assemble fragmented frame bytes");

    ::CloseHandle(client);
    server.stop();
    ::CloseHandle(done);
}

void testOversizedHeaderRejectedBeforePayload() {
    HANDLE done = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SharedResult shared;
    NativeIpc::NamedPipeServer server(
        uniquePipeName(L"oversized"),
        [&](HANDLE pipe, HANDLE stopEvent) {
            NativeIpc::IpcFramedConnection connection(pipe, stopEvent);
            shared.set(connection.readFrame(
                std::chrono::steady_clock::now() + 5s));
            ::SetEvent(done);
        });
    std::wstring error;
    expect(server.start(error), "oversized server should start");
    HANDLE client = connectPipe(server.snapshot().pipeName, false);
    expect(client != INVALID_HANDLE_VALUE,
           "oversized client should connect");

    auto bytes = encode(
        {IpcProtocol::MessageType::Response, 9, {}});
    writeU32(bytes, 20, IpcProtocol::kMaxFramePayloadBytes + 1u);
    writeAll(client, bytes.data(), IpcProtocol::kHeaderSize);
    expect(::WaitForSingleObject(done, 5000) == WAIT_OBJECT_0,
           "oversized header should fail without payload bytes");
    const auto result = shared.get();
    expect(result.status == NativeIpc::FrameIoStatus::ProtocolError &&
               result.error.find(L"limit") != std::wstring::npos &&
               result.bytesTransferred == IpcProtocol::kHeaderSize,
           "payload length must be rejected at the header boundary");

    ::CloseHandle(client);
    server.stop();
    ::CloseHandle(done);
}

void testInvalidUtf8RejectedAfterBoundedRead() {
    HANDLE done = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SharedResult shared;
    NativeIpc::NamedPipeServer server(
        uniquePipeName(L"utf8"),
        [&](HANDLE pipe, HANDLE stopEvent) {
            NativeIpc::IpcFramedConnection connection(pipe, stopEvent);
            shared.set(connection.readFrame(
                std::chrono::steady_clock::now() + 5s));
            ::SetEvent(done);
        });
    std::wstring error;
    expect(server.start(error), "UTF-8 server should start");
    HANDLE client = connectPipe(server.snapshot().pipeName, false);
    expect(client != INVALID_HANDLE_VALUE,
           "UTF-8 client should connect");

    auto bytes = encode({IpcProtocol::MessageType::Request, 11, "x"});
    bytes.back() = 0xffu;
    writeAll(client, bytes.data(), bytes.size());
    expect(::WaitForSingleObject(done, 5000) == WAIT_OBJECT_0,
           "invalid UTF-8 frame should finish");
    expect(shared.get().status ==
               NativeIpc::FrameIoStatus::ProtocolError &&
               shared.get().error.find(L"UTF-8") != std::wstring::npos,
           "frame payload must remain strict UTF-8");

    ::CloseHandle(client);
    server.stop();
    ::CloseHandle(done);
}

void testPartialHeaderCloseIsProtocolError() {
    HANDLE done = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE reading = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SharedResult shared;
    NativeIpc::NamedPipeServer server(
        uniquePipeName(L"truncated"),
        [&](HANDLE pipe, HANDLE stopEvent) {
            NativeIpc::IpcFramedConnection connection(pipe, stopEvent);
            ::SetEvent(reading);
            shared.set(connection.readFrame(
                std::chrono::steady_clock::now() + 5s));
            ::SetEvent(done);
        });
    std::wstring error;
    expect(server.start(error), "truncated server should start");
    HANDLE client = connectPipe(server.snapshot().pipeName, false);
    expect(client != INVALID_HANDLE_VALUE,
           "truncated client should connect");
    expect(::WaitForSingleObject(reading, 5000) == WAIT_OBJECT_0,
           "server should enter the truncated frame read");
    const auto bytes = encode(
        {IpcProtocol::MessageType::Request, 13, "payload"});
    writeAll(client, bytes.data(), 5);
    ::CloseHandle(client);
    expect(::WaitForSingleObject(done, 5000) == WAIT_OBJECT_0,
           "truncated read should finish");
    expect(shared.get().status ==
               NativeIpc::FrameIoStatus::ProtocolError &&
               shared.get().bytesTransferred == 5,
           "partial header close must not look like a clean disconnect");
    server.stop();
    ::CloseHandle(reading);
    ::CloseHandle(done);
}

void testReadDeadline() {
    HANDLE done = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SharedResult shared;
    NativeIpc::NamedPipeServer server(
        uniquePipeName(L"deadline"),
        [&](HANDLE pipe, HANDLE stopEvent) {
            NativeIpc::IpcFramedConnection connection(pipe, stopEvent);
            shared.set(connection.readFrame(
                std::chrono::steady_clock::now() + 50ms));
            ::SetEvent(done);
        });
    std::wstring error;
    expect(server.start(error), "deadline server should start");
    HANDLE client = connectPipe(server.snapshot().pipeName, false);
    expect(client != INVALID_HANDLE_VALUE,
           "deadline client should connect");
    expect(::WaitForSingleObject(done, 5000) == WAIT_OBJECT_0 &&
               shared.get().status == NativeIpc::FrameIoStatus::TimedOut,
           "idle frame read should honor its absolute deadline");
    ::CloseHandle(client);
    server.stop();
    ::CloseHandle(done);
}

void testStopCancelsFrameReadAndJoins() {
    HANDLE reading = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SharedResult shared;
    NativeIpc::NamedPipeServer server(
        uniquePipeName(L"cancel"),
        [&](HANDLE pipe, HANDLE stopEvent) {
            NativeIpc::IpcFramedConnection connection(pipe, stopEvent);
            ::SetEvent(reading);
            shared.set(connection.readFrame());
        });
    std::wstring error;
    expect(server.start(error), "cancel server should start");
    HANDLE client = connectPipe(server.snapshot().pipeName, false);
    expect(client != INVALID_HANDLE_VALUE,
           "cancel client should connect");
    expect(::WaitForSingleObject(reading, 5000) == WAIT_OBJECT_0,
           "server should enter framed read");
    server.stop();
    expect(shared.get().status == NativeIpc::FrameIoStatus::Cancelled &&
               server.snapshot().state == NativeIpc::ServerState::Stopped,
           "stop should cancel framed I/O before joining the handler");
    ::CloseHandle(client);
    ::CloseHandle(reading);
}

} // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"large round trip and exact write", &testLargeRoundTripAndExactWrite},
        {"fragmented frame read", &testFragmentedFrameRead},
        {"oversized header rejection",
         &testOversizedHeaderRejectedBeforePayload},
        {"invalid UTF-8 rejection", &testInvalidUtf8RejectedAfterBoundedRead},
        {"truncated header close", &testPartialHeaderCloseIsProtocolError},
        {"absolute read deadline", &testReadDeadline},
        {"stop cancellation and join", &testStopCancelsFrameReadAndJoins},
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
