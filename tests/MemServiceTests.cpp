#include "../gui/ai/AgentMemTools.h"
#include "../gui/ai/AgentController.h"
#include "../gui/ai/AgentTaskExecutor.h"
#include "../gui/ai/ProviderRegistry.h"
#include "../gui/ai/ToolExecutor.h"
#include "../mem/Address.h"
#include "../mem/MemService.h"
#include "../socket/DeviceSession.h"
#include "../third_party/nlohmann/json.hpp"

#include <functional>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace AI {

// AgentController's provider-dispatch path is not exercised by this
// no-device test binary. Supplying the one referenced registry method keeps
// the controller's tool/context behavior independently linkable.
AIProvider* ProviderRegistry::getProvider(const std::string&) {
    return nullptr;
}

} // namespace AI

namespace {

using nlohmann::json;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

AI::ToolCall toolCall(std::string id,
                      std::string name,
                      std::string arguments = "{}");

class FakeBackend final : public Mem::IMemBackend {
public:
    bool connected = true;
    bool poisoned = false;
    uint64_t generation = 1;
    Mem::TargetSnapshot target{42, 420, 2, 1};
    std::string selectedName = "com.example.game";
    std::vector<Mem::ProcessInfo> processes = {
        {10, "android.system"},
        {42, "com.example.game"},
        {84, "com.example.game.helper"},
    };
    std::vector<unsigned char> memory = {0xDE, 0xAD, 0xBE, 0xEF};
    bool fetchProcessesSucceeds = true;
    bool openProcessSucceeds = true;
    bool readMemorySucceeds = true;
    bool changeTargetAfterRead = false;
    bool changeGenerationAfterRead = false;
    bool changeGenerationDuringOpen = false;
    bool writeRequestStarted = true;
    bool writeResponseReceived = true;
    int32_t writeReportedBytes = -1;
    bool changeTargetAfterWrite = false;
    bool changeGenerationAfterWrite = false;
    int writeDelayMs = 0;
    Mem::CancellationToken cancelDuringWrite;
    int writeCalls = 0;
    uint64_t lastWriteAddress = 0;
    std::vector<unsigned char> lastWriteBytes;

    bool isConnected() const override {
        return connected;
    }

    bool isConnectionPoisoned() const override {
        return poisoned;
    }

    uint64_t connectionGeneration() const override {
        return generation;
    }

    Mem::TargetSnapshot targetSnapshot() const override {
        Mem::TargetSnapshot snapshot = target;
        snapshot.connectionGeneration = generation;
        return snapshot;
    }

    std::string processName() const override {
        return selectedName;
    }

    bool fetchServerVersion(int& version,
                            std::string& versionString) override {
        version = 101;
        versionString = "1.0.1";
        return connected;
    }

    bool fetchArchitecture(int& type, std::string& name) override {
        type = 3;
        name = "Kernel";
        return connected;
    }

    bool fetchProcesses(std::vector<Mem::ProcessInfo>& output) override {
        if (!fetchProcessesSucceeds) {
            return false;
        }
        output = processes;
        return true;
    }

    bool openProcess(int pid, const std::string& name) override {
        if (!openProcessSucceeds) {
            return false;
        }
        target.pid = pid;
        target.processHandle = pid + 1000;
        target.processRevision += 2;
        target.connectionGeneration = generation;
        selectedName = name;
        if (changeGenerationDuringOpen) {
            ++generation;
        }
        return true;
    }

    bool readMemory(uint64_t,
                    uint32_t,
                    std::vector<unsigned char>& output) override {
        if (!readMemorySucceeds) {
            return false;
        }
        output = memory;
        if (changeTargetAfterRead) {
            target.processRevision += 2;
        }
        if (changeGenerationAfterRead) {
            ++generation;
        }
        return true;
    }

    Mem::MemoryWriteBackendResult writeMemory(
        uint64_t address,
        const std::vector<unsigned char>& bytes) override {
        ++writeCalls;
        lastWriteAddress = address;
        lastWriteBytes = bytes;
        if (writeDelayMs > 0) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(writeDelayMs));
        }
        if (cancelDuringWrite) {
            cancelDuringWrite->store(true, std::memory_order_release);
        }
        if (changeTargetAfterWrite) {
            target.processRevision += 2;
        }
        if (changeGenerationAfterWrite) {
            ++generation;
        }

        Mem::MemoryWriteBackendResult result;
        result.requestStarted = writeRequestStarted;
        result.responseReceived = writeResponseReceived;
        result.writtenBytes = writeReportedBytes >= 0
            ? writeReportedBytes
            : static_cast<int32_t>(bytes.size());
        return result;
    }
};

void testAddressContract() {
    const auto parsed = Mem::parseAddress(" 0x7ff0 ");
    expect(parsed.ok() && parsed.value() == 0x7ff0,
           "explicit hexadecimal address should parse");
    expect(Mem::formatAddress(parsed.value()) == "0x7FF0",
           "address should use canonical uppercase formatting");

    const auto missingPrefix = Mem::parseAddress("7FF0");
    expect(!missingPrefix.ok() &&
               missingPrefix.error().code == Mem::ErrorCode::InvalidArgument,
           "canonical address must require 0x prefix");

    const auto legacy = Mem::parseAddress("7FF0", false);
    expect(legacy.ok() && legacy.value() == 0x7ff0,
           "legacy address mode should remain available to hidden aliases");

    const auto overflow = Mem::parseAddress("0x10000000000000000");
    expect(!overflow.ok(), "uint64 address overflow must fail");
}

