#include "IpcFramedConnection.h"

#include "NativePipeSecurity.h"

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

namespace NativeIpc {
namespace {

std::wstring widenAscii(const std::string& value) {
    return std::wstring(value.begin(), value.end());
}

bool isClosedError(DWORD error) {
    return error == ERROR_BROKEN_PIPE ||
           error == ERROR_PIPE_NOT_CONNECTED || error == ERROR_NO_DATA;
}

DWORD waitMilliseconds(PipeDeadline deadline) {
    if (deadline == (PipeDeadline::max)()) {
        return INFINITE;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
        return 0;
    }
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
        deadline - now);
    const auto maxWait = static_cast<int64_t>(INFINITE - 1u);
    if (remaining.count() > maxWait) {
        return INFINITE - 1u;
    }
    return static_cast<DWORD>(remaining.count());
}

FrameIoResult errorResult(FrameIoStatus status,
                          size_t transferred,
                          const std::wstring& error) {
    FrameIoResult result;
    result.status = status;
    result.bytesTransferred = transferred;
    result.error = error;
    return result;
}

} // namespace

FrameIoResult IpcFramedConnection::readFrame(
    PipeDeadline deadline,
    uint32_t maxPayloadBytes) {
    const auto fillTo = [&](size_t targetSize) {
        const size_t retained = readBuffer_.size();
        if (retained >= targetSize) {
            FrameIoResult complete;
            complete.status = FrameIoStatus::Complete;
            complete.bytesTransferred = retained;
            return complete;
        }
        readBuffer_.resize(targetSize);
        FrameIoResult result = transferExact(
            false, readBuffer_.data() + retained, targetSize - retained,
            deadline);
        if (result.status != FrameIoStatus::Complete) {
            readBuffer_.resize(retained + result.bytesTransferred);
        }
        result.bytesTransferred = readBuffer_.size();
        return result;
    };

    FrameIoResult headerIo = fillTo(IpcProtocol::kHeaderSize);
    if (headerIo.status != FrameIoStatus::Complete) {
        if (headerIo.status == FrameIoStatus::Closed &&
            !readBuffer_.empty()) {
            return errorResult(FrameIoStatus::ProtocolError,
                               readBuffer_.size(),
                               L"connection closed during frame header");
        }
        return headerIo;
    }

    const auto header = IpcProtocol::DecodeHeader(
        readBuffer_.data(), IpcProtocol::kHeaderSize, maxPayloadBytes);
    if (header.status != IpcProtocol::DecodeStatus::Complete) {
        return errorResult(FrameIoStatus::ProtocolError,
                           IpcProtocol::kHeaderSize,
                           widenAscii(header.error));
    }

    const size_t frameSize =
        IpcProtocol::kHeaderSize + header.header.payloadLength;
    FrameIoResult frameIo = fillTo(frameSize);
    if (frameIo.status != FrameIoStatus::Complete) {
        if (frameIo.status == FrameIoStatus::Closed) {
            return errorResult(FrameIoStatus::ProtocolError,
                               readBuffer_.size(),
                               L"connection closed during frame payload");
        }
        return frameIo;
    }

    auto decoded = IpcProtocol::DecodeFrame(readBuffer_, maxPayloadBytes);
    if (decoded.status != IpcProtocol::DecodeStatus::Complete) {
        return errorResult(FrameIoStatus::ProtocolError, readBuffer_.size(),
                           widenAscii(decoded.error));
    }
    readBuffer_.clear();

    FrameIoResult result;
    result.status = FrameIoStatus::Complete;
    result.frame = std::move(decoded.frame);
    result.bytesTransferred = frameSize;
    return result;
}

FrameIoResult IpcFramedConnection::writeFrame(
    const IpcProtocol::Frame& frame,
    PipeDeadline deadline,
    uint32_t maxPayloadBytes) {
    std::vector<uint8_t> encoded;
    std::string encodeError;
    if (!IpcProtocol::EncodeFrame(frame, encoded, encodeError,
                                  maxPayloadBytes)) {
        return errorResult(FrameIoStatus::ProtocolError, 0,
                           widenAscii(encodeError));
    }
    return transferExact(true, encoded.data(), encoded.size(), deadline);
}

void IpcFramedConnection::cancelPendingIo() const {
    if (pipe_ != INVALID_HANDLE_VALUE && pipe_ != nullptr) {
        ::CancelIoEx(pipe_, nullptr);
    }
}

