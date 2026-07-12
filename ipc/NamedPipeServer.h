#pragma once

#include "NativePipeSecurity.h"

#include <windows.h>

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace NativeIpc {

inline constexpr wchar_t kDefaultPipeName[] =
    L"\\\\.\\pipe\\AMem.NativeAgent.v1";
inline constexpr DWORD kPipeOpenMode =
    PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
    FILE_FLAG_FIRST_PIPE_INSTANCE;
inline constexpr DWORD kPipeMode =
    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
    PIPE_REJECT_REMOTE_CLIENTS;
inline constexpr DWORD kPipeBufferBytes = 64u * 1024u;

bool IsValidLocalPipeName(const std::wstring& pipeName);

enum class ServerState {
    Stopped,
    Listening,
    Connected,
    Stopping,
    Failed,
};

struct ServerSnapshot {
    ServerState state = ServerState::Stopped;
    uint64_t acceptedConnections = 0;
    std::wstring pipeName;
    std::wstring lastError;
};

class NamedPipeServer final {
public:
    // Runs serially on the owned server thread. Handlers must use cancellable
    // I/O and return when stopEvent is signaled so stop() can always join.
    using ClientHandler = std::function<void(HANDLE pipe, HANDLE stopEvent)>;

    explicit NamedPipeServer(std::wstring pipeName = kDefaultPipeName,
                             ClientHandler handler = {});
    ~NamedPipeServer();

    NamedPipeServer(const NamedPipeServer&) = delete;
    NamedPipeServer& operator=(const NamedPipeServer&) = delete;

    bool start(std::wstring& error);
    void stop();
    ServerSnapshot snapshot() const;

private:
    HANDLE createPipeInstance(std::wstring& error);
    bool waitForClient(HANDLE pipe, std::wstring& error) const;
    void serverLoop(HANDLE firstPipe);
    bool stopRequested() const;
    void setFailure(const std::wstring& error);
    void closeStoppedResources();

    const std::wstring pipeName_;
    const ClientHandler handler_;

    mutable std::mutex lifecycleMutex_;
    mutable std::mutex stateMutex_;
    std::thread serverThread_;
    NativePipeSecurity security_;
    HANDLE stopEvent_ = nullptr;
    HANDLE activePipe_ = INVALID_HANDLE_VALUE;
    ServerState state_ = ServerState::Stopped;
    uint64_t acceptedConnections_ = 0;
    std::wstring lastError_;
};

} // namespace NativeIpc