void testStatusAndConnectionGeneration() {
    FakeBackend backend;
    Mem::MemService service(backend);

    const auto context = service.captureContext(false);
    const auto response = service.status(context);
    expect(response.ok(), "connected status should succeed");
    expect(response.value().serverVersion == 101,
           "status should include server version");
    expect(response.value().architectureName == "Kernel",
           "status should include architecture");
    expect(response.value().target.pid == 42,
           "status should include target snapshot");

    const auto staleContext = service.captureContext(false);
    ++backend.generation;
    const auto stale = service.status(staleContext);
    expect(!stale.ok() &&
               stale.error().code == Mem::ErrorCode::ConnectionChanged,
           "stale connection generation must be rejected");
}

void testProcessPaginationAndOpen() {
    FakeBackend backend;
    Mem::MemService service(backend);

    Mem::ProcessListRequest request;
    request.filter = "GAME";
    request.limit = 1;
    const auto page = service.listProcesses(service.captureContext(false), request);
    expect(page.ok(), "filtered process list should succeed");
    expect(page.value().total == 2 && page.value().items.size() == 1,
           "process filter and page limit should be applied");
    expect(page.value().nextOffset == 1,
           "truncated process page should expose next offset");

    Mem::ProcessListRequest invalidRequest;
    invalidRequest.limit = Mem::kMaxProcessPageSize + 1;
    const auto invalid = service.listProcesses(
        service.captureContext(false), invalidRequest);
    expect(!invalid.ok() &&
               invalid.error().code == Mem::ErrorCode::InvalidArgument,
           "oversized process page must fail validation");

    Mem::OpenProcessRequest openRequest;
    openRequest.pid = 84;
    const auto opened = service.openProcess(
        service.captureContext(false), openRequest);
    expect(opened.ok() && opened.value().target.pid == 84,
           "process open should return the new target snapshot");
    expect(opened.value().name == "com.example.game.helper",
           "process open result should include the resolved name");
    expect(backend.selectedName == "com.example.game.helper",
           "process open should resolve an omitted process name");

    backend.changeGenerationDuringOpen = true;
    openRequest.pid = 42;
    const auto changed = service.openProcess(
        service.captureContext(false), openRequest);
    expect(!changed.ok() &&
               changed.error().code == Mem::ErrorCode::ConnectionChanged,
           "connection replacement during process open must be rejected");
}

void testMemoryReadTargetValidation() {
    FakeBackend backend;
    Mem::MemService service(backend);

    Mem::MemoryReadRequest request{0x1000, 4};
    const auto response = service.readMemory(
        service.captureContext(true), request);
    expect(response.ok() && response.value().bytes == backend.memory,
           "bounded memory read should return backend bytes");

    backend.changeTargetAfterRead = true;
    const auto targetChanged = service.readMemory(
        service.captureContext(true), request);
    expect(!targetChanged.ok() &&
               targetChanged.error().code == Mem::ErrorCode::TargetChanged,
           "memory bytes must be discarded when target changes in flight");
    backend.changeTargetAfterRead = false;

    backend.changeGenerationAfterRead = true;
    const auto connectionChanged = service.readMemory(
        service.captureContext(true), request);
    expect(!connectionChanged.ok() &&
               connectionChanged.error().code ==
                   Mem::ErrorCode::ConnectionChanged,
           "memory bytes must be discarded when connection changes in flight");
    backend.changeGenerationAfterRead = false;

    backend.target = {};
    const auto noTarget = service.readMemory(
        service.captureContext(true), request);
    expect(!noTarget.ok() &&
               noTarget.error().code == Mem::ErrorCode::NoTarget,
           "memory read without an attached target must fail");

    backend.target = {42, 420, 8, backend.generation};
    request.address = (std::numeric_limits<uint64_t>::max)() - 1;
    request.size = 4;
    const auto overflow = service.readMemory(
        service.captureContext(true), request);
    expect(!overflow.ok() &&
               overflow.error().code == Mem::ErrorCode::InvalidArgument,
           "overflowing memory range must fail validation");

    request = {0x1000, 4};
    Mem::OperationContext cancelled = service.captureContext(true);
    cancelled.cancellation = std::make_shared<std::atomic<bool>>(true);
    const auto cancelledRead = service.readMemory(cancelled, request);
    expect(!cancelledRead.ok() &&
               cancelledRead.error().code == Mem::ErrorCode::CancelRequested,
           "cancelled operation must stop before backend access");

    Mem::OperationContext expired = service.captureContext(true);
    expired.deadline = std::chrono::steady_clock::now();
    const auto expiredRead = service.readMemory(expired, request);
    expect(!expiredRead.ok() &&
               expiredRead.error().code == Mem::ErrorCode::Timeout,
           "expired operation deadline must fail before backend access");
}