FrameIoResult IpcFramedConnection::transferExact(bool write,
                                                 uint8_t* buffer,
                                                 size_t size,
                                                 PipeDeadline deadline) {
    if (pipe_ == INVALID_HANDLE_VALUE || pipe_ == nullptr) {
        return errorResult(FrameIoStatus::IoError, 0,
                           L"pipe handle is invalid");
    }

    size_t offset = 0;
    while (offset < size) {
        if (stopRequested()) {
            return errorResult(FrameIoStatus::Cancelled, offset,
                               L"pipe operation cancelled");
        }
        if (deadline != (PipeDeadline::max)() &&
            std::chrono::steady_clock::now() >= deadline) {
            return errorResult(FrameIoStatus::TimedOut, offset,
                               L"pipe operation timed out");
        }

        HANDLE operationEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (operationEvent == nullptr) {
            return errorResult(
                FrameIoStatus::IoError, offset,
                L"CreateEventW(pipe operation) failed: " +
                    FormatWindowsError(::GetLastError()));
        }

        OVERLAPPED overlapped{};
        overlapped.hEvent = operationEvent;
        const size_t remaining = size - offset;
        const DWORD chunk = static_cast<DWORD>((std::min)(
            remaining,
            static_cast<size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD transferred = 0;
        const BOOL immediate =
            write ? ::WriteFile(pipe_, buffer + offset, chunk, &transferred,
                                &overlapped)
                  : ::ReadFile(pipe_, buffer + offset, chunk, &transferred,
                               &overlapped);

        if (!immediate) {
            const DWORD operationError = ::GetLastError();
            if (operationError != ERROR_IO_PENDING) {
                ::CloseHandle(operationEvent);
                if (isClosedError(operationError)) {
                    return errorResult(FrameIoStatus::Closed, offset,
                                       L"pipe peer closed");
                }
                if (operationError == ERROR_OPERATION_ABORTED &&
                    stopRequested()) {
                    return errorResult(FrameIoStatus::Cancelled, offset,
                                       L"pipe operation cancelled");
                }
                return errorResult(
                    FrameIoStatus::IoError, offset,
                    (write ? L"WriteFile failed: " : L"ReadFile failed: ") +
                        FormatWindowsError(operationError));
            }

            HANDLE events[2]{};
            DWORD eventCount = 0;
            DWORD stopIndex = MAXDWORD;
            if (stopEvent_ != nullptr) {
                stopIndex = eventCount;
                events[eventCount++] = stopEvent_;
            }
            const DWORD operationIndex = eventCount;
            events[eventCount++] = operationEvent;
            const DWORD waitResult = ::WaitForMultipleObjects(
                eventCount, events, FALSE, waitMilliseconds(deadline));

            if (stopIndex != MAXDWORD &&
                waitResult == WAIT_OBJECT_0 + stopIndex) {
                ::CancelIoEx(pipe_, &overlapped);
                ::GetOverlappedResult(pipe_, &overlapped, &transferred, TRUE);
                ::CloseHandle(operationEvent);
                return errorResult(FrameIoStatus::Cancelled, offset,
                                   L"pipe operation cancelled");
            }
            if (waitResult == WAIT_TIMEOUT) {
                ::CancelIoEx(pipe_, &overlapped);
                ::GetOverlappedResult(pipe_, &overlapped, &transferred, TRUE);
                ::CloseHandle(operationEvent);
                return errorResult(FrameIoStatus::TimedOut, offset,
                                   L"pipe operation timed out");
            }
            if (waitResult != WAIT_OBJECT_0 + operationIndex) {
                const DWORD waitError = ::GetLastError();
                ::CancelIoEx(pipe_, &overlapped);
                ::GetOverlappedResult(pipe_, &overlapped, &transferred, TRUE);
                ::CloseHandle(operationEvent);
                return errorResult(
                    FrameIoStatus::IoError, offset,
                    L"WaitForMultipleObjects(pipe operation) failed: " +
                        FormatWindowsError(waitError));
            }

            if (!::GetOverlappedResult(pipe_, &overlapped, &transferred,
                                       FALSE)) {
                const DWORD resultError = ::GetLastError();
                ::CloseHandle(operationEvent);
                if (isClosedError(resultError)) {
                    return errorResult(FrameIoStatus::Closed, offset,
                                       L"pipe peer closed");
                }
                if (resultError == ERROR_OPERATION_ABORTED &&
                    stopRequested()) {
                    return errorResult(FrameIoStatus::Cancelled, offset,
                                       L"pipe operation cancelled");
                }
                return errorResult(
                    FrameIoStatus::IoError, offset,
                    L"GetOverlappedResult failed: " +
                        FormatWindowsError(resultError));
            }
        }

        ::CloseHandle(operationEvent);
        if (transferred == 0) {
            return errorResult(write ? FrameIoStatus::IoError
                                     : FrameIoStatus::Closed,
                               offset,
                               write ? L"pipe write transferred zero bytes"
                                     : L"pipe peer closed");
        }
        offset += transferred;
    }

    FrameIoResult result;
    result.status = FrameIoStatus::Complete;
    result.bytesTransferred = offset;
    return result;
}

bool IpcFramedConnection::stopRequested() const {
    return stopEvent_ != nullptr &&
           ::WaitForSingleObject(stopEvent_, 0) == WAIT_OBJECT_0;
}

} // namespace NativeIpc
