#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <utility>

class DeviceSession {
    struct SharedRequestLease;

public:
    enum class State {
        Disconnected,
        Connecting,
        Connected,
        Poisoned,
    };

    class RequestLease {
    public:
        RequestLease(RequestLease&&) noexcept = default;
        RequestLease& operator=(RequestLease&&) noexcept = default;
        RequestLease(const RequestLease&) = delete;
        RequestLease& operator=(const RequestLease&) = delete;

        explicit operator bool() const { return valid_; }
        uint64_t generation() const { return generation_; }
        bool isCurrent() const;

    private:
        friend class DeviceSession;
        RequestLease(DeviceSession* owner,
                     std::shared_ptr<SharedRequestLease> sharedLease,
                     uint64_t generation,
                     bool valid)
            : owner_(owner),
              sharedLease_(std::move(sharedLease)),
              generation_(generation),
              valid_(valid) {}

        DeviceSession* owner_ = nullptr;
        std::shared_ptr<SharedRequestLease> sharedLease_;
        uint64_t generation_ = 0;
        bool valid_ = false;
    };

    using LifecycleLease = std::unique_lock<std::shared_mutex>;

    static DeviceSession& GetInstance();

    RequestLease AcquireRequest();
    LifecycleLease AcquireLifecycle();

    // These transitions require an active LifecycleLease held by the caller.
    void BeginConnect();
    void FinishConnect(bool success);
    void Disconnect();

    // May be called by an in-flight request while it holds a shared lease.
    // The first poison event invalidates the generation immediately; socket
    // closure/reconnect is completed later under the exclusive lifecycle gate.
    void MarkPoisoned();

    State GetState() const;
    bool IsConnected() const;
    bool IsPoisoned() const;
    uint64_t GetGeneration() const;

private:
    struct SharedRequestLease {
        SharedRequestLease(DeviceSession* ownerValue,
                           std::shared_lock<std::shared_mutex> lockValue,
                           uint64_t generationValue)
            : owner(ownerValue),
              lock(std::move(lockValue)),
              generation(generationValue) {}

        DeviceSession* owner = nullptr;
        std::shared_lock<std::shared_mutex> lock;
        uint64_t generation = 0;
    };

    static thread_local std::weak_ptr<SharedRequestLease> activeRequestLease_;

    mutable std::shared_mutex lifecycleGate_;
    std::atomic<State> state_{State::Disconnected};
    std::atomic<uint64_t> generation_{0};
};
