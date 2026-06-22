#pragma once

#include <winsock2.h>
#include <windows.h>

#include <chrono>

namespace SocketIoTimeout {

inline thread_local DWORD g_threadTimeoutMs = 0;
inline thread_local std::chrono::steady_clock::time_point g_threadDeadline;

inline constexpr DWORD kMaxTimeoutMs = 300000;

inline DWORD ClampTimeoutMs(unsigned long long requestedMs) {
    return requestedMs > kMaxTimeoutMs
               ? kMaxTimeoutMs
               : static_cast<DWORD>(requestedMs);
}

inline bool HasThreadTimeout() {
    return g_threadTimeoutMs > 0;
}

inline DWORD GetThreadTimeoutMs() {
    return g_threadTimeoutMs;
}

inline bool IsThreadTimeoutExpired() {
    return HasThreadTimeout() &&
           std::chrono::steady_clock::now() >= g_threadDeadline;
}

inline DWORD GetRemainingTimeoutMs() {
    if (!HasThreadTimeout()) {
        return 0;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now >= g_threadDeadline) {
        return 1;
    }

    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            g_threadDeadline - now).count();
    if (remaining <= 0) {
        return 1;
    }

    return ClampTimeoutMs(static_cast<unsigned long long>(remaining));
}

class ScopedTimeout {
public:
    explicit ScopedTimeout(int seconds)
        : previous_(g_threadTimeoutMs),
          previousDeadline_(g_threadDeadline) {
        if (seconds <= 0) {
            g_threadTimeoutMs = 0;
            g_threadDeadline = {};
            return;
        }

        const unsigned long long requested =
            static_cast<unsigned long long>(seconds) * 1000ull;
        g_threadTimeoutMs = ClampTimeoutMs(requested);
        g_threadDeadline =
            std::chrono::steady_clock::now() +
            std::chrono::milliseconds(g_threadTimeoutMs);
    }

    ~ScopedTimeout() {
        g_threadTimeoutMs = previous_;
        g_threadDeadline = previousDeadline_;
    }

    ScopedTimeout(const ScopedTimeout&) = delete;
    ScopedTimeout& operator=(const ScopedTimeout&) = delete;

private:
    DWORD previous_ = 0;
    std::chrono::steady_clock::time_point previousDeadline_;
};

class SocketOptionTimeoutGuard {
public:
    SocketOptionTimeoutGuard(SOCKET sock, int option)
        : sock_(sock), option_(option) {
        if (!HasThreadTimeout() || sock_ == INVALID_SOCKET) {
            return;
        }

        const DWORD timeoutMs = GetRemainingTimeoutMs();
        int optLen = sizeof(previous_);
        restore_ = ::getsockopt(sock_, SOL_SOCKET, option_,
                                reinterpret_cast<char*>(&previous_),
                                &optLen) != SOCKET_ERROR;
        (void)::setsockopt(sock_, SOL_SOCKET, option_,
                           reinterpret_cast<const char*>(&timeoutMs),
                           sizeof(timeoutMs));
    }

    ~SocketOptionTimeoutGuard() {
        if (restore_ && sock_ != INVALID_SOCKET) {
            (void)::setsockopt(sock_, SOL_SOCKET, option_,
                               reinterpret_cast<const char*>(&previous_),
                               sizeof(previous_));
        }
    }

    void dismissRestore() {
        restore_ = false;
    }

    SocketOptionTimeoutGuard(const SocketOptionTimeoutGuard&) = delete;
    SocketOptionTimeoutGuard& operator=(const SocketOptionTimeoutGuard&) = delete;

private:
    SOCKET sock_ = INVALID_SOCKET;
    int option_ = 0;
    DWORD previous_ = 0;
    bool restore_ = false;
};

} // namespace SocketIoTimeout
