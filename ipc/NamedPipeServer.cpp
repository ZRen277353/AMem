#include "NamedPipeServer.h"

#include <exception>
#include <system_error>
#include <utility>

namespace NativeIpc {

bool IsValidLocalPipeName(const std::wstring& pipeName) {
    constexpr wchar_t prefix[] = L"\\\\.\\pipe\\";
    constexpr size_t prefixLength = (sizeof(prefix) / sizeof(prefix[0])) - 1;
    if (pipeName.size() <= prefixLength ||
        pipeName.compare(0, prefixLength, prefix) != 0) {
        return false;
    }
    return pipeName.find_first_of(L"\\/", prefixLength) ==
           std::wstring::npos;
}

NamedPipeServer::NamedPipeServer(std::wstring pipeName,
                                 ClientHandler handler)
    : pipeName_(std::move(pipeName)), handler_(std::move(handler)) {}

NamedPipeServer::~NamedPipeServer() {
    stop();
}

bool NamedPipeServer::start(std::wstring& error) {
    std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);
    error.clear();

    {
        std::lock_guard<std::mutex> stateLock(stateMutex_);
        if (state_ == ServerState::Listening ||
            state_ == ServerState::Connected) {
            return true;
        }
    }

    if (serverThread_.joinable()) {
        serverThread_.join();
    }
    closeStoppedResources();

    if (!IsValidLocalPipeName(pipeName_)) {
        error = L"pipe name must be a single local \\.\\pipe entry";
        setFailure(error);
        return false;
    }
    if (!security_.initialize(error)) {
        setFailure(error);
        return false;
    }

    stopEvent_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (stopEvent_ == nullptr) {
        error = L"CreateEventW(stop) failed: " +
                FormatWindowsError(::GetLastError());
        security_.reset();
        setFailure(error);
        return false;
    }

    HANDLE firstPipe = createPipeInstance(error);
    if (firstPipe == INVALID_HANDLE_VALUE) {
        closeStoppedResources();
        setFailure(error);
        return false;
    }

    {
        std::lock_guard<std::mutex> stateLock(stateMutex_);
        activePipe_ = firstPipe;
        state_ = ServerState::Listening;
        acceptedConnections_ = 0;
        lastError_.clear();
    }

    try {
        serverThread_ =
            std::thread(&NamedPipeServer::serverLoop, this, firstPipe);
    } catch (const std::system_error& threadError) {
        ::CloseHandle(firstPipe);
        {
            std::lock_guard<std::mutex> stateLock(stateMutex_);
            activePipe_ = INVALID_HANDLE_VALUE;
        }
        error = L"failed to create Named Pipe server thread, error " +
                std::to_wstring(threadError.code().value());
        closeStoppedResources();
        setFailure(error);
        return false;
    }
    return true;
}

void NamedPipeServer::stop() {
    std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);

    if (stopEvent_ != nullptr) {
        ::SetEvent(stopEvent_);
    }
    {
        std::lock_guard<std::mutex> stateLock(stateMutex_);
        if (!serverThread_.joinable() && state_ == ServerState::Stopped) {
            closeStoppedResources();
            return;
        }
        state_ = ServerState::Stopping;
        if (activePipe_ != INVALID_HANDLE_VALUE) {
            ::CancelIoEx(activePipe_, nullptr);
        }
    }
    if (serverThread_.joinable()) {
        serverThread_.join();
    }

    closeStoppedResources();
    std::lock_guard<std::mutex> stateLock(stateMutex_);
    activePipe_ = INVALID_HANDLE_VALUE;
    state_ = ServerState::Stopped;
}

ServerSnapshot NamedPipeServer::snapshot() const {
    std::lock_guard<std::mutex> stateLock(stateMutex_);
    ServerSnapshot result;
    result.state = state_;
    result.acceptedConnections = acceptedConnections_;
    result.pipeName = pipeName_;
    result.lastError = lastError_;
    return result;
}

HANDLE NamedPipeServer::createPipeInstance(std::wstring& error) {
    HANDLE pipe = ::CreateNamedPipeW(
        pipeName_.c_str(), kPipeOpenMode, kPipeMode, 1,
        kPipeBufferBytes, kPipeBufferBytes, 0, security_.attributes());
    if (pipe == INVALID_HANDLE_VALUE) {
        error = L"CreateNamedPipeW failed: " +
                FormatWindowsError(::GetLastError());
    }
    return pipe;
}