void testMemoryWriteCompletionContract() {
    FakeBackend backend;
    Mem::MemService service(backend);
    Mem::MemoryWriteRequest request;
    request.address = 0x2000;
    request.bytes = {0x90, 0x90, 0xC0, 0x03, 0x5F, 0xD6};

    const auto written = service.writeMemory(
        service.captureContext(true), request);
    expect(written.ok() && written.value().writtenBytes == request.bytes.size(),
           "confirmed memory write should return a receipt");
    expect(backend.lastWriteAddress == request.address &&
               backend.lastWriteBytes == request.bytes,
           "memory write should pass the exact address and bytes to backend");

    const int callsAfterSuccess = backend.writeCalls;
    Mem::OperationContext cancelled = service.captureContext(true);
    cancelled.cancellation = std::make_shared<std::atomic<bool>>(true);
    const auto cancelledBeforeSend = service.writeMemory(cancelled, request);
    expect(!cancelledBeforeSend.ok() &&
               cancelledBeforeSend.error().code ==
                   Mem::ErrorCode::CancelRequested &&
               backend.writeCalls == callsAfterSuccess,
           "cancelled write must stop before backend send");

    Mem::OperationContext cancelledDuringSend = service.captureContext(true);
    cancelledDuringSend.cancellation =
        std::make_shared<std::atomic<bool>>(false);
    backend.cancelDuringWrite = cancelledDuringSend.cancellation;
    const auto completedAfterCancel = service.writeMemory(
        cancelledDuringSend, request);
    expect(completedAfterCancel.ok() &&
               completedAfterCancel.value().completedAfterCancelRequest,
           "confirmed write must report completion after a cancel request");
    backend.cancelDuringWrite.reset();

    Mem::OperationContext completedAfterDeadlineContext =
        service.captureContext(true);
    completedAfterDeadlineContext.deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
    backend.writeDelayMs = 40;
    const auto completedAfterDeadline = service.writeMemory(
        completedAfterDeadlineContext, request);
    expect(completedAfterDeadline.ok() &&
               completedAfterDeadline.value().completedAfterDeadline,
           "confirmed write must report completion after its deadline");
    backend.writeDelayMs = 0;

    backend.writeResponseReceived = false;
    backend.writeRequestStarted = true;
    const auto completionUnknown = service.writeMemory(
        service.captureContext(true), request);
    expect(!completionUnknown.ok() &&
               completionUnknown.error().code ==
                   Mem::ErrorCode::CompletionUnknown &&
               !completionUnknown.error().retryable,
           "sent write without a response must be completion_unknown");

    backend.writeRequestStarted = false;
    const auto notSent = service.writeMemory(
        service.captureContext(true), request);
    expect(!notSent.ok() &&
               notSent.error().code == Mem::ErrorCode::ProtocolError &&
               notSent.error().retryable,
           "write known not to be sent may report a retryable transport error");

    backend.writeRequestStarted = true;
    backend.writeResponseReceived = true;
    backend.writeReportedBytes = 2;
    const auto partial = service.writeMemory(
        service.captureContext(true), request);
    expect(!partial.ok() &&
               partial.error().code == Mem::ErrorCode::ProtocolError &&
               !partial.error().retryable,
           "confirmed partial write must fail without automatic retry");

    backend.writeReportedBytes = -1;
    backend.changeTargetAfterWrite = true;
    const auto targetChanged = service.writeMemory(
        service.captureContext(true), request);
    expect(!targetChanged.ok() &&
               targetChanged.error().code ==
                   Mem::ErrorCode::CompletionUnknown,
           "confirmed write must not be bound to a target that changed in flight");
    backend.changeTargetAfterWrite = false;

    backend.changeGenerationAfterWrite = true;
    const auto connectionChanged = service.writeMemory(
        service.captureContext(true), request);
    expect(!connectionChanged.ok() &&
               connectionChanged.error().code ==
                   Mem::ErrorCode::CompletionUnknown,
           "confirmed write on a replaced connection must be completion_unknown");
    backend.changeGenerationAfterWrite = false;

    request.address = (std::numeric_limits<uint64_t>::max)() - 1;
    request.bytes = {1, 2, 3, 4};
    const auto overflow = service.writeMemory(
        service.captureContext(true), request);
    expect(!overflow.ok() &&
               overflow.error().code == Mem::ErrorCode::InvalidArgument,
           "overflowing memory write range must fail validation");
}

