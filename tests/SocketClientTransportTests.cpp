#include "socket/DeviceSession.h"
#include "socket/client.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class ScriptedSocketOps final : public IWindowsSocketOps {
public:
    struct Step {
        enum class Kind {
            Transfer,
            Error,
            Eof,
        };

        Kind kind = Kind::Transfer;
        int count = 0;
        int error = 0;
        std::vector<char> data;
    };

    struct Endpoint {
        bool connected = false;
        bool closed = false;
        unsigned closeCalls = 0;
        std::deque<Step> sendSteps;
        std::deque<Step> receiveSteps;
        std::vector<char> sent;
        std::vector<int> sendRequestLengths;
        std::vector<int> receiveRequestLengths;
    };

    SOCKET CreateTcpSocket() override {
        const SOCKET value = nextSocket_++;
        endpoints_.emplace(value, Endpoint{});
        lastCreated_ = value;
        return value;
    }

    int Connect(SOCKET socketValue, const sockaddr*, int) override {
        Endpoint& value = endpoint(socketValue);
        value.connected = true;
        return 0;
    }

    int Send(SOCKET socketValue, const char* buffer, int length,
             int) override {
        Endpoint& value = endpoint(socketValue);
        value.sendRequestLengths.push_back(length);
        if (value.sendSteps.empty()) {
            lastError_ = WSAEINVAL;
            return SOCKET_ERROR;
        }

        Step step = std::move(value.sendSteps.front());
        value.sendSteps.pop_front();
        if (step.kind == Step::Kind::Error) {
            lastError_ = step.error;
            return SOCKET_ERROR;
        }
        if (step.kind == Step::Kind::Eof) {
            return 0;
        }

        const int transferred = (std::min)(step.count, length);
        value.sent.insert(value.sent.end(), buffer, buffer + transferred);
        return transferred;
    }

    int Receive(SOCKET socketValue, char* buffer, int length,
                int) override {
        Endpoint& value = endpoint(socketValue);
        value.receiveRequestLengths.push_back(length);
        if (value.receiveSteps.empty()) {
            lastError_ = WSAEINVAL;
            return SOCKET_ERROR;
        }

        Step step = std::move(value.receiveSteps.front());
        value.receiveSteps.pop_front();
        if (step.kind == Step::Kind::Error) {
            lastError_ = step.error;
            return SOCKET_ERROR;
        }
        if (step.kind == Step::Kind::Eof) {
            return 0;
        }

        const int transferred = (std::min)(
            length, static_cast<int>(step.data.size()));
        std::memcpy(buffer, step.data.data(),
                    static_cast<size_t>(transferred));
        return transferred;
    }

    int Close(SOCKET socketValue) override {
        Endpoint& value = endpoint(socketValue);
        value.connected = false;
        value.closed = true;
        ++value.closeCalls;
        return 0;
    }

    int LastError() const override {
        return lastError_;
    }

    SOCKET lastCreated() const {
        return lastCreated_;
    }

    Endpoint& endpoint(SOCKET socketValue) {
        const auto found = endpoints_.find(socketValue);
        if (found == endpoints_.end()) {
            throw std::runtime_error("unknown scripted socket");
        }
        return found->second;
    }

    const Endpoint& endpoint(SOCKET socketValue) const {
        const auto found = endpoints_.find(socketValue);
        if (found == endpoints_.end()) {
            throw std::runtime_error("unknown scripted socket");
        }
        return found->second;
    }

    void queueSendCount(SOCKET socketValue, int count) {
        endpoint(socketValue).sendSteps.push_back(
            Step{Step::Kind::Transfer, count, 0, {}});
    }

    void queueSendError(SOCKET socketValue, int error) {
        endpoint(socketValue).sendSteps.push_back(
            Step{Step::Kind::Error, 0, error, {}});
    }

    void queueReceive(SOCKET socketValue, std::string data) {
        endpoint(socketValue).receiveSteps.push_back(
            Step{Step::Kind::Transfer, 0, 0,
                 std::vector<char>(data.begin(), data.end())});
    }

    void queueReceiveError(SOCKET socketValue, int error) {
        endpoint(socketValue).receiveSteps.push_back(
            Step{Step::Kind::Error, 0, error, {}});
    }

    void queueReceiveEof(SOCKET socketValue) {
        endpoint(socketValue).receiveSteps.push_back(
            Step{Step::Kind::Eof, 0, 0, {}});
    }

