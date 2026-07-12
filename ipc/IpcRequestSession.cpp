#include "IpcRequestSession.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <utility>

namespace NativeIpc {
namespace {

std::wstring widenAscii(const std::string& value) {
    return std::wstring(value.begin(), value.end());
}

FrameIoResult localIoError(const std::wstring& error) {
    FrameIoResult result;
    result.status = FrameIoStatus::IoError;
    result.error = error;
    return result;
}

IpcDispatchResult cancellationResult(RequestCancelReason reason) {
    IpcDispatchResult result;
    result.errorCode = "cancelled";
    result.errorMessage = std::string("request cancelled before dispatch: ") +
                          RequestCancelReasonName(reason);
    result.completion = RequestCompletion::CancelledBeforeStart;
    return result;
}

IpcDispatchResult invalidDispatcherResult() {
    IpcDispatchResult result;
    result.errorCode = "internal_response_error";
    result.errorMessage = "dispatcher returned an invalid response";
    result.completion = RequestCompletion::CompletionUnknown;
    return result;
}

} // namespace

struct IpcRequestSession::ActiveRequest {
    IpcRequestDto request;
    IpcRequestContext context;
};

const char* RequestCancelReasonName(RequestCancelReason reason) {
    switch (reason) {
    case RequestCancelReason::None:
        return "none";
    case RequestCancelReason::Client:
        return "client";
    case RequestCancelReason::Deadline:
        return "deadline";
    case RequestCancelReason::SessionInvalidated:
        return "session_invalidated";
    case RequestCancelReason::SessionStopping:
        return "session_stopping";
    }
    return "session_stopping";
}

bool RequestCancellation::request(RequestCancelReason reason) {
    if (reason == RequestCancelReason::None) {
        return false;
    }
    RequestCancelReason expected = RequestCancelReason::None;
    const bool changed = reason_.compare_exchange_strong(
        expected, reason, std::memory_order_acq_rel,
        std::memory_order_acquire);
    if (changed || expected != RequestCancelReason::None) {
        booleanToken_->store(true, std::memory_order_release);
    }
    return changed;
}

RequestCancelReason RequestCancellation::reason() const {
    return reason_.load(std::memory_order_acquire);
}

bool RequestCancellation::requested() const {
    return reason() != RequestCancelReason::None;
}

std::shared_ptr<std::atomic<bool>> RequestCancellation::booleanToken() const {
    return booleanToken_;
}

bool IpcRequestContext::cancellationRequested() const {
    return cancellation != nullptr && cancellation->requested();
}

bool IpcRequestContext::deadlineExceeded() const {
    return deadline != (PipeDeadline::max)() &&
           std::chrono::steady_clock::now() >= deadline;
}

IpcRequestSession::IpcRequestSession(
    IpcFramedConnection& connection,
    IIpcRequestDispatcher& dispatcher,
    std::vector<IpcCapability> grantedCapabilities,
    RequestSessionConfig config)
    : connection_(connection), dispatcher_(dispatcher),
      grantedCapabilities_(std::move(grantedCapabilities)), config_(config) {}

IpcRequestSession::~IpcRequestSession() {
    stopWorker();
}

bool IpcRequestSession::hasCapability(IpcCapability capability) const {
    return std::find(grantedCapabilities_.begin(),
                     grantedCapabilities_.end(), capability) !=
           grantedCapabilities_.end();
}

FrameIoResult IpcRequestSession::writeFrame(
    const IpcProtocol::Frame& frame) {
    std::lock_guard<std::mutex> lock(writeMutex_);
    return connection_.writeFrame(
        frame, std::chrono::steady_clock::now() + config_.writeTimeout,
        IpcProtocol::kMaxResponsePayloadBytes);
}

bool IpcRequestSession::sendError(uint64_t requestId,
                                  const char* code,
                                  const std::string& message) {
    std::string payload;
    std::string buildError;
    if (!BuildErrorPayload(code, message, payload, buildError)) {
        recordFatalWrite(localIoError(
            L"failed to build protocol error: " + widenAscii(buildError)));
        return false;
    }
    const FrameIoResult written = writeFrame(
        {IpcProtocol::MessageType::Error, requestId, std::move(payload)});
    if (written.status != FrameIoStatus::Complete) {
        recordFatalWrite(written);
        return false;
    }
    std::lock_guard<std::mutex> lock(stateMutex_);
    ++responsesSent_;
    return true;
}

void IpcRequestSession::recordFatalWrite(const FrameIoResult& result) {
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (!fatalWrite_.has_value()) {
            fatalWrite_ = result;
        }
    }
    connection_.cancelPendingIo();
}

