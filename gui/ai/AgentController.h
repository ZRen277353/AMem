#pragma once
#ifdef HAVE_AI_CHAT

#include "AIProvider.h"
#include "AgentTrace.h"

#include <atomic>
#include <string>
#include <vector>

namespace AI {

// Owns model-request dispatch for an agent run. ChatWindow still owns the UI
// and message persistence, while AgentController centralizes provider lookup,
// request construction, tool exposure, and async dispatch.
class AgentController {
public:
    enum class RunState {
        Idle,
        WaitingModel,
        ExecutingTools,
        WaitingApproval,
        Completed,
        Failed,
        Cancelled
    };

    struct ModelRequest {
        std::string providerName;
        std::string modelOverride;
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
                                        std::atomic<bool>& cancelFlag) const;

    void reset();
    void markWaitingModel();
    void markExecutingTools();
    void markWaitingApproval();
    void markCompleted();
    void markFailed();
    void markCancelled();
    RunState state() const { return state_; }

private:
    RunState state_ = RunState::Idle;
};

} // namespace AI

#endif // HAVE_AI_CHAT
