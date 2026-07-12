#ifdef HAVE_AI_CHAT

#include "AgentTaskExecutor.h"
#include "AgentMutationAudit.h"

#include "../../mem/MemResult.h"
#include "../../third_party/nlohmann/json.hpp"

#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <utility>
#include <vector>

namespace AI {

namespace {

using Clock = std::chrono::steady_clock;

long long elapsedMilliseconds(Clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               Clock::now() - start)
        .count();
}

ToolResult taskFailure(Mem::ErrorCode code,
                       std::string message,
                       bool retryable,
                       ToolCompletionState completion) {
    nlohmann::json output;
    output["success"] = false;
    output["error"] = {
        {"code", Mem::errorCodeName(code)},
        {"message", message},
        {"retryable", retryable},
    };
    output["completion"] = toolCompletionStateName(completion);

    ToolResult result;
    result.success = false;
    result.resultJson = output.dump();
    result.errorMessage = std::move(message);
    result.completion = completion;
    return result;
}

} // namespace

AgentTaskExecutor& AgentTaskExecutor::getInstance() {
    static AgentTaskExecutor instance;
    return instance;
}

AgentTaskExecutor::AgentTaskExecutor()
    : AgentTaskExecutor(ToolExecutor::getInstance(),
                        &AgentMutationAuditLog::getInstance()) {}

AgentTaskExecutor::AgentTaskExecutor(ToolExecutor& toolExecutor)
    : AgentTaskExecutor(toolExecutor, nullptr) {}

AgentTaskExecutor::AgentTaskExecutor(ToolExecutor& toolExecutor,
                                     AgentMutationAuditLog* auditLog)
    : toolExecutor_(toolExecutor), auditLog_(auditLog) {
    worker_ = std::thread([this] { workerLoop(); });
}

AgentTaskExecutor::~AgentTaskExecutor() {
    shutdown();
}

bool AgentTaskExecutor::enqueue(AgentToolTask task,
                                CompletionCallback completion) {
    if (task.runId.empty() || task.call.name.empty()) {
        return false;
    }
    if (!task.context.cancellation) {
        task.context.cancellation =
            std::make_shared<std::atomic<bool>>(false);
    }
    task.safety = toolExecutor_.getToolSafety(task.call.name);
    task.targetPolicy =
        toolExecutor_.getToolTargetPolicy(task.call.name);
    const auto executorDeadline =
        Clock::now() +
        std::chrono::seconds(toolExecutor_.getExecutionTimeout());
    if (task.context.deadline > executorDeadline) {
        task.context.deadline = executorDeadline;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!accepting_ || queue_.size() >= kMaxQueuedTasks) {
            return false;
        }
        queue_.push_back(
            QueuedTask{std::move(task), std::move(completion)});
    }
    cv_.notify_one();
    return true;
}

void AgentTaskExecutor::cancelRun(const std::string& runId) {
    if (runId.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (activeRunId_ == runId && activeCancellation_) {
        activeCancellation_->store(true, std::memory_order_release);
    }
    for (auto& queued : queue_) {
        if (queued.task.runId == runId && queued.task.context.cancellation) {
            queued.task.context.cancellation->store(
                true, std::memory_order_release);
        }
    }
}

void AgentTaskExecutor::shutdown() {
    std::vector<QueuedTask> cancelled;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!accepting_ && stopping_ && !worker_.joinable()) {
            return;
        }
        accepting_ = false;
        stopping_ = true;
        if (activeCancellation_) {
            activeCancellation_->store(true, std::memory_order_release);
        }
        cancelled.reserve(queue_.size());
        while (!queue_.empty()) {
            QueuedTask task = std::move(queue_.front());
            queue_.pop_front();
            if (task.task.context.cancellation) {
                task.task.context.cancellation->store(
                    true, std::memory_order_release);
            }
            cancelled.push_back(std::move(task));
        }
    }

    for (auto& task : cancelled) {
        deliver(
            std::move(task),
            taskFailure(Mem::ErrorCode::CancelRequested,
                        "tool task was cancelled before start during shutdown",
                        false,
                        ToolCompletionState::CancelledBeforeStart),
            0);
    }

    cv_.notify_all();
    if (worker_.joinable() &&
        worker_.get_id() != std::this_thread::get_id()) {
        worker_.join();
    }
}

bool AgentTaskExecutor::isAccepting() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return accepting_;
}

size_t AgentTaskExecutor::pendingCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size() + (activeRunId_.empty() ? 0u : 1u);
}

void AgentTaskExecutor::workerLoop() {
    while (true) {
        QueuedTask queued;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] {
                return stopping_ || !queue_.empty();
            });
            if (stopping_ && queue_.empty()) {
                return;
            }

            queued = std::move(queue_.front());
            queue_.pop_front();
            activeRunId_ = queued.task.runId;
            activeCancellation_ = queued.task.context.cancellation;
        }

        const auto start = Clock::now();
        ToolResult result;
        if (queued.task.context.cancellation &&
            queued.task.context.cancellation->load(
                std::memory_order_acquire)) {
            result = taskFailure(
                Mem::ErrorCode::CancelRequested,
                "tool task was cancelled before execution",
                false,
                ToolCompletionState::CancelledBeforeStart);
        } else if (Clock::now() >= queued.task.context.deadline) {
            result = taskFailure(
                Mem::ErrorCode::Timeout,
                "tool task deadline expired while waiting in the queue",
                true,
                ToolCompletionState::TimedOutBeforeStart);
        } else {
            result = toolExecutor_.execute(
                queued.task.call, queued.task.context);
            if (queued.task.context.cancellation &&
                queued.task.context.cancellation->load(
                    std::memory_order_acquire)) {
                if (result.success) {
                    result.completion =
                        ToolCompletionState::CompletedAfterCancelRequest;
                } else if (result.completion !=
                           ToolCompletionState::CompletionUnknown) {
                    result.completion = ToolCompletionState::CancelRequested;
                }
            }
        }
        const long long durationMs = elapsedMilliseconds(start);

        deliver(std::move(queued), std::move(result), durationMs);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            activeRunId_.clear();
            activeCancellation_.reset();
        }
    }
}

void AgentTaskExecutor::deliver(QueuedTask task,
                                ToolResult result,
                                long long durationMs) {
    AgentToolTaskOutcome outcome;
    outcome.runId = std::move(task.task.runId);
    outcome.call = std::move(task.task.call);
    outcome.result = std::move(result);
    outcome.context = std::move(task.task.context);
    outcome.safety = task.task.safety;
    outcome.targetPolicy = task.task.targetPolicy;
    outcome.approval = task.task.approval;
    outcome.durationMs = durationMs;

    if (auditLog_ && shouldAuditMutationOutcome(
                         outcome.safety, outcome.call.name)) {
        AgentMutationAuditEvent event;
        event.runId = outcome.runId;
        event.call = outcome.call;
        event.result = outcome.result;
        event.context = outcome.context;
        event.safety = outcome.safety;
        event.targetPolicy = outcome.targetPolicy;
        event.approval = outcome.approval;
        event.durationMs = outcome.durationMs;
        outcome.auditPersisted =
            auditLog_->append(event, &outcome.auditError);
    }

    if (!task.completion) {
        return;
    }
    try {
        task.completion(std::move(outcome));
    } catch (...) {
        // Completion callbacks cross a worker boundary and must not terminate
        // the owned worker. UI delivery failures are handled by run-id gates.
    }
}

} // namespace AI

#endif // HAVE_AI_CHAT