void testHiddenToolRegistration() {
    auto& registry = AI::ToolExecutor::getInstance();
    registry.registerTool("test_visible_tool", "visible", "{}",
                          AI::ToolSafety::ReadOnly,
                          [](const std::string&) { return std::string("{}"); });
    registry.registerTool("test_hidden_alias", "hidden", "{}",
                          AI::ToolSafety::ReadOnly,
                          [](const std::string&) { return std::string("{}"); },
                          false);
    registry.registerTool(
        "test_completion_unknown", "completion", "{}",
        AI::ToolSafety::Write,
        [](const std::string&) {
            return std::string(
                R"({"success":false,"error":{"code":"completion_unknown","message":"receipt lost","retryable":false}})");
        });

    const auto definitions = registry.getToolDefinitions();
    bool foundVisible = false;
    bool foundHidden = false;
    for (const auto& definition : definitions) {
        foundVisible = foundVisible || definition.name == "test_visible_tool";
        foundHidden = foundHidden || definition.name == "test_hidden_alias";
    }
    expect(foundVisible, "advertised tool must be returned to providers");
    expect(!foundHidden, "hidden compatibility alias must not be advertised");

    AI::ToolCall call;
    call.name = "test_hidden_alias";
    call.arguments = "{}";
    const AI::ToolResult executed = registry.execute(call);
    expect(executed.success,
           "hidden compatibility alias must remain executable for old sessions");

    call.name = "test_completion_unknown";
    const AI::ToolResult unknown = registry.execute(call);
    expect(!unknown.success &&
               unknown.completion ==
                   AI::ToolCompletionState::CompletionUnknown,
           "structured completion_unknown must survive result normalization");
}

