#pragma once
#ifdef HAVE_AI_CHAT

#include "AgentMutationAudit.h"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace AI {

struct AgentToolTask {
    std::string runId;
    ToolCall call;
    Mem::OperationContext context;
    ToolSafety safety = ToolSafety::ReadOnly;
    ToolTargetPolicy targetPolicy = ToolTargetPolicy::None;
    MutationApproval approval = MutationApproval::NotRequired;
};

struct AgentToolTaskOutcome {
    std::string runId;
    ToolCall call;
    ToolResult result;
    Mem::OperationContext context;
    ToolSafety safety = ToolSafety::ReadOnly;
    ToolTargetPolicy targetPolicy = ToolTargetPolicy::None;
    MutationApproval approval = MutationApproval::NotRequired;
    long long durationMs = 0;
    bool auditPersisted = true;
    std::string auditError;
};

class AgentTaskExecutor {
public:
    using CompletionCallback =
        std::function<void(AgentToolTaskOutcome outcome)>;

    static AgentTaskExecutor& getInstance();

    AgentTaskExecutor();
    explicit AgentTaskExecutor(ToolExecutor& toolExecutor);
    AgentTaskExecutor(ToolExecutor& toolExecutor,
                      AgentMutationAuditLog* auditLog);
    ~AgentTaskExecutor();

    AgentTaskExecutor(const AgentTaskExecutor&) = delete;
    AgentTaskExecutor& operator=(const AgentTaskExecutor&) = delete;

    bool enqueue(AgentToolTask task, CompletionCallback completion);
    void cancelRun(const std::string& runId);

    // Stops accepting work, marks queued/active tasks cancelled, and joins
    // the owned worker. Idempotent; returning proves the worker has exited.
    void shutdown();

    bool isAccepting() const;
    size_t pendingCount() const;

private:
    struct QueuedTask {
        AgentToolTask task;
        CompletionCallback completion;
    };

    static constexpr size_t kMaxQueuedTasks = 64;

    void workerLoop();
    void deliver(QueuedTask task,
                 ToolResult result,
                 long long durationMs);

    ToolExecutor& toolExecutor_;
    AgentMutationAuditLog* auditLog_ = nullptr;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<QueuedTask> queue_;
    std::thread worker_;
    std::string activeRunId_;
    Mem::CancellationToken activeCancellation_;
    bool accepting_ = true;
    bool stopping_ = false;
};

} // namespace AI

#endif // HAVE_AI_CHAT
