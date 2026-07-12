#pragma once

#include "IpcProtocol.h"

#include <windows.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace NativeIpc {

using PipeDeadline = std::chrono::steady_clock::time_point;

enum class FrameIoStatus {
    Complete,
    Closed,
    Cancelled,
    TimedOut,
    ProtocolError,
    IoError,
};

struct FrameIoResult {
    FrameIoStatus status = FrameIoStatus::IoError;
    IpcProtocol::Frame frame;
    size_t bytesTransferred = 0;
    std::wstring error;
};

class IpcFramedConnection final {
public:
    IpcFramedConnection(HANDLE pipe, HANDLE stopEvent)
        : pipe_(pipe), stopEvent_(stopEvent) {}

    FrameIoResult readFrame(
        PipeDeadline deadline = (PipeDeadline::max)(),
        uint32_t maxPayloadBytes = IpcProtocol::kMaxFramePayloadBytes);

    FrameIoResult writeFrame(
        const IpcProtocol::Frame& frame,
        PipeDeadline deadline = (PipeDeadline::max)(),
        uint32_t maxPayloadBytes = IpcProtocol::kMaxFramePayloadBytes);

    // Wakes another thread blocked in overlapped I/O on this connection.
    void cancelPendingIo() const;

private:
    FrameIoResult transferExact(bool write,
                                uint8_t* buffer,
                                size_t size,
                                PipeDeadline deadline);
    bool stopRequested() const;

    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    HANDLE stopEvent_ = nullptr;
};

} // namespace NativeIpc
