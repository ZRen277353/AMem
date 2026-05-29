#pragma once
#ifdef HAVE_AI_CHAT

#include <string>

namespace AI {

enum class AgentTraceType {
    Started,
    ModelRequestDispatched,
    CallLimitTruncated,
    StepLimitReached,
    AwaitingApproval,
    Approved,
    Denied,
    AutoApproved,
    ToolStarted,
    ToolSucceeded,
    ToolFailed,
    ToolSkipped,
    ToolBatchComplete,
    FollowUpRequested,
    Completed,
    Cancelled,
    ProviderError
};

struct AgentTraceEvent {
    AgentTraceType type = AgentTraceType::Started;
    long long timestamp = 0;
    int step = 0;
    int index = 0;
    int total = 0;
    std::string tool;
    std::string detail;
    long long durationMs = 0;
};

} // namespace AI

#endif // HAVE_AI_CHAT
