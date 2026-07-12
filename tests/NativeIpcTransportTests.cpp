#include "ipc/NamedPipeServer.h"
#include "ipc/NativePipeSecurity.h"

#include <windows.h>

#include <Aclapi.h>

#include <atomic>
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

std::vector<unsigned char> currentUserSid() {
    HANDLE token = nullptr;
    expect(::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token) !=
               FALSE,
           "OpenProcessToken should succeed");
    DWORD required = 0;
    ::GetTokenInformation(token, TokenUser, nullptr, 0, &required);
    expect(required != 0 && ::GetLastError() == ERROR_INSUFFICIENT_BUFFER,
           "TokenUser size should be available");
    std::vector<unsigned char> buffer(required);
    expect(::GetTokenInformation(token, TokenUser, buffer.data(), required,
                                 &required) != FALSE,
           "TokenUser should be readable");
    ::CloseHandle(token);
    return buffer;
}

std::wstring uniquePipeName(const wchar_t* suffix) {
    return std::wstring(L"\\\\.\\pipe\\AMem.NativeAgent.Test.") +
           std::to_wstring(::GetCurrentProcessId()) + L"." + suffix;
}

HANDLE connectPipe(const std::wstring& pipeName) {
    const ULONGLONG deadline = ::GetTickCount64() + 5000;
    do {
        HANDLE client = ::CreateFileW(pipeName.c_str(),
                                      GENERIC_READ | GENERIC_WRITE, 0,
                                      nullptr, OPEN_EXISTING, 0, nullptr);
        if (client != INVALID_HANDLE_VALUE) {
            return client;
        }
        if (::GetLastError() != ERROR_PIPE_BUSY) {
            break;
        }
        ::WaitNamedPipeW(pipeName.c_str(), 100);
    } while (::GetTickCount64() < deadline);
    return INVALID_HANDLE_VALUE;
}

void testSecurityDescriptorContainsOnlyUserAndSystem() {
    NativeIpc::NativePipeSecurity security;
    std::wstring error;
    expect(security.initialize(error),
           "pipe security should initialize");

    BOOL daclPresent = FALSE;
    BOOL daclDefaulted = FALSE;
    PACL dacl = nullptr;
    expect(::GetSecurityDescriptorDacl(security.descriptor(), &daclPresent,
                                       &dacl, &daclDefaulted) != FALSE &&
               daclPresent && dacl != nullptr && !daclDefaulted,
           "security descriptor should contain an explicit DACL");

    ACL_SIZE_INFORMATION aclInfo{};
    expect(::GetAclInformation(dacl, &aclInfo, sizeof(aclInfo),
                               AclSizeInformation) != FALSE,
           "DACL metadata should be readable");
    expect(aclInfo.AceCount == 2,
           "DACL should contain exactly user and SYSTEM ACEs");

    const auto userBuffer = currentUserSid();
    const auto* tokenUser =
        reinterpret_cast<const TOKEN_USER*>(userBuffer.data());
    unsigned char systemBuffer[SECURITY_MAX_SID_SIZE]{};
    DWORD systemSize = sizeof(systemBuffer);
    expect(::CreateWellKnownSid(WinLocalSystemSid, nullptr, systemBuffer,
                                &systemSize) != FALSE,
           "SYSTEM SID should be constructible");

    bool foundUser = false;
    bool foundSystem = false;
    for (DWORD index = 0; index < aclInfo.AceCount; ++index) {
        void* rawAce = nullptr;
        expect(::GetAce(dacl, index, &rawAce) != FALSE,
               "DACL ACE should be readable");
        const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(rawAce);
        expect(ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE,
               "DACL should contain allow ACEs only");
        expect((ace->Mask & (GENERIC_READ | GENERIC_WRITE)) ==
                   (GENERIC_READ | GENERIC_WRITE) &&
                   (ace->Mask & (WRITE_DAC | WRITE_OWNER)) == 0,
               "DACL should grant pipe read/write without ownership rights");
        PSID sid = const_cast<DWORD*>(&ace->SidStart);
        if (::EqualSid(sid, tokenUser->User.Sid)) {
            foundUser = true;
        } else if (::EqualSid(sid, systemBuffer)) {
            foundSystem = true;
        } else {
            expect(false, "DACL must not grant an unexpected SID");
        }
    }
    expect(foundUser && foundSystem,
           "DACL should grant current user and SYSTEM");
}

