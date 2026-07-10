#pragma once
#ifdef HAVE_AI_CHAT

#include "ToolExecutor.h"

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
};

struct AgentToolTaskOutcome {
    std::string runId;
    ToolCall call;
    ToolResult result;
    long long durationMs = 0;
};

class AgentTaskExecutor {
public:
    using CompletionCallback =
        std::function<void(AgentToolTaskOutcome outcome)>;

    static AgentTaskExecutor& getInstance();

    AgentTaskExecutor();
    explicit AgentTaskExecutor(ToolExecutor& toolExecutor);
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
    static void deliver(QueuedTask task,
                        ToolResult result,
                        long long durationMs);

    ToolExecutor& toolExecutor_;
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
