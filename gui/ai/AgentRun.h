#pragma once
#ifdef HAVE_AI_CHAT

#include "AIProvider.h"
#include "AgentRunContext.h"
#include "AgentTrace.h"

#include <optional>
#include <string>
#include <vector>

namespace AI {

enum class AgentRunState {
    Idle,
    WaitingModel,
    ExecutingTools,
    WaitingApproval,
    Completed,
    Failed,
    Cancelled
};

enum class AgentApprovalDecision {
    Pending,
    Approved,
    Denied
};

inline const char* agentRunStateLabel(AgentRunState state) {
    switch (state) {
        case AgentRunState::Idle:            return "idle";
        case AgentRunState::WaitingModel:    return "waiting model";
        case AgentRunState::ExecutingTools:  return "executing tools";
        case AgentRunState::WaitingApproval: return "waiting approval";
        case AgentRunState::Completed:       return "completed";
        case AgentRunState::Failed:          return "failed";
        case AgentRunState::Cancelled:       return "cancelled";
        default:                             return "unknown";
    }
}

inline const char* agentApprovalDecisionLabel(AgentApprovalDecision decision) {
    switch (decision) {
        case AgentApprovalDecision::Pending:  return "pending";
        case AgentApprovalDecision::Approved: return "approved";
        case AgentApprovalDecision::Denied:   return "denied";
        default:                              return "unknown";
    }
}

struct AgentRun {
    std::string id;
    AgentRunState state = AgentRunState::Idle;
    int modelTurns = 0;
    int toolSteps = 0;
    std::vector<AgentTraceEvent> trace;
    std::optional<ToolCall> pendingApproval;
    AgentApprovalDecision approvalDecision = AgentApprovalDecision::Pending;
    AgentRunContext context;
};

struct AgentRunSnapshot {
    std::string id;
    AgentRunState state = AgentRunState::Idle;
    int modelTurns = 0;
    int toolSteps = 0;
    std::vector<AgentTraceEvent> trace;
    std::optional<ToolCall> pendingApproval;
    AgentApprovalDecision approvalDecision = AgentApprovalDecision::Pending;
    AgentRunContext context;
};

} // namespace AI

#endif // HAVE_AI_CHAT