void testDeviceSessionLifecycle() {
    auto& session = DeviceSession::GetInstance();
    {
        auto lifecycle = session.AcquireLifecycle();
        session.Disconnect();
        session.BeginConnect();
        session.FinishConnect(true);
    }

    std::atomic<bool> lifecycleStarted{false};
    std::atomic<bool> lifecycleAcquired{false};
    std::thread lifecycleThread;
    {
        auto request = session.AcquireRequest();
        expect(request && request.isCurrent(),
               "connected session should grant a current request lease");
        auto nestedRequest = session.AcquireRequest();
        expect(nestedRequest && nestedRequest.isCurrent() &&
                   nestedRequest.generation() == request.generation(),
               "nested commands should reuse the active request lease");
        lifecycleThread = std::thread([&] {
            lifecycleStarted.store(true, std::memory_order_release);
            auto lifecycle = session.AcquireLifecycle();
            lifecycleAcquired.store(true, std::memory_order_release);
            session.Disconnect();
        });

        while (!lifecycleStarted.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        expect(!lifecycleAcquired.load(std::memory_order_acquire),
               "exclusive lifecycle transition must wait for request lease");
    }
    lifecycleThread.join();
    expect(lifecycleAcquired.load(std::memory_order_acquire),
           "lifecycle transition should proceed after request lease release");

    {
        auto lifecycle = session.AcquireLifecycle();
        session.BeginConnect();
        session.FinishConnect(true);
    }
    {
        auto request = session.AcquireRequest();
        const uint64_t generationBeforePoison = session.GetGeneration();
        session.MarkPoisoned();
        expect(session.IsPoisoned(), "I/O failure should poison the session");
        expect(session.GetGeneration() == generationBeforePoison + 1,
               "first poison event should invalidate connection generation");
        expect(!request.isCurrent(),
               "poisoned generation should invalidate an in-flight request lease");
    }
    {
        auto rejected = session.AcquireRequest();
        expect(!rejected, "poisoned session must reject new requests");
    }
    {
        auto lifecycle = session.AcquireLifecycle();
        session.Disconnect();
    }
}

void testAgentTaskExecutorLifecycle() {
    auto& registry = AI::ToolExecutor::getInstance();
    std::mutex stateMutex;
    std::condition_variable stateCv;
    std::vector<AI::AgentToolTaskOutcome> outcomes;
    bool blockerStarted = false;
    bool releaseBlocker = false;
    bool cancellableStarted = false;
    bool cancellableFinished = false;
    int immediateExecutions = 0;
    std::thread::id executorThread;
    std::thread::id callbackThread;

    registry.registerTool(
        "test_task_immediate", "immediate", "{}",
        AI::ToolSafety::ReadOnly,
        [&](const std::string&) {
            ++immediateExecutions;
            executorThread = std::this_thread::get_id();
            return std::string(R"({"success":true})");
        });
    registry.registerTool(
        "test_task_blocker", "blocker", "{}",
        AI::ToolSafety::ReadOnly,
        [&](const std::string&, const Mem::OperationContext& context) {
            std::unique_lock<std::mutex> lock(stateMutex);
            blockerStarted = true;
            stateCv.notify_all();
            while (!releaseBlocker &&
                   !(context.cancellation &&
                     context.cancellation->load(std::memory_order_acquire))) {
                stateCv.wait_for(lock, std::chrono::milliseconds(5));
            }
            return std::string(R"({"success":true})");
        },
        AI::ToolTargetPolicy::None);
    registry.registerTool(
        "test_task_cancellable", "cancellable", "{}",
        AI::ToolSafety::ReadOnly,
        [&](const std::string&, const Mem::OperationContext& context) {
            {
                std::lock_guard<std::mutex> lock(stateMutex);
                cancellableStarted = true;
                stateCv.notify_all();
            }
            while (!(context.cancellation &&
                     context.cancellation->load(std::memory_order_acquire))) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            {
                std::lock_guard<std::mutex> lock(stateMutex);
                cancellableFinished = true;
                stateCv.notify_all();
            }
            return std::string(
                R"({"success":false,"error":{"code":"cancel_requested","message":"cancel observed","retryable":false}})");
        },
        AI::ToolTargetPolicy::None);
    registry.registerTool(
        "test_task_slow_read", "slow read", "{}",
        AI::ToolSafety::ReadOnly,
        [&](const std::string&) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            return std::string(R"({"success":true,"value":1})");
        });

    AI::AgentTaskExecutor executor(registry);
    const auto completion = [&](AI::AgentToolTaskOutcome outcome) {
        std::lock_guard<std::mutex> lock(stateMutex);
        callbackThread = std::this_thread::get_id();
        outcomes.push_back(std::move(outcome));
        stateCv.notify_all();
    };
    const auto waitFor = [&](const std::function<bool()>& predicate,
                             const char* message) {
        std::unique_lock<std::mutex> lock(stateMutex);
        if (!stateCv.wait_for(lock, std::chrono::seconds(2), predicate)) {
            throw std::runtime_error(message);
        }
    };
    const auto completionFor = [&](const std::string& runId) {
        std::lock_guard<std::mutex> lock(stateMutex);
        for (const auto& outcome : outcomes) {
            if (outcome.runId == runId) {
                return outcome.result.completion;
            }
        }
        throw std::runtime_error("missing task outcome for " + runId);
    };
    const auto enqueue = [&](const std::string& runId,
                             const std::string& tool,
                             Mem::OperationContext context = {}) {
        AI::AgentToolTask task;
        task.runId = runId;
        task.call = toolCall(runId + "-call", tool);
        task.context = std::move(context);
        return executor.enqueue(std::move(task), completion);
    };

    expect(enqueue("task-basic", "test_task_immediate"),
           "owned task executor should accept work");
    waitFor([&] { return outcomes.size() >= 1; },
            "basic task did not complete");
    expect(immediateExecutions == 1 && executorThread == callbackThread &&
               executorThread != std::this_thread::get_id(),
           "tool and callback should run on the same owned worker thread");

    {
        std::lock_guard<std::mutex> lock(stateMutex);
        blockerStarted = false;
        releaseBlocker = false;
    }
    expect(enqueue("task-block-timeout", "test_task_blocker"),
           "blocker should enqueue for queue-timeout test");
    waitFor([&] { return blockerStarted; }, "blocker did not start");
    Mem::OperationContext shortDeadline;
    shortDeadline.deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(30);
    expect(enqueue("task-queued-timeout", "test_task_immediate",
                   shortDeadline),
           "queued timeout task should enqueue");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        releaseBlocker = true;
        stateCv.notify_all();
    }
    waitFor([&] { return outcomes.size() >= 3; },
            "queue-timeout outcomes did not complete");
    expect(completionFor("task-queued-timeout") ==
               AI::ToolCompletionState::TimedOutBeforeStart &&
               immediateExecutions == 1,
           "expired queued task must not invoke its tool executor");

    {
        std::lock_guard<std::mutex> lock(stateMutex);
        blockerStarted = false;
        releaseBlocker = false;
    }
    expect(enqueue("task-block-cancel", "test_task_blocker"),
           "blocker should enqueue for cancellation test");
    waitFor([&] { return blockerStarted; },
            "cancellation blocker did not start");
    expect(enqueue("task-queued-cancel", "test_task_immediate"),
           "queued cancellation task should enqueue");
    executor.cancelRun("task-queued-cancel");
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        releaseBlocker = true;
        stateCv.notify_all();
    }
    waitFor([&] { return outcomes.size() >= 5; },
            "queued cancellation outcomes did not complete");
    expect(completionFor("task-queued-cancel") ==
               AI::ToolCompletionState::CancelledBeforeStart &&
               immediateExecutions == 1,
           "cancelled queued task must not invoke its tool executor");

    {
        std::lock_guard<std::mutex> lock(stateMutex);
        cancellableStarted = false;
        cancellableFinished = false;
    }
    expect(enqueue("task-active-cancel", "test_task_cancellable"),
           "active cancellation task should enqueue");
    waitFor([&] { return cancellableStarted; },
            "cancellable task did not start");
    executor.cancelRun("task-active-cancel");
    waitFor([&] { return outcomes.size() >= 6; },
            "active cancellation outcome did not complete");
    expect(cancellableFinished &&
               completionFor("task-active-cancel") ==
                   AI::ToolCompletionState::CancelRequested,
           "active task should observe cancellation without being detached");

    Mem::OperationContext slowDeadline;
    slowDeadline.deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    expect(enqueue("task-slow-timeout", "test_task_slow_read", slowDeadline),
           "slow read should enqueue");
    waitFor([&] { return outcomes.size() >= 7; },
            "slow timeout outcome did not complete");
    expect(completionFor("task-slow-timeout") ==
               AI::ToolCompletionState::TimedOut,
           "late read must finish on the owned worker and report timed_out");

    {
        std::lock_guard<std::mutex> lock(stateMutex);
        cancellableStarted = false;
        cancellableFinished = false;
    }
    expect(enqueue("task-shutdown", "test_task_cancellable"),
           "shutdown task should enqueue");
    waitFor([&] { return cancellableStarted; },
            "shutdown task did not start");
    expect(enqueue("task-shutdown-queued", "test_task_immediate"),
           "queued shutdown task should enqueue");
    executor.shutdown();
    expect(cancellableFinished && !executor.isAccepting() &&
               executor.pendingCount() == 0 &&
               completionFor("task-shutdown") ==
                   AI::ToolCompletionState::CancelRequested &&
               completionFor("task-shutdown-queued") ==
                   AI::ToolCompletionState::CancelledBeforeStart &&
               immediateExecutions == 1,
           "shutdown must cancel and join the active worker");
    expect(!enqueue("task-after-shutdown", "test_task_immediate"),
           "executor must reject work after shutdown");
}

