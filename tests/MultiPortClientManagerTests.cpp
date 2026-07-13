#include "socket/MultiPortClientManager.h"
#include "socket/ServerHandshake.h"
#include "tests/ScriptedSocketOps.h"

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using TestSupport::ScriptedSocketOps;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void resetSession(DeviceSession& session) {
    auto lifecycle = session.AcquireLifecycle();
    session.Disconnect();
}

bool allConnected(MultiPortClientManager& manager) {
    return manager.GetClient(ManagedSocketPort::Main)->IsConnected() &&
           manager.GetClient(ManagedSocketPort::Debug)->IsConnected() &&
           manager.GetClient(ManagedSocketPort::Error)->IsConnected();
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

bool sendServerVersion(SOCKET socketValue, const std::string& identity) {
    if (identity.size() > 255) {
        return false;
    }
    const CeVersion version{
        1, static_cast<unsigned char>(identity.size())};
    return sendAll(socketValue,
                   reinterpret_cast<const char*>(&version),
                   static_cast<int>(sizeof(version))) &&
           sendAll(socketValue, identity.data(),
                   static_cast<int>(identity.size()));
}

void testSystemOpsThreePortLoopback() {
    auto& session = DeviceSession::GetInstance();
    resetSession(session);
    unsigned resetCalls = 0;
    auto& systemOps = GetSystemWindowsSocketOps();
    MultiPortClientManager manager(
        session, systemOps, systemOps, systemOps,
        [&] { ++resetCalls; }, &AmemServerHandshake::Validate);

    SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    expect(listener != INVALID_SOCKET,
           "three-port loopback listener should be created");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    expect(::bind(listener, reinterpret_cast<sockaddr*>(&address),
                  sizeof(address)) != SOCKET_ERROR &&
               ::listen(listener, 3) != SOCKET_ERROR,
           "three-port loopback listener should bind and listen");
    int addressLength = sizeof(address);
    expect(::getsockname(listener, reinterpret_cast<sockaddr*>(&address),
                         &addressLength) != SOCKET_ERROR,
           "three-port loopback listener should expose its port");
    const uint16_t port = ntohs(address.sin_port);

    std::string serverError;
    std::thread server([&] {
        std::vector<SOCKET> accepted;
        for (size_t index = 0; index < 3; ++index) {
            SOCKET client = ::accept(listener, nullptr, nullptr);
            if (client == INVALID_SOCKET) {
                serverError = "three-port loopback accept failed";
                break;
            }
            const DWORD timeoutMs = 5000;
            (void)::setsockopt(
                client, SOL_SOCKET, SO_RCVTIMEO,
                reinterpret_cast<const char*>(&timeoutMs),
                sizeof(timeoutMs));
            accepted.push_back(client);
        }

        if (accepted.size() == 3) {
            unsigned char command = 0;
            if (!receiveExact(accepted[0],
                              reinterpret_cast<char*>(&command),
                              sizeof(command)) ||
                command != CMD_GETVERSION ||
                !sendServerVersion(accepted[0], "CHEATENGINE v2.0")) {
                serverError = "three-port compatibility handshake failed";
            }
            for (SOCKET client : accepted) {
                char value = 0;
                if (::recv(client, &value, sizeof(value), 0) != 0 &&
                    serverError.empty()) {
                    serverError =
                        "three-port disconnect should close every TCP stream";
                }
            }
        }
        for (SOCKET client : accepted) {
            ::closesocket(client);
        }
    });

    const MultiPortConnectFailure failure =
        manager.Connect("127.0.0.1", port);
    bool disconnected = false;
    if (failure == MultiPortConnectFailure::None) {
        disconnected = manager.Disconnect();
    } else {
        ::closesocket(listener);
        listener = INVALID_SOCKET;
    }
    server.join();
    if (listener != INVALID_SOCKET) {
        ::closesocket(listener);
    }

    expect(failure == MultiPortConnectFailure::None && disconnected &&
               !manager.IsConnected() && !allConnected(manager) &&
               resetCalls == 2 && serverError.empty(),
           serverError.empty()
               ? "system Winsock manager should connect and close all three loopback streams"
               : serverError);
}

void runRejectedCompatibilityHandshake(const char* identity,
                                       bool expectTimeout) {
    auto& session = DeviceSession::GetInstance();
    resetSession(session);
    unsigned resetCalls = 0;
    auto& systemOps = GetSystemWindowsSocketOps();
    MultiPortClientManager manager(
        session, systemOps, systemOps, systemOps,
        [&] { ++resetCalls; }, &AmemServerHandshake::Validate);

    SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    expect(listener != INVALID_SOCKET,
           "compatibility test listener should be created");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    expect(::bind(listener, reinterpret_cast<sockaddr*>(&address),
                  sizeof(address)) != SOCKET_ERROR &&
               ::listen(listener, 3) != SOCKET_ERROR,
           "compatibility test listener should bind and listen");
    int addressLength = sizeof(address);
    expect(::getsockname(listener, reinterpret_cast<sockaddr*>(&address),
                         &addressLength) != SOCKET_ERROR,
           "compatibility test listener should expose its port");
    const uint16_t port = ntohs(address.sin_port);

    std::string serverError;
    std::thread server([&] {
        std::vector<SOCKET> accepted;
        for (size_t index = 0; index < 3; ++index) {
            SOCKET client = ::accept(listener, nullptr, nullptr);
            if (client == INVALID_SOCKET) {
                serverError = "compatibility test accept failed";
                break;
            }
            accepted.push_back(client);
        }

        if (accepted.size() == 3) {
            unsigned char command = 0;
            if (!receiveExact(accepted[0],
                              reinterpret_cast<char*>(&command),
                              sizeof(command)) ||
                command != CMD_GETVERSION) {
                serverError = "compatibility test received an invalid command";
            } else if (identity &&
                       !sendServerVersion(accepted[0], identity)) {
                serverError = "compatibility test failed to send version";
            }

            for (SOCKET client : accepted) {
                char value = 0;
                if (::recv(client, &value, sizeof(value), 0) != 0 &&
                    serverError.empty()) {
                    serverError =
                        "rejected compatibility handshake should close every stream";
                }
            }
        }
        for (SOCKET client : accepted) {
            ::closesocket(client);
        }
    });

    const auto start = std::chrono::steady_clock::now();
    const MultiPortConnectFailure failure = manager.Connect("127.0.0.1", port);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    server.join();
    ::closesocket(listener);

    expect(failure == MultiPortConnectFailure::Compatibility &&
               !manager.IsConnected() && !allConnected(manager) &&
               session.GetState() == DeviceSession::State::Disconnected &&
               resetCalls == 1 && serverError.empty(),
           serverError.empty()
               ? "rejected server identity should roll back all three ports"
               : serverError);
    if (expectTimeout) {
        expect(elapsed >= std::chrono::seconds(4) &&
                   elapsed < std::chrono::seconds(15),
               "silent handshake should fail at the default bounded I/O timeout");
    }
}

void testIncompatibleServerIsRejected() {
    runRejectedCompatibilityHandshake("MiniMem 1.0.0", false);
}

void testSilentServerTimesOutAndRollsBack() {
    runRejectedCompatibilityHandshake(nullptr, true);
}

void testConnectAndDisconnectAllPorts() {
    auto& session = DeviceSession::GetInstance();
    resetSession(session);
    const uint64_t initialGeneration = session.GetGeneration();
    unsigned resetCalls = 0;
    ScriptedSocketOps mainOps;
    ScriptedSocketOps debugOps;
    ScriptedSocketOps errorOps;

    MultiPortClientManager manager(
        session, mainOps, debugOps, errorOps,
        [&] { ++resetCalls; });
    expect(manager.Connect("127.0.0.1", 28110) ==
               MultiPortConnectFailure::None &&
               manager.IsConnected() && allConnected(manager),
           "all three clients should connect under one lifecycle transition");
    expect(resetCalls == 1 &&
               session.GetGeneration() == initialGeneration + 1 &&
               mainOps.createdCount() == 1 &&
               debugOps.createdCount() == 1 &&
               errorOps.createdCount() == 1,
           "connect should reset target state once and create each endpoint once");

    const SOCKET mainSocket = mainOps.lastCreated();
    const SOCKET debugSocket = debugOps.lastCreated();
    const SOCKET errorSocket = errorOps.lastCreated();
    expect(manager.Disconnect() && !manager.IsConnected() &&
               !allConnected(manager),
           "disconnect should close all clients and transition the session");
    expect(resetCalls == 2 &&
               session.GetGeneration() == initialGeneration + 2 &&
               mainOps.endpoint(mainSocket).closed &&
               debugOps.endpoint(debugSocket).closed &&
               errorOps.endpoint(errorSocket).closed,
           "disconnect should reset target state and close every old endpoint");
}

void testConnectionFailureRollsBackEarlierPorts() {
    const std::array<MultiPortConnectFailure, 3> failures = {
        MultiPortConnectFailure::Main,
        MultiPortConnectFailure::Debug,
        MultiPortConnectFailure::Error,
    };

    for (const MultiPortConnectFailure expectedFailure : failures) {
        auto& session = DeviceSession::GetInstance();
        resetSession(session);
        unsigned resetCalls = 0;
        ScriptedSocketOps mainOps;
        ScriptedSocketOps debugOps;
        ScriptedSocketOps errorOps;
        if (expectedFailure == MultiPortConnectFailure::Main) {
            mainOps.failNextConnect(WSAECONNREFUSED);
        } else if (expectedFailure == MultiPortConnectFailure::Debug) {
            debugOps.failNextConnect(WSAECONNREFUSED);
        } else {
            errorOps.failNextConnect(WSAECONNREFUSED);
        }

        MultiPortClientManager manager(
            session, mainOps, debugOps, errorOps,
            [&] { ++resetCalls; });
        expect(manager.Connect("127.0.0.1", 28111) == expectedFailure &&
                   !manager.IsConnected() && !allConnected(manager),
               "a failed port should fail and roll back the whole connection");
        expect(resetCalls == 1 &&
                   mainOps.createdCount() >= 1 &&
                   (expectedFailure == MultiPortConnectFailure::Main ||
                    debugOps.createdCount() >= 1) &&
                   (expectedFailure != MultiPortConnectFailure::Error ||
                    errorOps.createdCount() == 1),
               "connect should stop at the failing port after one state reset");
        if (mainOps.createdCount() != 0) {
            expect(mainOps.endpoint(mainOps.lastCreated()).closed,
                   "failed connect should close the MAIN endpoint");
        }
        if (debugOps.createdCount() != 0) {
            expect(debugOps.endpoint(debugOps.lastCreated()).closed,
                   "failed connect should close the DEBUG endpoint");
        }
        if (errorOps.createdCount() != 0) {
            expect(errorOps.endpoint(errorOps.lastCreated()).closed,
                   "failed connect should close the ERROR endpoint");
        }

        expect(manager.Connect("127.0.0.1", 28111) ==
                   MultiPortConnectFailure::None &&
                   allConnected(manager),
               "a later explicit connect should recover from failed setup");
        manager.Disconnect();
    }
}

void testSinglePortPoisonRequiresFullReconnect() {
    auto& session = DeviceSession::GetInstance();
    resetSession(session);
    unsigned resetCalls = 0;
    ScriptedSocketOps mainOps;
    ScriptedSocketOps debugOps;
    ScriptedSocketOps errorOps;
    MultiPortClientManager manager(
        session, mainOps, debugOps, errorOps,
        [&] { ++resetCalls; });
    expect(manager.Connect("127.0.0.1", 28112) ==
               MultiPortConnectFailure::None,
           "initial three-port connection should succeed");

    const uint64_t connectedGeneration = session.GetGeneration();
    const SOCKET oldMain = mainOps.lastCreated();
    const SOCKET oldDebug = debugOps.lastCreated();
    const SOCKET oldError = errorOps.lastCreated();
    debugOps.queueReceiveError(oldDebug, WSAETIMEDOUT);
    {
        auto request = session.AcquireRequest();
        expect(request && request.isCurrent(),
               "connected manager should grant a request lease");
        char value = 0;
        expect(!manager.GetClient(ManagedSocketPort::Debug)->Receive(
                   &value, sizeof(value)),
               "a scripted DEBUG timeout should fail its I/O");
        expect(session.IsPoisoned() && !request.isCurrent() &&
                   session.GetGeneration() == connectedGeneration + 1 &&
                   !debugOps.endpoint(oldDebug).connected &&
                   mainOps.endpoint(oldMain).connected &&
                   errorOps.endpoint(oldError).connected,
               "single-port failure should poison the session without pretending other sockets closed");
    }
    expect(!session.AcquireRequest(),
           "poisoned multi-port session must reject every new request");

    expect(manager.Connect("127.0.0.1", 28112) ==
               MultiPortConnectFailure::None &&
               session.GetGeneration() == connectedGeneration + 2 &&
               allConnected(manager),
           "explicit reconnect should replace all ports with a fresh generation");
    expect(mainOps.endpoint(oldMain).closed &&
               debugOps.endpoint(oldDebug).closed &&
               errorOps.endpoint(oldError).closed && resetCalls == 2 &&
               mainOps.createdCount() == 2 &&
               debugOps.createdCount() == 2 &&
               errorOps.createdCount() == 2,
           "reconnect should close every endpoint from the poisoned generation");
    manager.Disconnect();
}

void testDisconnectWaitsForActiveRequest() {
    auto& session = DeviceSession::GetInstance();
    resetSession(session);
    ScriptedSocketOps mainOps;
    ScriptedSocketOps debugOps;
    ScriptedSocketOps errorOps;
    MultiPortClientManager manager(
        session, mainOps, debugOps, errorOps, [] {});
    expect(manager.Connect("127.0.0.1", 28113) ==
               MultiPortConnectFailure::None,
           "manager should connect before lifecycle blocking test");

    std::atomic<bool> started{false};
    std::atomic<bool> finished{false};
    std::thread disconnectThread;
    {
        auto request = session.AcquireRequest();
        expect(static_cast<bool>(request),
               "active request should hold the shared lifecycle lease");
        disconnectThread = std::thread([&] {
            started.store(true, std::memory_order_release);
            manager.Disconnect();
            finished.store(true, std::memory_order_release);
        });
        while (!started.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        expect(!finished.load(std::memory_order_acquire) &&
                   manager.IsConnected() && allConnected(manager),
               "disconnect must wait while an active request owns the shared lease");
    }
    disconnectThread.join();
    expect(finished.load(std::memory_order_acquire) &&
               !manager.IsConnected() && !allConnected(manager),
           "disconnect should close all ports after the request lease releases");
}

void testRepeatedLifecycleDoesNotReuseEndpoints() {
    auto& session = DeviceSession::GetInstance();
    resetSession(session);
    const uint64_t initialGeneration = session.GetGeneration();
    unsigned resetCalls = 0;
    ScriptedSocketOps mainOps;
    ScriptedSocketOps debugOps;
    ScriptedSocketOps errorOps;
    MultiPortClientManager manager(
        session, mainOps, debugOps, errorOps,
        [&] { ++resetCalls; });

    constexpr size_t kIterations = 50;
    for (size_t index = 0; index < kIterations; ++index) {
        expect(manager.Connect("127.0.0.1", 28114) ==
                   MultiPortConnectFailure::None &&
                   allConnected(manager),
               "repeated connect should create a complete generation");
        expect(manager.Disconnect(),
               "repeated disconnect should observe an active session");
    }

    expect(mainOps.createdCount() == kIterations &&
               debugOps.createdCount() == kIterations &&
               errorOps.createdCount() == kIterations &&
               resetCalls == kIterations * 2 &&
               session.GetGeneration() ==
                   initialGeneration + kIterations * 2,
           "each repeated lifecycle should use fresh endpoints and generation values");
}

} // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"system Winsock three-port loopback", &testSystemOpsThreePortLoopback},
        {"incompatible server handshake rejection",
         &testIncompatibleServerIsRejected},
        {"silent server bounded handshake timeout",
         &testSilentServerTimesOutAndRollsBack},
        {"connect and disconnect all ports", &testConnectAndDisconnectAllPorts},
        {"connection failure rollback", &testConnectionFailureRollsBackEarlierPorts},
        {"single-port poison and full reconnect", &testSinglePortPoisonRequiresFullReconnect},
        {"disconnect waits for active request", &testDisconnectWaitsForActiveRequest},
        {"repeated lifecycle endpoint isolation", &testRepeatedLifecycleDoesNotReuseEndpoints},
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