std::optional<FrameIoResult> IpcRequestSession::fatalWrite() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return fatalWrite_;
}

void IpcRequestSession::startWorker() {
    worker_ = std::thread(&IpcRequestSession::workerLoop, this);
}

void IpcRequestSession::stopWorker() {
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (!worker_.joinable()) {
            return;
        }
        workerStopping_ = true;
        if (active_ != nullptr && active_->context.cancellation != nullptr &&
            active_->context.cancellation->request(
                RequestCancelReason::SessionStopping)) {
            ++cancellationsObserved_;
        }
        pending_.reset();
    }
    workerCv_.notify_all();
    worker_.join();
    std::lock_guard<std::mutex> lock(stateMutex_);
    active_.reset();
}

void IpcRequestSession::workerLoop() {
    while (true) {
        std::shared_ptr<ActiveRequest> task;
        {
            std::unique_lock<std::mutex> lock(stateMutex_);
            workerCv_.wait(lock, [this] {
                return workerStopping_ || pending_ != nullptr;
            });
            if (workerStopping_) {
                return;
            }
            task = std::move(pending_);
        }

        IpcDispatchResult dispatchResult;
        const RequestCancelReason reason =
            task->context.cancellation->reason();
        if (reason != RequestCancelReason::None) {
            dispatchResult = cancellationResult(reason);
        } else {
            try {
                dispatchResult =
                    dispatcher_.execute(task->request, task->context);
            } catch (...) {
                dispatchResult.errorCode = "dispatcher_exception";
                dispatchResult.errorMessage =
                    "dispatcher terminated with an exception";
                dispatchResult.completion =
                    RequestCompletion::CompletionUnknown;
            }
        }

        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            if (workerStopping_) {
                return;
            }
        }

        std::string payload;
        std::string buildError;
        if (!BuildResponsePayload(dispatchResult, payload, buildError)) {
            dispatchResult = invalidDispatcherResult();
            if (!BuildResponsePayload(dispatchResult, payload, buildError)) {
                recordFatalWrite(localIoError(
                    L"failed to build fallback response: " +
                    widenAscii(buildError)));
                return;
            }
        }

        const FrameIoResult written = writeFrame(
            {IpcProtocol::MessageType::Response,
             task->request.requestId, std::move(payload)});
        bool failed = false;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            if (active_ == task) {
                active_.reset();
            }
            if (written.status == FrameIoStatus::Complete) {
                ++responsesSent_;
            } else {
                failed = true;
            }
        }
        if (failed) {
            recordFatalWrite(written);
            return;
        }
    }
}

RequestSessionResult IpcRequestSession::finish(
    RequestSessionStatus status,
    const std::wstring& error) {
    stopWorker();
    RequestSessionResult result;
    result.status = status;
    result.error = error;
    std::lock_guard<std::mutex> lock(stateMutex_);
    result.requestFrames = requestFrames_;
    result.dispatchedRequests = dispatchedRequests_;
    result.responsesSent = responsesSent_;
    result.cancellationsObserved = cancellationsObserved_;
    return result;
}

RequestSessionResult IpcRequestSession::finishFromIo(
    const FrameIoResult& result) {
    FrameIoResult effective = result;
    if (const auto fatal = fatalWrite()) {
        effective = *fatal;
    }
    switch (effective.status) {
    case FrameIoStatus::Complete:
        return finish(RequestSessionStatus::IoError,
                      L"unexpected complete I/O result");
    case FrameIoStatus::Closed:
        return finish(RequestSessionStatus::Closed, effective.error);
    case FrameIoStatus::Cancelled:
        return finish(RequestSessionStatus::Cancelled, effective.error);
    case FrameIoStatus::TimedOut:
        return finish(RequestSessionStatus::IdleTimedOut, effective.error);
    case FrameIoStatus::ProtocolError:
        return finish(RequestSessionStatus::ProtocolError, effective.error);
    case FrameIoStatus::IoError:
        return finish(RequestSessionStatus::IoError, effective.error);
    }
    return finish(RequestSessionStatus::IoError,
                  L"unknown framed I/O result");
}