void testAgentAdapter() {
    FakeBackend backend;
    Mem::MemService service(backend);
    AI::AgentMemTools tools(service);
    const Mem::OperationContext connectionContext =
        service.captureContext(false);
    const Mem::OperationContext targetContext = service.captureContext(true);

    const json status = json::parse(tools.status("{}", connectionContext));
    expect(status.at("success").get<bool>() &&
               status.at("connection_generation").get<uint64_t>() == 1,
           "status adapter should expose structured success and generation");

    const json processes = json::parse(
        tools.processList(
            R"({"filter":"game","count":1})", connectionContext));
    expect(processes.at("success").get<bool>() &&
               processes.at("truncated").get<bool>(),
           "process adapter should expose pagination metadata");

    const json strictAddress = json::parse(
        tools.memoryRead(
            R"({"address":"1000","size":4})", false, targetContext));
    expect(!strictAddress.at("success").get<bool>() &&
               strictAddress.at("error").at("code") == "invalid_argument",
           "canonical memory_read should reject missing 0x prefix");

    const json read = json::parse(
        tools.memoryRead(
            R"({"address":"0x1000","size":4})", false, targetContext));
    expect(read.at("success").get<bool>() &&
               read.at("hex") == "DEADBEEF" &&
               read.at("data") == "DE AD BE EF",
           "memory adapter should expose compact and spaced hex");

    const json legacyRead = json::parse(
        tools.memoryRead(
            R"({"address":4096,"size":4})", true, targetContext));
    expect(legacyRead.at("success").get<bool>(),
           "hidden legacy memory alias should accept integer addresses");

    const json strictWriteAddress = json::parse(
        tools.memoryWrite(
            R"({"address":"2000","data_hex":"90 90"})",
            false,
            targetContext));
    expect(!strictWriteAddress.at("success").get<bool>() &&
               strictWriteAddress.at("error").at("code") ==
                   "invalid_argument",
           "canonical memory_write should require a 0x-prefixed address");

    const json write = json::parse(
        tools.memoryWrite(
            R"({"address":"0x2000","data_hex":"90 90"})",
            false,
            targetContext));
    expect(write.at("success").get<bool>() &&
               write.at("written_bytes") == 2 &&
               write.at("completion") == "completed",
           "canonical memory_write should expose a confirmed write receipt");

    const json legacyWrite = json::parse(
        tools.memoryWrite(
            R"({"address":8192,"hex_string":"C0 03 5F D6"})",
            true,
            targetContext));
    expect(legacyWrite.at("success").get<bool>() &&
               legacyWrite.at("written_bytes") == 4,
           "hidden write_bytes alias should retain legacy argument forms");

    backend.connected = false;
    const json disconnected = json::parse(
        tools.processList("{}", connectionContext));
    expect(!disconnected.at("success").get<bool>() &&
               disconnected.at("error").at("code") == "not_connected",
           "adapter should preserve structured service errors");
}

AI::ToolCall toolCall(std::string id,
                      std::string name,
                      std::string arguments) {
    AI::ToolCall call;
    call.id = std::move(id);
    call.name = std::move(name);
    call.arguments = std::move(arguments);
    return call;
}

void registerContextTestTools(AI::AgentMemTools& tools) {
    auto& registry = AI::ToolExecutor::getInstance();
    registry.registerTool(
        "test_context_status", "status", "{}", AI::ToolSafety::ReadOnly,
        [&tools](const std::string& args,
                 const Mem::OperationContext& context) {
            return tools.status(args, context);
        },
        AI::ToolTargetPolicy::None);
    registry.registerTool(
        "test_context_process_list", "process list", "{}",
        AI::ToolSafety::ReadOnly,
        [&tools](const std::string& args,
                 const Mem::OperationContext& context) {
            return tools.processList(args, context);
        },
        AI::ToolTargetPolicy::None);
    registry.registerTool(
        "test_context_process_open", "process open", "{}",
        AI::ToolSafety::Write,
        [&tools](const std::string& args,
                 const Mem::OperationContext& context) {
            return tools.processOpen(args, context);
        },
        AI::ToolTargetPolicy::Selection);
    registry.registerTool(
        "test_context_memory_read", "memory read", "{}",
        AI::ToolSafety::ReadOnly,
        [&tools](const std::string& args,
                 const Mem::OperationContext& context) {
            return tools.memoryRead(args, false, context);
        },
        AI::ToolTargetPolicy::Bound);
    registry.registerTool(
        "test_context_memory_write", "memory write", "{}",
        AI::ToolSafety::Write,
        [&tools](const std::string& args,
                 const Mem::OperationContext& context) {
            return tools.memoryWrite(args, false, context);
        },
        AI::ToolTargetPolicy::Bound);
}

