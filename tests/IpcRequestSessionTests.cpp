#include "ipc/IpcHandshakeSession.h"
#include "ipc/IpcRequestSession.h"
#include "ipc/NamedPipeServer.h"

#include <nlohmann/json.hpp>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
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
    return std::wstring(L"\\\\.\\pipe\\AMem.NativeAgent.RequestTest.") +
           std::to_wstring(::GetCurrentProcessId()) + L"." + suffix;
}

HANDLE connectPipe(const std::wstring& name) {
    const ULONGLONG deadline = ::GetTickCount64() + 5000;
    do {
        HANDLE pipe = ::CreateFileW(
            name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
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

class TestDispatcher final : public NativeIpc::IIpcRequestDispatcher {
public:
    TestDispatcher() {
        started_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        expect(started_ != nullptr, "dispatcher event should be created");
    }

    ~TestDispatcher() override {
        ::CloseHandle(started_);
    }

    bool resolveCapability(const std::string& method,
                           NativeIpc::IpcCapability& capability) const override {
        if (method == "status" || method == "slow" ||
            method == "bad_json") {
            capability = NativeIpc::IpcCapability::Observe;
            return true;
        }
        if (method == "memory_write") {
            capability = NativeIpc::IpcCapability::TargetMutation;
            return true;
        }
        return false;
    }

    NativeIpc::IpcDispatchResult execute(
        const NativeIpc::IpcRequestDto& request,
        const NativeIpc::IpcRequestContext& context) override {
        ++dispatchCount_;
        if (request.method == "status") {
            NativeIpc::IpcDispatchResult result;
            result.ok = true;
            result.resultJson =
                json({{"method", request.method}, {"connected", true}})
                    .dump();
            return result;
        }
        if (request.method == "bad_json") {
            NativeIpc::IpcDispatchResult result;
            result.ok = true;
            result.resultJson = "{";
            return result;
        }

        ::SetEvent(started_);
        while (!context.cancellationRequested()) {
            ::Sleep(1);
        }
        const auto reason = context.cancellation->reason();
        cancelReason_.store(reason, std::memory_order_release);
        NativeIpc::IpcDispatchResult result;
        result.errorCode = reason == NativeIpc::RequestCancelReason::Deadline
                               ? "deadline_exceeded"
                               : "cancelled";
        result.errorMessage = "cooperative dispatcher observed cancellation";
        result.completion = NativeIpc::RequestCompletion::CancelRequested;
        return result;
    }

    bool waitStarted(DWORD timeout = 5000) const {
        return ::WaitForSingleObject(started_, timeout) == WAIT_OBJECT_0;
    }

    int dispatchCount() const {
        return dispatchCount_.load(std::memory_order_acquire);
    }

    NativeIpc::RequestCancelReason cancelReason() const {
        return cancelReason_.load(std::memory_order_acquire);
    }

private:
    HANDLE started_ = nullptr;
    std::atomic<int> dispatchCount_{0};
    std::atomic<NativeIpc::RequestCancelReason> cancelReason_{
        NativeIpc::RequestCancelReason::None};
};

struct SharedSessionResult {
    void set(const NativeIpc::RequestSessionResult& value) {
        std::lock_guard<std::mutex> lock(mutex);
        result = value;
        available = true;
    }

    NativeIpc::RequestSessionResult get() const {
        std::lock_guard<std::mutex> lock(mutex);
        expect(available, "session result should be available");
        return result;
    }

    mutable std::mutex mutex;
    NativeIpc::RequestSessionResult result;
    bool available = false;
};

class SessionHarness final {
public:
    SessionHarness(const wchar_t* suffix,
                   TestDispatcher& dispatcher,
                   NativeIpc::RequestSessionConfig config = {})
        : dispatcher_(dispatcher), config_(config),
          pipeName_(uniquePipeName(suffix)) {
        done_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        clientStop_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        expect(done_ != nullptr && clientStop_ != nullptr,
               "session events should be created");
    }

    ~SessionHarness() {
        closeClient();
        if (server_ != nullptr) {
            server_->stop();
        }
        ::CloseHandle(clientStop_);
        ::CloseHandle(done_);
    }

    void start() {
        server_ = std::make_unique<NativeIpc::NamedPipeServer>(
            pipeName_, [this](HANDLE pipe, HANDLE stopEvent) {
                NativeIpc::IpcFramedConnection connection(pipe, stopEvent);
                NativeIpc::IpcHandshakeSession handshake(connection);
                const auto handshakeResult = handshake.perform();
                if (handshakeResult.status ==
                    NativeIpc::HandshakeStatus::Established) {
                    NativeIpc::IpcRequestSession session(
                        connection, dispatcher_,
                        handshakeResult.grantedCapabilities, config_);
                    shared_.set(session.run());
                }
                ::SetEvent(done_);
            });
        std::wstring error;
        expect(server_->start(error), "request session server should start");
        client_ = connectPipe(server_->snapshot().pipeName);
        expect(client_ != INVALID_HANDLE_VALUE,
               "request session client should connect");
        connection_ = std::make_unique<NativeIpc::IpcFramedConnection>(
            client_, clientStop_);

        const std::string hello =
            json({{"client_name", "AMem.RequestTests"},
                  {"client_version", "1.0"},
                  {"requested_capabilities",
                   json::array({"Observe", "TargetMutation"})}})
                .dump();
        expect(connection_
                       ->writeFrame(
                           {IpcProtocol::MessageType::Hello, 0, hello},
                           std::chrono::steady_clock::now() + 5s,
                           NativeIpc::kMaxHandshakePayloadBytes)
                       .status == NativeIpc::FrameIoStatus::Complete,
               "Hello should be sent");
        const auto ack = connection_->readFrame(
            std::chrono::steady_clock::now() + 5s,
            NativeIpc::kMaxHandshakePayloadBytes);
        expect(ack.status == NativeIpc::FrameIoStatus::Complete &&
                   ack.frame.type == IpcProtocol::MessageType::HelloAck,
               "HelloAck should establish the request session");
        const json ackPayload = json::parse(ack.frame.payload);
        expect(ackPayload["granted_capabilities"] ==
                   json::array({"Observe"}) &&
                   ackPayload["denied_capabilities"] ==
                       json::array({"TargetMutation"}),
               "test session should be Observe-only");
    }

    void send(IpcProtocol::MessageType type,
              uint64_t requestId,
              const std::string& payload) {
        expect(connection_ != nullptr, "client connection should be open");
        expect(connection_
                       ->writeFrame({type, requestId, payload},
                                    std::chrono::steady_clock::now() + 5s)
                       .status == NativeIpc::FrameIoStatus::Complete,
               "client frame should be sent");
    }

    void request(uint64_t requestId,
                 const std::string& method,
                 const json& params = json::object(),
                 uint32_t timeoutMs = 5000) {
        send(IpcProtocol::MessageType::Request, requestId,
             json({{"method", method},
                   {"params", params},
                   {"timeout_ms", timeoutMs}})
                 .dump());
    }

    IpcProtocol::Frame read() {
        expect(connection_ != nullptr, "client connection should be open");
        const auto response = connection_->readFrame(
            std::chrono::steady_clock::now() + 5s);
        expect(response.status == NativeIpc::FrameIoStatus::Complete,
               "server response should be readable");
        return response.frame;
    }

    void closeClient() {
        connection_.reset();
        if (client_ != INVALID_HANDLE_VALUE) {
            ::CloseHandle(client_);
            client_ = INVALID_HANDLE_VALUE;
        }
    }

    void stopServer() {
        expect(server_ != nullptr, "server should exist");
        server_->stop();
    }

    NativeIpc::RequestSessionResult waitResult() const {
        expect(::WaitForSingleObject(done_, 5000) == WAIT_OBJECT_0,
               "request session should finish");
        return shared_.get();
    }

private:
    TestDispatcher& dispatcher_;
    NativeIpc::RequestSessionConfig config_;
    std::wstring pipeName_;
    HANDLE done_ = nullptr;
    HANDLE clientStop_ = nullptr;
    HANDLE client_ = INVALID_HANDLE_VALUE;
    std::unique_ptr<NativeIpc::IpcFramedConnection> connection_;
    std::unique_ptr<NativeIpc::NamedPipeServer> server_;
    SharedSessionResult shared_;
};

json readJson(const IpcProtocol::Frame& frame,
              IpcProtocol::MessageType type,
              uint64_t requestId) {
    expect(frame.type == type && frame.requestId == requestId,
           "response type/id should match the request");
    return json::parse(frame.payload);
}

void expectError(const IpcProtocol::Frame& frame,
                 uint64_t requestId,
                 const std::string& code) {
    const json payload =
        readJson(frame, IpcProtocol::MessageType::Error, requestId);
    expect(payload["code"] == code,
           "unexpected protocol error: " + payload.dump());
}

void testPersistentSequentialRequests() {
    TestDispatcher dispatcher;
    SessionHarness harness(L"persistent", dispatcher);
    harness.start();

    harness.request(1, "status");
    const json first = readJson(harness.read(),
                                IpcProtocol::MessageType::Response, 1);
    harness.request(2, "status");
    const json second = readJson(harness.read(),
                                 IpcProtocol::MessageType::Response, 2);
    expect(first["ok"] == true && second["ok"] == true &&
               first["result"]["connected"] == true,
           "one connection should serve sequential Observe requests");

    harness.closeClient();
    const auto result = harness.waitResult();
    expect(result.status == NativeIpc::RequestSessionStatus::Closed &&
               result.requestFrames == 2 &&
               result.dispatchedRequests == 2 &&
               result.responsesSent == 2 && dispatcher.dispatchCount() == 2,
           "persistent session counters should be exact");
}

void testSchemaCapabilityAndDuplicateEnforcement() {
    TestDispatcher dispatcher;
    SessionHarness harness(L"enforcement", dispatcher);
    harness.start();

    harness.send(IpcProtocol::MessageType::Request, 1,
                 json({{"method", "status"},
                       {"params", json::object()},
                       {"capability", "Observe"}})
                     .dump());
    expectError(harness.read(), 1, "invalid_request");
    harness.request(1, "status");
    expectError(harness.read(), 1, "duplicate_request_id");
    harness.request(2, "memory_write");
    expectError(harness.read(), 2, "capability_denied");
    harness.request(3, "not_registered");
    expectError(harness.read(), 3, "method_not_found");
    harness.send(IpcProtocol::MessageType::Cancel, 99,
                 "{\"reason\":\"stop\"}");
    expectError(harness.read(), 99, "invalid_cancel");
    harness.send(IpcProtocol::MessageType::Cancel, 99, "{}");
    expectError(harness.read(), 99, "request_not_active");
    harness.request(4, "status");
    expect(readJson(harness.read(), IpcProtocol::MessageType::Response, 4)
                   ["ok"] == true,
           "valid Observe request should still execute after rejections");

    harness.closeClient();
    const auto result = harness.waitResult();
    expect(result.requestFrames == 4 &&
               result.dispatchedRequests == 1 &&
               dispatcher.dispatchCount() == 1,
           "rejected and duplicate requests must not dispatch");
}

void testActiveCancelAndSessionReuse() {
    TestDispatcher dispatcher;
    SessionHarness harness(L"cancel", dispatcher);
    harness.start();
    harness.request(10, "slow");
    expect(dispatcher.waitStarted(), "slow request should begin dispatch");
    harness.send(IpcProtocol::MessageType::Cancel, 10, "{}");
    const json cancelled = readJson(
        harness.read(), IpcProtocol::MessageType::Response, 10);
    expect(cancelled["ok"] == false &&
               cancelled["completion"] == "cancel_requested" &&
               cancelled["error"]["code"] == "cancelled" &&
               dispatcher.cancelReason() ==
                   NativeIpc::RequestCancelReason::Client,
           "Cancel should reach the active dispatcher cooperatively");

    harness.request(11, "status");
    expect(readJson(harness.read(), IpcProtocol::MessageType::Response, 11)
                   ["ok"] == true,
           "session should remain usable after a cancelled request");
    harness.closeClient();
    const auto result = harness.waitResult();
    expect(result.cancellationsObserved == 1 &&
               result.dispatchedRequests == 2,
           "client cancellation should be counted once");
}

void testDeadlineRequestsCancellation() {
    TestDispatcher dispatcher;
    SessionHarness harness(L"deadline", dispatcher);
    harness.start();
    harness.request(20, "slow", json::object(), 50);
    expect(dispatcher.waitStarted(), "deadline request should begin");
    const json response = readJson(
        harness.read(), IpcProtocol::MessageType::Response, 20);
    expect(response["ok"] == false &&
               response["error"]["code"] == "deadline_exceeded" &&
               dispatcher.cancelReason() ==
                   NativeIpc::RequestCancelReason::Deadline,
           "absolute server deadline should signal cancellation");
    harness.closeClient();
    expect(harness.waitResult().cancellationsObserved == 1,
           "deadline cancellation should be observable");
}

void testBusyAndRequestCountLimit() {
    TestDispatcher dispatcher;
    NativeIpc::RequestSessionConfig config;
    config.maxRequestsPerSession = 2;
    SessionHarness harness(L"limits", dispatcher, config);
    harness.start();
    harness.request(30, "slow");
    expect(dispatcher.waitStarted(), "limited slow request should begin");
    harness.request(31, "status");
    expectError(harness.read(), 31, "session_busy");
    harness.send(IpcProtocol::MessageType::Cancel, 30, "{}");
    readJson(harness.read(), IpcProtocol::MessageType::Response, 30);
    harness.request(32, "status");
    expectError(harness.read(), 32, "session_request_limit");
    const auto result = harness.waitResult();
    expect(result.status ==
                   NativeIpc::RequestSessionStatus::RequestLimitReached &&
               result.requestFrames == 2 &&
               result.dispatchedRequests == 1 &&
               dispatcher.dispatchCount() == 1,
           "busy request ids count toward the bounded session lifetime");
}

void testInvalidDispatcherOutputIsNormalized() {
    TestDispatcher dispatcher;
    SessionHarness harness(L"bad-output", dispatcher);
    harness.start();
    harness.request(40, "bad_json");
    const json response = readJson(
        harness.read(), IpcProtocol::MessageType::Response, 40);
    expect(response["ok"] == false &&
               response["completion"] == "completion_unknown" &&
               response["error"]["code"] == "internal_response_error",
           "invalid dispatcher output must not escape the response boundary");
    harness.closeClient();
    harness.waitResult();
}

void testUnexpectedMessageAndIdleTimeout() {
    {
        TestDispatcher dispatcher;
        SessionHarness harness(L"unexpected", dispatcher);
        harness.start();
        harness.send(IpcProtocol::MessageType::Response, 50, "{}");
        expectError(harness.read(), 50, "unexpected_message");
        expect(harness.waitResult().status ==
                   NativeIpc::RequestSessionStatus::ProtocolError,
               "post-handshake response frames should close the session");
    }
    {
        TestDispatcher dispatcher;
        NativeIpc::RequestSessionConfig config;
        config.idleTimeout = 50ms;
        SessionHarness harness(L"idle", dispatcher, config);
        harness.start();
        expect(harness.waitResult().status ==
                   NativeIpc::RequestSessionStatus::IdleTimedOut,
               "silent established sessions should have an idle timeout");
    }
}

void testStopCancelsDispatcherAndJoins() {
    TestDispatcher dispatcher;
    SessionHarness harness(L"stop", dispatcher);
    harness.start();
    harness.request(60, "slow");
    expect(dispatcher.waitStarted(), "stop test request should begin");
    harness.stopServer();
    const auto result = harness.waitResult();
    expect(result.status == NativeIpc::RequestSessionStatus::Cancelled &&
               result.cancellationsObserved == 1 &&
               dispatcher.cancelReason() ==
                   NativeIpc::RequestCancelReason::SessionStopping,
           "server Stop should cancel dispatch and join the worker");
}

} // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"persistent sequential requests", &testPersistentSequentialRequests},
        {"schema capability and duplicate enforcement",
         &testSchemaCapabilityAndDuplicateEnforcement},
        {"active cancel and session reuse", &testActiveCancelAndSessionReuse},
        {"deadline cancellation", &testDeadlineRequestsCancellation},
        {"busy and request count limit", &testBusyAndRequestCountLimit},
        {"invalid dispatcher output normalization",
         &testInvalidDispatcherOutputIsNormalized},
        {"unexpected message and idle timeout",
         &testUnexpectedMessageAndIdleTimeout},
        {"Stop cancellation and join", &testStopCancelsDispatcherAndJoins},
    };

    int failures = 0;
    for (const auto& test : tests) {
        try {
            test.second();
            std::cout << "[PASS] " << test.first << '\n';
        } catch (const std::exception& exception) {
            ++failures;
            std::cerr << "[FAIL] " << test.first << ": "
                      << exception.what() << '\n';
        }
    }
    if (failures != 0) {
        std::cerr << failures << " test group(s) failed\n";
        return 1;
    }
    std::cout << tests.size() << " test groups passed\n";
    return 0;
}
