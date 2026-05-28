#pragma once
#ifdef HAVE_AI_CHAT

#include "AIProvider.h"
#include "AgentRun.h"
#include "AgentRunner.h"
#include "AgentTrace.h"

#include <atomic>
#include <optional>
#include <string>
#include <vector>

namespace AI {

// Owns model-request dispatch for an agent run. ChatWindow still owns the UI
// and message persistence, while AgentController centralizes provider lookup,
// request construction, tool exposure, and async dispatch.
class AgentController {
public:
    using RunState = AgentRunState;
    using ToolConfig = AgentRunner::Config;
    using ToolOutcome = AgentRunner::Outcome;
    using ToolOutcomeKind = AgentRunner::OutcomeKind;

    struct ModelRequest {
        std::string providerName;
        std::string modelOverride;
        std::string failureDetail;
        std::vector<ChatMessage> messages;
        bool stream = true;
    };

    struct DispatchResult {
        bool dispatched = false;
        std::string providerName;
        std::string model;
        std::string error;
        AgentTraceEvent traceEvent;
    };

    DispatchResult dispatchModelRequest(const ModelRequest& request,
                                        CancellationToken cancelToken);

    ToolOutcome beginToolCalls(const std::vector<ToolCall>& calls,
                               const ToolConfig& config);
    ToolOutcome resumeApprovedTool(const ToolConfig& config);
    ToolOutcome resumeDeniedTool(const ToolConfig& config);
    ToolOutcome approvePendingTool(const ToolConfig& config);
    ToolOutcome denyPendingTool(const ToolConfig& config);

    void reset();
    void resetForNewRun();
    void clearTrace();
    void addTraceEvent(AgentTraceType type,
                       const std::string& detail = {},
                       const std::string& tool = {},
                       long long durationMs = 0);
    void appendTraceEvent(AgentTraceEvent event);
    void appendTraceEvents(std::vector<AgentTraceEvent> events);
    void finishCompleted();
    void finishFailed();
    void finishCancelled();
    RunState state() const { return run_.state; }
    const std::string& runId() const { return run_.id; }
    int stepCount() const { return runner_.stepCount(); }
    const std::vector<AgentTraceEvent>& trace() const { return run_.trace; }
    const ToolCall* pendingApproval() const;
    AgentRunSnapshot snapshot() const;

private:
    void trimTrace();
    void updateRunFromToolOutcome(const ToolOutcome& outcome);
    void markWaitingModel();
    void markExecutingTools();
    void markWaitingApproval();
    void markCompleted();
    void markFailed();
    void markCancelled();

    AgentRun run_;
    AgentRunner runner_;
};

} // namespace AI

#endif // HAVE_AI_CHAT
