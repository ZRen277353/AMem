#pragma once

#include "IpcHandshakeSession.h"
#include "IpcRequestProtocol.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace NativeIpc {

enum class RequestCancelReason {
    None,
    Client,
    Deadline,
    SessionStopping,
};

const char* RequestCancelReasonName(RequestCancelReason reason);

class RequestCancellation final {
public:
    bool request(RequestCancelReason reason);
    RequestCancelReason reason() const;
    bool requested() const;

private:
    std::atomic<RequestCancelReason> reason_{RequestCancelReason::None};
};

struct IpcRequestContext {
    PipeDeadline deadline = (PipeDeadline::max)();
    std::shared_ptr<RequestCancellation> cancellation;

    bool cancellationRequested() const;
    bool deadlineExceeded() const;
};

class IIpcRequestDispatcher {
public:
    virtual ~IIpcRequestDispatcher() = default;

    // Capability metadata is server-owned. Unknown methods return false.
    virtual bool resolveCapability(const std::string& method,
                                   IpcCapability& capability) const = 0;
    virtual IpcDispatchResult execute(const IpcRequestDto& request,
                                      const IpcRequestContext& context) = 0;
};

constexpr size_t kDefaultMaxRequestsPerSession = 1024;

struct RequestSessionConfig {
    std::chrono::milliseconds idleTimeout{5 * 60 * 1000};
    std::chrono::milliseconds writeTimeout{5000};
    RequestPayloadConfig payload;
    size_t maxRequestsPerSession = kDefaultMaxRequestsPerSession;
};

enum class RequestSessionStatus {
    Closed,
    Cancelled,
    IdleTimedOut,
    RequestLimitReached,
    ProtocolError,
    IoError,
};

struct RequestSessionResult {
    RequestSessionStatus status = RequestSessionStatus::IoError;
    size_t requestFrames = 0;
    size_t dispatchedRequests = 0;
    size_t responsesSent = 0;
    size_t cancellationsObserved = 0;
    std::wstring error;
};

class IpcRequestSession final {
public:
    IpcRequestSession(IpcFramedConnection& connection,
                      IIpcRequestDispatcher& dispatcher,
                      std::vector<IpcCapability> grantedCapabilities,
                      RequestSessionConfig config = {});
    ~IpcRequestSession();

    IpcRequestSession(const IpcRequestSession&) = delete;
    IpcRequestSession& operator=(const IpcRequestSession&) = delete;

    // Runs the reader loop on the caller and one owned serial dispatch worker.
    // The dispatcher must observe context cancellation so shutdown can join.
    RequestSessionResult run();

private:
    struct ActiveRequest;

    bool hasCapability(IpcCapability capability) const;
    bool sendError(uint64_t requestId,
                   const char* code,
                   const std::string& message);
    FrameIoResult writeFrame(const IpcProtocol::Frame& frame);
    void recordFatalWrite(const FrameIoResult& result);
    std::optional<FrameIoResult> fatalWrite() const;

    void startWorker();
    void stopWorker();
    void workerLoop();

    RequestSessionResult finish(RequestSessionStatus status,
                                const std::wstring& error);
    RequestSessionResult finishFromIo(const FrameIoResult& result);

    IpcFramedConnection& connection_;
    IIpcRequestDispatcher& dispatcher_;
    std::vector<IpcCapability> grantedCapabilities_;
    RequestSessionConfig config_;

    mutable std::mutex stateMutex_;
    std::condition_variable workerCv_;
    std::shared_ptr<ActiveRequest> active_;
    std::shared_ptr<ActiveRequest> pending_;
    std::optional<FrameIoResult> fatalWrite_;
    bool workerStopping_ = false;
    bool runStarted_ = false;
    size_t requestFrames_ = 0;
    size_t dispatchedRequests_ = 0;
    size_t responsesSent_ = 0;
    size_t cancellationsObserved_ = 0;
    std::unordered_set<uint64_t> seenRequestIds_;

    std::mutex writeMutex_;
    std::thread worker_;
};

} // namespace NativeIpc