private:
    std::unordered_map<SOCKET, Endpoint> endpoints_;
    SOCKET nextSocket_ = 100;
    SOCKET lastCreated_ = INVALID_SOCKET;
    int lastError_ = 0;
};

void connectSession(DeviceSession& session) {
    auto lifecycle = session.AcquireLifecycle();
    session.Disconnect();
    session.BeginConnect();
    session.FinishConnect(true);
}

void disconnectSession(DeviceSession& session) {
    auto lifecycle = session.AcquireLifecycle();
    session.Disconnect();
}

bool receiveExact(SOCKET socketValue, char* buffer, int length) {
    int receivedTotal = 0;
    while (receivedTotal < length) {
        const int received = ::recv(socketValue, buffer + receivedTotal,
                                    length - receivedTotal, 0);
        if (received <= 0) {
            return false;
        }
        receivedTotal += received;
    }
    return true;
}

bool sendAll(SOCKET socketValue, const char* buffer, int length) {
    int sentTotal = 0;
    while (sentTotal < length) {
        const int sent = ::send(socketValue, buffer + sentTotal,
                                length - sentTotal, 0);
        if (sent <= 0) {
            return false;
        }
        sentTotal += sent;
    }
    return true;
}

void testSystemOpsLoopbackRoundTrip() {
    WindowsSocketClient client;
    unsigned poisonCalls = 0;
    client.SetPoisonCallback([&] { ++poisonCalls; });

    SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    expect(listener != INVALID_SOCKET,
           "loopback listener socket should be created");

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    expect(::bind(listener, reinterpret_cast<sockaddr*>(&address),
                  sizeof(address)) != SOCKET_ERROR &&
               ::listen(listener, 1) != SOCKET_ERROR,
           "loopback listener should bind and listen");

    int addressLength = sizeof(address);
    expect(::getsockname(listener, reinterpret_cast<sockaddr*>(&address),
                         &addressLength) != SOCKET_ERROR,
           "loopback listener should expose its assigned port");
    const uint16_t port = ntohs(address.sin_port);

    std::string serverError;
    std::thread server([&] {
        SOCKET accepted = ::accept(listener, nullptr, nullptr);
        if (accepted == INVALID_SOCKET) {
            serverError = "loopback accept failed";
            return;
        }

        std::array<char, 4> request{};
        if (!receiveExact(accepted, request.data(),
                          static_cast<int>(request.size())) ||
            std::string(request.data(), request.size()) != "PING") {
            serverError = "loopback server received an invalid request";
        } else {
            const std::array<char, 4> response = {'P', 'O', 'N', 'G'};
            if (!sendAll(accepted, response.data(),
                         static_cast<int>(response.size()))) {
                serverError = "loopback server failed to send its response";
            }
        }
        ::closesocket(accepted);
    });

    const bool connected = client.Connect("127.0.0.1", port);
    bool sent = false;
    bool received = false;
    std::array<char, 4> response{};
    if (connected) {
        const std::array<char, 4> request = {'P', 'I', 'N', 'G'};
        sent = client.Send(request.data(), request.size());
        if (sent) {
            received = client.Receive(response.data(), response.size());
        }
    } else {
        ::closesocket(listener);
        listener = INVALID_SOCKET;
    }

    server.join();
    if (listener != INVALID_SOCKET) {
        ::closesocket(listener);
    }
    client.Close();

    expect(connected && sent && received && serverError.empty() &&
               std::string(response.data(), response.size()) == "PONG" &&
               poisonCalls == 0,
           serverError.empty()
               ? "default system socket operations should complete a loopback round trip"
               : serverError);
}

void testPartialSendLoopsAndFailurePoisons() {
    ScriptedSocketOps ops;
    WindowsSocketClient client(ops);
    unsigned poisonCalls = 0;
    client.SetPoisonCallback([&] { ++poisonCalls; });
    expect(client.Connect("127.0.0.1", 28101),
           "scripted client should connect");
    const SOCKET socketValue = ops.lastCreated();

    ops.queueSendCount(socketValue, 2);
    ops.queueSendCount(socketValue, 3);
    const std::array<char, 5> first = {'A', 'B', 'C', 'D', 'E'};
    expect(client.Send(first.data(), first.size()),
           "partial sends should continue until the payload is complete");
    expect(ops.endpoint(socketValue).sent ==
               std::vector<char>(first.begin(), first.end()) &&
               ops.endpoint(socketValue).sendRequestLengths ==
                   std::vector<int>({5, 3}) &&
               poisonCalls == 0 && client.IsConnected(),
           "successful partial sends must preserve offsets and connection state");

    ops.queueSendCount(socketValue, 1);
    ops.queueSendError(socketValue, WSAETIMEDOUT);
    const std::array<char, 3> second = {'X', 'Y', 'Z'};
    expect(!client.Send(second.data(), second.size()),
           "an error after a partial send should fail the request");
    expect(poisonCalls == 1 && !client.IsConnected() &&
               ops.endpoint(socketValue).closed &&
               ops.endpoint(socketValue).closeCalls == 1 &&
               ops.endpoint(socketValue).sent.back() == 'X',
           "partial send failure must poison and close the stream exactly once");
}