std::optional<RequestSessionResult>
IpcRequestSession::finishIfInvalidated() {
    const auto validation = dispatcher_.validateSession();
    if (validation.valid) {
        return std::nullopt;
    }
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (active_ != nullptr &&
            active_->context.cancellation->request(
                RequestCancelReason::SessionInvalidated)) {
            ++cancellationsObserved_;
        }
    }
    if (!sendError(0, validation.code.c_str(), validation.message)) {
        return finishFromIo(*fatalWrite());
    }
    return finish(RequestSessionStatus::Invalidated,
                  widenAscii(validation.message));
}

RequestSessionResult IpcRequestSession::run() {
    bool alreadyStarted = false;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (runStarted_) {
            alreadyStarted = true;
        } else {
            runStarted_ = true;
        }
    }
    if (alreadyStarted) {
        return finish(RequestSessionStatus::ProtocolError,
                      L"request session may only run once");
    }
    if (config_.idleTimeout.count() <= 0 ||
        config_.writeTimeout.count() <= 0 ||
        config_.validationInterval.count() <= 0 ||
        config_.maxRequestsPerSession == 0 ||
        config_.payload.defaultTimeout.count() <= 0 ||
        config_.payload.maxTimeout.count() <= 0 ||
        config_.payload.defaultTimeout > config_.payload.maxTimeout ||
        config_.payload.maxTimeout.count() >
            static_cast<int64_t>((std::numeric_limits<uint32_t>::max)())) {
        return finish(RequestSessionStatus::ProtocolError,
                      L"request session configuration is invalid");
    }
    startWorker();

    PipeDeadline idleDeadline =
        std::chrono::steady_clock::now() + config_.idleTimeout;

    while (true) {
        if (const auto fatal = fatalWrite()) {
            return finishFromIo(*fatal);
        }
        if (auto invalidated = finishIfInvalidated()) {
            return *invalidated;
        }

        PipeDeadline readDeadline = idleDeadline;
        uint64_t deadlineRequestId = 0;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            if (active_ != nullptr &&
                !active_->context.cancellation->requested() &&
                active_->context.deadline < readDeadline) {
                readDeadline = active_->context.deadline;
                deadlineRequestId = active_->request.requestId;
            }
        }
        const PipeDeadline validationDeadline =
            std::chrono::steady_clock::now() +
            config_.validationInterval;
        if (validationDeadline < readDeadline) {
            readDeadline = validationDeadline;
        }

        const FrameIoResult incoming = connection_.readFrame(
            readDeadline, IpcProtocol::kMaxRequestPayloadBytes);
        if (incoming.status == FrameIoStatus::TimedOut &&
            deadlineRequestId != 0) {
            bool handledDeadline = false;
            {
                std::lock_guard<std::mutex> lock(stateMutex_);
                if (active_ != nullptr &&
                    active_->request.requestId == deadlineRequestId &&
                    std::chrono::steady_clock::now() >=
                        active_->context.deadline) {
                    handledDeadline = true;
                    if (active_->context.cancellation->request(
                            RequestCancelReason::Deadline)) {
                        ++cancellationsObserved_;
                    }
                }
            }
            if (handledDeadline) {
                continue;
            }
            continue;
        }
        if (incoming.status == FrameIoStatus::TimedOut &&
            std::chrono::steady_clock::now() < idleDeadline) {
            continue;
        }
        if (incoming.status != FrameIoStatus::Complete) {
            return finishFromIo(incoming);
        }
        idleDeadline =
            std::chrono::steady_clock::now() + config_.idleTimeout;
        if (auto invalidated = finishIfInvalidated()) {
            return *invalidated;
        }

        if (incoming.frame.type == IpcProtocol::MessageType::Request) {
            bool requestLimitReached = false;
            bool duplicateRequestId = false;
            {
                std::lock_guard<std::mutex> lock(stateMutex_);
                if (seenRequestIds_.size() >=
                    config_.maxRequestsPerSession) {
                    requestLimitReached = true;
                } else if (!seenRequestIds_
                                .insert(incoming.frame.requestId)
                                .second) {
                    duplicateRequestId = true;
                } else {
                    ++requestFrames_;
                }
            }
            if (requestLimitReached) {
                if (!sendError(
                        incoming.frame.requestId, "session_request_limit",
                        "request count limit reached; reconnect required")) {
                    return finishFromIo(*fatalWrite());
                }
                return finish(RequestSessionStatus::RequestLimitReached,
                              L"request count limit reached");
            }
            if (duplicateRequestId) {
                if (!sendError(
                        incoming.frame.requestId, "duplicate_request_id",
                        "request id was already used in this session")) {
                    return finishFromIo(*fatalWrite());
                }
                continue;
            }

            const auto parsed = ParseRequestPayload(
                incoming.frame.requestId, incoming.frame.payload,
                config_.payload);
            if (!parsed.valid) {
                if (!sendError(incoming.frame.requestId, "invalid_request",
                               parsed.error)) {
                    return finishFromIo(*fatalWrite());
                }
                continue;
            }

            bool sessionBusy = false;
            {
                std::lock_guard<std::mutex> lock(stateMutex_);
                sessionBusy = active_ != nullptr;
            }
            if (sessionBusy) {
                if (!sendError(incoming.frame.requestId, "session_busy",
                               "wait for the active request response")) {
                    return finishFromIo(*fatalWrite());
                }
                continue;
            }

            IpcCapability required = IpcCapability::Observe;
            bool knownMethod = false;
            try {
                knownMethod = dispatcher_.resolveCapability(
                    parsed.request.method, required);
            } catch (...) {
                knownMethod = false;
            }
            if (!knownMethod) {
                if (!sendError(incoming.frame.requestId, "method_not_found",
                               "request method is not registered")) {
                    return finishFromIo(*fatalWrite());
                }
                continue;
            }
            if (!hasCapability(required)) {
                if (!sendError(incoming.frame.requestId, "capability_denied",
                               "required capability was not granted")) {
                    return finishFromIo(*fatalWrite());
                }
                continue;
            }

            auto task = std::make_shared<ActiveRequest>();
            task->request = parsed.request;
            task->context.deadline =
                std::chrono::steady_clock::now() + parsed.request.timeout;
            task->context.cancellation =
                std::make_shared<RequestCancellation>();
            {
                std::lock_guard<std::mutex> lock(stateMutex_);
                active_ = task;
                pending_ = task;
                ++dispatchedRequests_;
            }
            workerCv_.notify_one();
            continue;
        }

        if (incoming.frame.type == IpcProtocol::MessageType::Cancel) {
            std::string validationError;
            if (!ValidateCancelPayload(incoming.frame.payload,
                                       validationError)) {
                if (!sendError(incoming.frame.requestId, "invalid_cancel",
                               validationError)) {
                    return finishFromIo(*fatalWrite());
                }
                continue;
            }

            bool activeRequest = false;
            {
                std::lock_guard<std::mutex> lock(stateMutex_);
                if (active_ != nullptr &&
                    active_->request.requestId == incoming.frame.requestId) {
                    activeRequest = true;
                    if (active_->context.cancellation->request(
                            RequestCancelReason::Client)) {
                        ++cancellationsObserved_;
                    }
                }
            }
            if (!activeRequest &&
                !sendError(incoming.frame.requestId, "request_not_active",
                           "request id is not currently active")) {
                return finishFromIo(*fatalWrite());
            }
            continue;
        }

        if (!sendError(incoming.frame.requestId, "unexpected_message",
                       "only Request and Cancel are valid after HelloAck")) {
            return finishFromIo(*fatalWrite());
        }
        return finish(RequestSessionStatus::ProtocolError,
                      L"unexpected post-handshake message type");
    }
}

} // namespace NativeIpc