bool outcomeContains(const AI::AgentRunner::Outcome& outcome,
                     const std::string& text) {
    for (const auto& message : outcome.messages) {
        if (message.content.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

void testApprovalContextInvalidation() {
    AI::AgentController::ToolConfig config;

    {
        FakeBackend backend;
        Mem::MemService service(backend);
        AI::AgentMemTools tools(service);
        registerContextTestTools(tools);
        AI::AgentController controller(service);
        controller.resetForNewRun();

        const AI::ToolCall open = toolCall(
            "open-target-switch", "test_context_process_open", R"({"pid":84})");
        auto waiting = controller.beginToolCalls({open}, config);
        expect(waiting.kind == AI::AgentRunner::OutcomeKind::NeedsConfirmation,
               "process open should wait for approval");

        backend.target.processRevision += 2;
        auto rejected = controller.approvePendingTool(config);
        expect(rejected.kind == AI::AgentRunner::OutcomeKind::ReadyForFollowUp &&
                   outcomeContains(rejected, "target_changed"),
               "target switch while awaiting approval must reject execution");
    }

    {
        FakeBackend backend;
        Mem::MemService service(backend);
        AI::AgentMemTools tools(service);
        registerContextTestTools(tools);
        AI::AgentController controller(service);
        controller.resetForNewRun();

        const AI::ToolCall open = toolCall(
            "open-reconnect", "test_context_process_open", R"({"pid":84})");
        auto waiting = controller.beginToolCalls({open}, config);
        expect(waiting.kind == AI::AgentRunner::OutcomeKind::NeedsConfirmation,
               "process open should wait for approval before reconnect test");

        ++backend.generation;
        auto rejected = controller.approvePendingTool(config);
        expect(rejected.kind == AI::AgentRunner::OutcomeKind::ReadyForFollowUp &&
                   outcomeContains(rejected, "connection_changed"),
               "reconnect while awaiting approval must reject execution");
    }

    {
        FakeBackend backend;
        Mem::MemService service(backend);
        AI::AgentMemTools tools(service);
        registerContextTestTools(tools);
        AI::AgentController controller(service);
        controller.resetForNewRun();

        const AI::ToolCall open = toolCall(
            "open-before-send", "test_context_process_open", R"({"pid":84})");
        auto waiting = controller.beginToolCalls({open}, config);
        expect(waiting.kind == AI::AgentRunner::OutcomeKind::NeedsConfirmation,
               "process open should wait for approval before send-time test");
        auto approved = controller.approvePendingTool(config);
        expect(approved.kind == AI::AgentRunner::OutcomeKind::NeedsExecution,
               "stable approved target should be released for execution");

        backend.target.processRevision += 2;
        const AI::ToolResult rejected =
            AI::ToolExecutor::getInstance().execute(
                open, controller.operationContext());
        expect(!rejected.success &&
                   rejected.resultJson.find("target_changed") !=
                       std::string::npos,
               "target switch after approval but before send must be rejected");
    }

    {
        FakeBackend backend;
        Mem::MemService service(backend);
        AI::AgentMemTools tools(service);
        registerContextTestTools(tools);
        AI::AgentController controller(service);
        controller.resetForNewRun();

        const AI::ToolCall write = toolCall(
            "write-before-send",
            "test_context_memory_write",
            R"({"address":"0x2000","data_hex":"90 90"})");
        auto waiting = controller.beginToolCalls({write}, config);
        expect(waiting.kind == AI::AgentRunner::OutcomeKind::NeedsConfirmation,
               "memory write should wait for approval");
        auto approved = controller.approvePendingTool(config);
        expect(approved.kind == AI::AgentRunner::OutcomeKind::NeedsExecution,
               "approved memory write should be released while target is current");

        backend.target.processRevision += 2;
        const AI::ToolResult rejected =
            AI::ToolExecutor::getInstance().execute(
                write, controller.operationContext());
        expect(!rejected.success && backend.writeCalls == 0 &&
                   rejected.resultJson.find("target_changed") !=
                       std::string::npos,
               "memory write must revalidate target before backend send");
    }
}

void testProcessOpenAdvancesRunTarget() {
    FakeBackend backend;
    Mem::MemService service(backend);
    AI::AgentMemTools tools(service);
    registerContextTestTools(tools);
    AI::AgentController controller(service);
    AI::AgentController::ToolConfig config;
    controller.resetForNewRun();

    const AI::ToolCall open = toolCall(
        "open-batch", "test_context_process_open", R"({"pid":84})");
    const AI::ToolCall read = toolCall(
        "read-batch", "test_context_memory_read",
        R"({"address":"0x1000","size":4})");

    auto waiting = controller.beginToolCalls({open, read}, config);
    expect(waiting.kind == AI::AgentRunner::OutcomeKind::NeedsConfirmation,
           "target selection should block the tool batch for approval");

    auto openReady = controller.approvePendingTool(config);
    expect(openReady.kind == AI::AgentRunner::OutcomeKind::NeedsExecution &&
               openReady.toolCallToExecute &&
               openReady.toolCallToExecute->name == open.name,
           "approved process open should be released for execution");

    AI::ToolResult openResult = AI::ToolExecutor::getInstance().execute(
        open, controller.operationContext());
    expect(openResult.success && openResult.selectedTarget &&
               openResult.selectedTarget->pid == 84,
           "process open should return a structured selected target");

    auto readReady = controller.completeToolExecution(
        open, openResult, 1, config);
    expect(readReady.kind == AI::AgentRunner::OutcomeKind::NeedsExecution &&
               readReady.toolCallToExecute &&
               readReady.toolCallToExecute->name == read.name,
           "successful process open should release the next bound tool");
    expect(controller.snapshot().context.operation.target &&
               controller.snapshot().context.operation.target->pid == 84,
           "process open must explicitly advance the run target snapshot");

    AI::ToolResult readResult = AI::ToolExecutor::getInstance().execute(
        read, controller.operationContext());
    expect(readResult.success,
           "memory read should use the target selected earlier in the batch");
    auto complete = controller.completeToolExecution(
        read, readResult, 1, config);
    expect(complete.kind == AI::AgentRunner::OutcomeKind::ReadyForFollowUp,
           "successful open/read batch should complete normally");
}

void testNonTargetToolsAndStaleResult() {
    AI::AgentController::ToolConfig config;

    {
        FakeBackend backend;
        Mem::MemService service(backend);
        AI::AgentMemTools tools(service);
        registerContextTestTools(tools);
        AI::AgentController controller(service);
        controller.resetForNewRun();

        backend.target.processRevision += 2;
        const AI::ToolCall status =
            toolCall("status-non-target", "test_context_status");
        const AI::ToolCall list =
            toolCall("list-non-target", "test_context_process_list");
        auto statusReady = controller.beginToolCalls({status, list}, config);
        expect(statusReady.kind == AI::AgentRunner::OutcomeKind::NeedsExecution &&
                   statusReady.toolCallToExecute &&
                   statusReady.toolCallToExecute->name == status.name,
               "non-target status should ignore a target-only change");

        const AI::ToolResult statusResult =
            AI::ToolExecutor::getInstance().execute(
                status, controller.operationContext());
        expect(statusResult.success, "status should execute on the run generation");
        auto listReady = controller.completeToolExecution(
            status, statusResult, 1, config);
        expect(listReady.kind == AI::AgentRunner::OutcomeKind::NeedsExecution &&
                   listReady.toolCallToExecute &&
                   listReady.toolCallToExecute->name == list.name,
               "process list should follow status despite target change");

        const AI::ToolResult listResult =
            AI::ToolExecutor::getInstance().execute(
                list, controller.operationContext());
        expect(listResult.success, "process list should remain connection-bound only");
        auto complete = controller.completeToolExecution(
            list, listResult, 1, config);
        expect(complete.kind == AI::AgentRunner::OutcomeKind::ReadyForFollowUp,
               "non-target tool batch should complete normally");
    }

    {
        FakeBackend backend;
        Mem::MemService service(backend);
        AI::AgentMemTools tools(service);
        registerContextTestTools(tools);
        AI::AgentController controller(service);
        controller.resetForNewRun();

        const AI::ToolCall read = toolCall(
            "read-stale", "test_context_memory_read",
            R"({"address":"0x1000","size":4})");
        auto ready = controller.beginToolCalls({read}, config);
        expect(ready.kind == AI::AgentRunner::OutcomeKind::NeedsExecution,
               "bound read should be released while its target is current");

        const AI::ToolResult successfulRead =
            AI::ToolExecutor::getInstance().execute(
                read, controller.operationContext());
        expect(successfulRead.success,
               "read should initially complete against the expected target");
        backend.target.processRevision += 2;

        auto rejected = controller.completeToolExecution(
            read, successfulRead, 1, config);
        expect(rejected.kind == AI::AgentRunner::OutcomeKind::ReadyForFollowUp &&
                   outcomeContains(rejected, "target_changed") &&
                   outcomeContains(rejected, "stale_result_was_success"),
               "late success must be rejected but retained for audit after target change");
    }
}

} // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"address contract", &testAddressContract},
        {"status and generation", &testStatusAndConnectionGeneration},
        {"process pagination and open", &testProcessPaginationAndOpen},
        {"memory target validation", &testMemoryReadTargetValidation},
        {"memory write completion contract", &testMemoryWriteCompletionContract},
        {"agent adapter", &testAgentAdapter},
        {"hidden tool registration", &testHiddenToolRegistration},
        {"device session lifecycle", &testDeviceSessionLifecycle},
        {"agent task executor lifecycle", &testAgentTaskExecutorLifecycle},
        {"approval context invalidation", &testApprovalContextInvalidation},
        {"process open advances run target", &testProcessOpenAdvancesRunTarget},
        {"non-target and stale result handling", &testNonTargetToolsAndStaleResult},
    };

    int failed = 0;
    for (const auto& test : tests) {
        try {
            test.second();
            std::cout << "[PASS] " << test.first << '\n';
        } catch (const std::exception& error) {
            ++failed;
            std::cerr << "[FAIL] " << test.first << ": " << error.what()
                      << '\n';
        }
    }

    if (failed != 0) {
        std::cerr << failed << " test group(s) failed\n";
        return 1;
    }
    std::cout << tests.size() << " test groups passed\n";
    return 0;
}
