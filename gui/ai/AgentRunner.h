#pragma once
#ifdef HAVE_AI_CHAT

#include "AIProvider.h"
#include "AgentTrace.h"
#include "ToolExecutor.h"

#include <optional>
#include <string>
#include <vector>

namespace AI {

// Runs the model->tool->model loop for one AI turn. This class deliberately
// contains no ImGui code: ChatWindow owns presentation and confirmation UI,
// while AgentRunner owns queueing, budgets, and tool-result audit messages.
class AgentRunner {
public:
    struct Config {
        int maxAgentSteps = 12;
        int maxToolCallsPerTurn = 16;
        bool autoApproveWrites = false;
    };

    enum class OutcomeKind {
        Idle,
        NeedsConfirmation,
        NeedsExecution,
        ReadyForFollowUp,
        Stopped
    };

    struct Outcome {
        OutcomeKind kind = OutcomeKind::Idle;
        std::vector<ChatMessage> messages;
        std::vector<std::string> logs;
        std::vector<AgentTraceEvent> traceEvents;
        std::optional<ToolCall> pendingToolCall;
        std::optional<ToolCall> toolCallToExecute;
    };

    void reset();

    Outcome beginToolCalls(const std::vector<ToolCall>& calls, const Config& config);
    Outcome resumeApproved(const Config& config);
    Outcome resumeDenied(const Config& config);
    Outcome completeToolExecution(const ToolCall& call,
                                  const ToolResult& result,
                                  long long durationMs,
                                  const Config& config);

    int stepCount() const { return stepCount_; }
    bool hasPendingConfirmation() const { return awaitingConfirmation_; }

private:
    Outcome runUntilBlocked(const Config& config);
    void appendTrace(Outcome& out,
                     AgentTraceType type,
                     const ToolCall* tc,
                     const std::string& detail = {},
                     long long durationMs = 0) const;
    static ChatMessage makeToolMessage(const ToolCall& tc, const ToolResult& result, long long durationMs);
    static ChatMessage makeDeniedToolMessage(const ToolCall& tc);
    static ChatMessage makeSkippedToolMessage(const ToolCall& tc, const std::string& reason);
    static ChatMessage makeSystemMessage(const std::string& text);

    std::vector<ToolCall> pendingToolCalls_;
    int currentToolCallIndex_ = 0;
    int stepCount_ = 0;
    bool awaitingConfirmation_ = false;
};

} // namespace AI

#endif // HAVE_AI_CHAT