bool NamedPipeServer::waitForClient(HANDLE pipe, std::wstring& error) const {
    HANDLE connectedEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (connectedEvent == nullptr) {
        error = L"CreateEventW(connect) failed: " +
                FormatWindowsError(::GetLastError());
        return false;
    }

    OVERLAPPED overlapped{};
    overlapped.hEvent = connectedEvent;
    bool connected = false;
    if (::ConnectNamedPipe(pipe, &overlapped)) {
        connected = true;
    } else {
        const DWORD connectError = ::GetLastError();
        if (connectError == ERROR_PIPE_CONNECTED) {
            connected = true;
        } else if (connectError == ERROR_IO_PENDING) {
            const HANDLE events[] = {stopEvent_, connectedEvent};
            const DWORD waitResult =
                ::WaitForMultipleObjects(2, events, FALSE, INFINITE);
            if (waitResult == WAIT_OBJECT_0) {
                ::CancelIoEx(pipe, &overlapped);
                ::WaitForSingleObject(connectedEvent, INFINITE);
            } else if (waitResult == WAIT_OBJECT_0 + 1) {
                DWORD transferred = 0;
                connected = ::GetOverlappedResult(pipe, &overlapped,
                                                  &transferred, FALSE) != FALSE;
                if (!connected) {
                    error = L"ConnectNamedPipe completion failed: " +
                            FormatWindowsError(::GetLastError());
                }
            } else {
                error = L"WaitForMultipleObjects(connect) failed: " +
                        FormatWindowsError(::GetLastError());
                ::CancelIoEx(pipe, &overlapped);
                ::WaitForSingleObject(connectedEvent, INFINITE);
            }
        } else if (!(connectError == ERROR_OPERATION_ABORTED &&
                     stopRequested())) {
            error = L"ConnectNamedPipe failed: " +
                    FormatWindowsError(connectError);
        }
    }

    ::CloseHandle(connectedEvent);
    return connected;
}

void NamedPipeServer::serverLoop(HANDLE firstPipe) {
    HANDLE pipe = firstPipe;
    while (!stopRequested()) {
        std::wstring error;
        if (!waitForClient(pipe, error)) {
            {
                std::lock_guard<std::mutex> stateLock(stateMutex_);
                if (activePipe_ == pipe) {
                    activePipe_ = INVALID_HANDLE_VALUE;
                }
            }
            ::CloseHandle(pipe);
            if (!stopRequested() && !error.empty()) {
                setFailure(error);
            }
            return;
        }

        bool handleClient = false;
        {
            std::lock_guard<std::mutex> stateLock(stateMutex_);
            ++acceptedConnections_;
            if (state_ != ServerState::Stopping) {
                state_ = ServerState::Connected;
                handleClient = true;
            }
        }

        if (handleClient && handler_) {
            try {
                handler_(pipe, stopEvent_);
            } catch (const std::exception&) {
                std::lock_guard<std::mutex> stateLock(stateMutex_);
                lastError_ = L"client handler threw an exception";
            } catch (...) {
                std::lock_guard<std::mutex> stateLock(stateMutex_);
                lastError_ = L"client handler failed with an unknown error";
            }
        }

        ::DisconnectNamedPipe(pipe);
        bool keepListening = false;
        {
            std::lock_guard<std::mutex> stateLock(stateMutex_);
            if (state_ != ServerState::Stopping && !stopRequested()) {
                state_ = ServerState::Listening;
                keepListening = true;
            } else if (activePipe_ == pipe) {
                activePipe_ = INVALID_HANDLE_VALUE;
            }
        }
        if (!keepListening) {
            ::CloseHandle(pipe);
            return;
        }
    }

    {
        std::lock_guard<std::mutex> stateLock(stateMutex_);
        if (activePipe_ == pipe) {
            activePipe_ = INVALID_HANDLE_VALUE;
        }
    }
    ::CloseHandle(pipe);
}

bool NamedPipeServer::stopRequested() const {
    return stopEvent_ != nullptr &&
           ::WaitForSingleObject(stopEvent_, 0) == WAIT_OBJECT_0;
}

void NamedPipeServer::setFailure(const std::wstring& error) {
    std::lock_guard<std::mutex> stateLock(stateMutex_);
    state_ = ServerState::Failed;
    lastError_ = error;
}

void NamedPipeServer::closeStoppedResources() {
    if (stopEvent_ != nullptr) {
        ::CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
    }
    security_.reset();
}

} // namespace NativeIpc