void testPipeConfigurationAndNameValidation() {
    expect((NativeIpc::kPipeMode & PIPE_REJECT_REMOTE_CLIENTS) != 0,
           "pipe mode must reject remote clients");
    expect((NativeIpc::kPipeOpenMode & FILE_FLAG_FIRST_PIPE_INSTANCE) != 0,
           "pipe should prevent a second server from claiming its name");
    expect(NativeIpc::IsValidLocalPipeName(NativeIpc::kDefaultPipeName),
           "product pipe name should be a local pipe");
    expect(!NativeIpc::IsValidLocalPipeName(
               L"\\\\remote-host\\pipe\\AMem.NativeAgent.v1"),
           "remote pipe paths must be rejected");
    expect(!NativeIpc::IsValidLocalPipeName(L"\\\\.\\pipe\\"),
           "empty local pipe names must be rejected");
    expect(!NativeIpc::IsValidLocalPipeName(
               L"\\\\.\\pipe\\nested\\AMem.NativeAgent.v1"),
           "nested pipe names must be rejected");
}

void testStartConnectStopAndStatus() {
    HANDLE handlerStarted = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    expect(handlerStarted != nullptr,
           "handler synchronization event should be created");

    NativeIpc::NamedPipeServer server(
        uniquePipeName(L"lifecycle"),
        [handlerStarted](HANDLE, HANDLE stopEvent) {
            ::SetEvent(handlerStarted);
            ::WaitForSingleObject(stopEvent, INFINITE);
        });
    std::wstring error;
    expect(server.start(error), "Named Pipe server should start");
    expect(server.start(error), "starting an active server is idempotent");
    expect(server.snapshot().state == NativeIpc::ServerState::Listening,
           "server should report listening before a connection");

    HANDLE client = connectPipe(server.snapshot().pipeName);
    expect(client != INVALID_HANDLE_VALUE,
           "current user should connect through the pipe DACL");
    expect(::WaitForSingleObject(handlerStarted, 5000) == WAIT_OBJECT_0,
           "server should run the client handler");

    const auto connected = server.snapshot();
    expect(connected.state == NativeIpc::ServerState::Connected &&
               connected.acceptedConnections == 1 &&
               connected.lastError.empty(),
           "server status should expose the active connection");

    server.stop();
    const auto stopped = server.snapshot();
    expect(stopped.state == NativeIpc::ServerState::Stopped &&
               stopped.acceptedConnections == 1,
           "stop should join the handler and preserve counters");
    server.stop();

    ::CloseHandle(client);
    ::CloseHandle(handlerStarted);
}

void testSerialInstancesRetainPipeOwnership() {
    HANDLE handled = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    expect(handled != nullptr,
           "connection event should be created");
    std::atomic<unsigned> handlerCalls{0};
    NativeIpc::NamedPipeServer server(
        uniquePipeName(L"rollover"),
        [&handlerCalls, handled](HANDLE, HANDLE) {
            handlerCalls.fetch_add(1, std::memory_order_relaxed);
            ::SetEvent(handled);
        });
    std::wstring error;
    expect(server.start(error), "rollover server should start");
    const std::wstring pipeName = server.snapshot().pipeName;

    HANDLE firstClient = connectPipe(pipeName);
    expect(firstClient != INVALID_HANDLE_VALUE &&
               ::WaitForSingleObject(handled, 5000) == WAIT_OBJECT_0,
           "first client should be handled");

    HANDLE secondClient = connectPipe(pipeName);
    expect(secondClient != INVALID_HANDLE_VALUE &&
               ::WaitForSingleObject(handled, 5000) == WAIT_OBJECT_0,
           "second client should connect while the old client remains open");
    expect(handlerCalls.load(std::memory_order_relaxed) == 2 &&
               server.snapshot().acceptedConnections == 2,
           "serial server should count both connections");

    ::CloseHandle(secondClient);
    ::CloseHandle(firstClient);
    server.stop();
    ::CloseHandle(handled);
}

void testSecondServerCannotClaimPipeName() {
    const std::wstring name = uniquePipeName(L"collision");
    NativeIpc::NamedPipeServer first(name);
    NativeIpc::NamedPipeServer second(name);
    std::wstring error;
    expect(first.start(error), "first server should claim the pipe name");
    expect(!second.start(error) && !error.empty() &&
               second.snapshot().state == NativeIpc::ServerState::Failed,
           "second server should fail while the first instance exists");

    HANDLE extraInstance = ::CreateNamedPipeW(
        name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        NativeIpc::kPipeMode, 1, NativeIpc::kPipeBufferBytes,
        NativeIpc::kPipeBufferBytes, 0, nullptr);
    expect(extraInstance == INVALID_HANDLE_VALUE,
           "single-instance limit should reject another local server");
    if (extraInstance != INVALID_HANDLE_VALUE) {
        ::CloseHandle(extraInstance);
    }
    second.stop();
    first.stop();
}

} // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"current-user and SYSTEM DACL",
         &testSecurityDescriptorContainsOnlyUserAndSystem},
        {"pipe configuration and names",
         &testPipeConfigurationAndNameValidation},
        {"start, connect, stop, and status",
         &testStartConnectStopAndStatus},
        {"serial instance rollover",
         &testSerialInstancesRetainPipeOwnership},
        {"first pipe instance ownership",
         &testSecondServerCannotClaimPipeName},
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