void testPartialReceiveEofPoisons() {
    ScriptedSocketOps ops;
    WindowsSocketClient client(ops);
    unsigned poisonCalls = 0;
    client.SetPoisonCallback([&] { ++poisonCalls; });
    expect(client.Connect("127.0.0.1", 28102),
           "scripted client should connect");
    const SOCKET socketValue = ops.lastCreated();
    ops.queueReceive(socketValue, "AB");
    ops.queueReceiveEof(socketValue);

    std::array<char, 4> buffer{};
    expect(!client.Receive(buffer.data(), buffer.size()),
           "EOF after a partial receive should fail the request");
    expect(buffer[0] == 'A' && buffer[1] == 'B' &&
               ops.endpoint(socketValue).receiveRequestLengths ==
                   std::vector<int>({4, 2}) &&
               poisonCalls == 1 && !client.IsConnected() &&
               ops.endpoint(socketValue).closed,
           "partial EOF must preserve offsets, poison, and close the stream");
}

void testLateBytesCannotCrossReconnectGeneration() {
    auto& session = DeviceSession::GetInstance();
    connectSession(session);

    ScriptedSocketOps ops;
    WindowsSocketClient client(ops);
    client.SetPoisonCallback([&] { session.MarkPoisoned(); });
    expect(client.Connect("127.0.0.1", 28103),
           "first scripted generation should connect");
    const SOCKET oldSocket = ops.lastCreated();
    ops.queueReceive(oldSocket, "OL");
    ops.queueReceiveError(oldSocket, WSAETIMEDOUT);

    const uint64_t firstGeneration = session.GetGeneration();
    {
        auto request = session.AcquireRequest();
        expect(request && request.generation() == firstGeneration,
               "first request should bind the connected generation");
        std::array<char, 4> staleBuffer{};
        expect(!client.Receive(staleBuffer.data(), staleBuffer.size()) &&
                   staleBuffer[0] == 'O' && staleBuffer[1] == 'L',
               "scripted timeout should occur after a partial old response");
        expect(session.IsPoisoned() && !request.isCurrent() &&
                   session.GetGeneration() == firstGeneration + 1 &&
                   ops.endpoint(oldSocket).closed,
               "timeout must poison the session, advance generation, and close old I/O");
    }

    ops.queueReceive(oldSocket, "D!");
    {
        auto lifecycle = session.AcquireLifecycle();
        session.BeginConnect();
        expect(client.Connect("127.0.0.1", 28103),
               "explicit reconnect should create a fresh socket");
        session.FinishConnect(true);
    }

    const SOCKET newSocket = ops.lastCreated();
    expect(newSocket != oldSocket &&
               ops.endpoint(oldSocket).receiveSteps.size() == 1,
           "late old bytes must remain attached to the closed endpoint");
    ops.queueReceive(newSocket, "NEW!");

    {
        auto request = session.AcquireRequest();
        expect(request && request.isCurrent() &&
                   request.generation() == firstGeneration + 2,
               "reconnect should issue a fresh current generation");
        std::array<char, 4> buffer{};
        expect(client.Receive(buffer.data(), buffer.size()) &&
                   std::string(buffer.data(), buffer.size()) == "NEW!",
               "new generation must receive only its own endpoint bytes");
    }

    client.Close();
    disconnectSession(session);
}

} // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"system Winsock loopback round trip", &testSystemOpsLoopbackRoundTrip},
        {"partial send and timeout poisoning",
         &testPartialSendLoopsAndFailurePoisons},
        {"partial receive EOF poisoning", &testPartialReceiveEofPoisons},
        {"late bytes isolated by reconnect generation",
         &testLateBytesCannotCrossReconnectGeneration},
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
