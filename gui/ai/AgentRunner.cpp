#ifdef HAVE_AI_CHAT

#include "AgentRunner.h"
#include "ToolCallSecurity.h"

#include "../../third_party/nlohmann/json.hpp"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <string>

namespace AI {

namespace {

long long nowUnixSeconds() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

int clampBudget(int value) {
    return std::clamp(value, 1, 64);
}

bool sameToolCall(const ToolCall& expected, const ToolCall& actual) {
    if (!expected.id.empty() || !actual.id.empty()) {
        return expected.id == actual.id;
    }
    return expected.name == actual.name && expected.arguments == actual.arguments;
}

} // namespace

void AgentRunner::reset() {
    pendingToolCalls_.clear();
    currentToolCallIndex_ = 0;
    stepCount_ = 0;
    awaitingConfirmation_ = false;
}

AgentRunner::Outcome AgentRunner::beginToolCalls(const std::vector<ToolCall>& calls,
                                                 const Config& config) {
    Outcome out;
    if (stepCount_ >= clampBudget(config.maxAgentSteps)) {
        appendTrace(out,
                    AgentTraceType::StepLimitReached,
                    nullptr,
                    "agent step limit reached");
        out.messages.push_back(makeSystemMessage(
            "[error] Agent step limit reached before executing more tools. "
            "Review the current results or raise the limit in Settings."));
        reset();
        out.kind = OutcomeKind::Stopped;
        return out;
    }

    ++stepCount_;
    pendingToolCalls_ = calls;
    currentToolCallIndex_ = 0;
    awaitingConfirmation_ = false;
    appendTrace(out,
                AgentTraceType::Started,
                nullptr,
                "received " + std::to_string(calls.size()) + " tool call(s)");

    const int maxCalls = clampBudget(config.maxToolCallsPerTurn);
    if (static_cast<int>(pendingToolCalls_.size()) > maxCalls) {
        appendTrace(out,
                    AgentTraceType::CallLimitTruncated,
                    nullptr,
                    "truncated to " + std::to_string(maxCalls) + " tool call(s)");
        out.messages.push_back(makeSystemMessage(
            "[error] Too many tool calls in one assistant turn; executing only the configured limit."));
        pendingToolCalls_.resize(static_cast<size_t>(maxCalls));
    }

    Outcome run = runUntilBlocked(config);
    out.messages.insert(out.messages.end(),
                        std::make_move_iterator(run.messages.begin()),
                        std::make_move_iterator(run.messages.end()));
    out.logs.insert(out.logs.end(),
                    std::make_move_iterator(run.logs.begin()),
                    std::make_move_iterator(run.logs.end()));
    out.traceEvents.insert(out.traceEvents.end(),
                           std::make_move_iterator(run.traceEvents.begin()),
                           std::make_move_iterator(run.traceEvents.end()));
    out.pendingToolCall = std::move(run.pendingToolCall);
    out.toolCallToExecute = std::move(run.toolCallToExecute);
    out.kind = run.kind;
    return out;
}

AgentRunner::Outcome AgentRunner::resumeApproved(const Config& config) {
    (void)config;
    Outcome out;
    if (!awaitingConfirmation_ ||
        currentToolCallIndex_ >= static_cast<int>(pendingToolCalls_.size())) {
        reset();
        out.kind = OutcomeKind::Stopped;
        return out;
    }

    awaitingConfirmation_ = false;
    appendTrace(out, AgentTraceType::Approved, &pendingToolCalls_[currentToolCallIndex_]);
    appendTrace(out, AgentTraceType::ToolStarted, &pendingToolCalls_[currentToolCallIndex_]);
    out.toolCallToExecute = pendingToolCalls_[currentToolCallIndex_];
    out.kind = OutcomeKind::NeedsExecution;
    return out;
}

AgentRunner::Outcome AgentRunner::resumeDenied(const Config& config) {
    (void)config;
    Outcome out;
    if (!awaitingConfirmation_ ||
        currentToolCallIndex_ >= static_cast<int>(pendingToolCalls_.size())) {
        reset();
        out.kind = OutcomeKind::Stopped;
        return out;
    }

    awaitingConfirmation_ = false;
    appendTrace(out, AgentTraceType::Denied, &pendingToolCalls_[currentToolCallIndex_]);
    out.messages.push_back(makeDeniedToolMessage(pendingToolCalls_[currentToolCallIndex_]));
    ++currentToolCallIndex_;

    const std::string skipReason =
        "skipped because an earlier write-classified tool was denied by user";
    while (currentToolCallIndex_ < static_cast<int>(pendingToolCalls_.size())) {
        const ToolCall& skipped = pendingToolCalls_[currentToolCallIndex_];
        appendTrace(out, AgentTraceType::ToolSkipped, &skipped, skipReason);
        out.messages.push_back(makeSkippedToolMessage(skipped, skipReason));
        ++currentToolCallIndex_;
    }

    pendingToolCalls_.clear();
    currentToolCallIndex_ = 0;
    appendTrace(out, AgentTraceType::ToolBatchComplete, nullptr);
    out.kind = OutcomeKind::ReadyForFollowUp;
    return out;
}

AgentRunner::Outcome AgentRunner::failPendingTool(
    const ToolResult& result,
    long long durationMs,
    const Config& config) {
    if (!awaitingConfirmation_ ||
        currentToolCallIndex_ >= static_cast<int>(pendingToolCalls_.size())) {
        Outcome out;
        reset();
        out.kind = OutcomeKind::Stopped;
        return out;
    }

    awaitingConfirmation_ = false;
    return completeToolExecution(
        pendingToolCalls_[currentToolCallIndex_],
        result,
        durationMs,
        config);
}

AgentRunner::Outcome AgentRunner::completeToolExecution(const ToolCall& call,
                                                        const ToolResult& result,
                                                        long long durationMs,
                                                        const Config& config) {
    Outcome out;
    if (currentToolCallIndex_ >= static_cast<int>(pendingToolCalls_.size())) {
        reset();
        out.kind = OutcomeKind::Stopped;
        return out;
    }

    const ToolCall& tc = pendingToolCalls_[currentToolCallIndex_];
    if (!sameToolCall(tc, call)) {
        appendTrace(out,
                    AgentTraceType::ToolFailed,
                    &tc,
                    "tool result did not match the pending tool call",
                    durationMs);
        out.messages.push_back(makeSystemMessage(
            "[error] Ignored a stale or mismatched tool result. "
            "The current agent run was stopped to preserve tool-call ordering."));
        reset();
        out.kind = OutcomeKind::Stopped;
        return out;
    }

    appendTrace(out,
                result.success ? AgentTraceType::ToolSucceeded
                               : AgentTraceType::ToolFailed,
                &tc,
                result.success ? "" : result.errorMessage,
                durationMs);
    out.messages.push_back(makeToolMessage(tc, result, durationMs));
    ++currentToolCallIndex_;

    if (!result.success) {
        std::string skipReason = "skipped because an earlier tool failed";
        if (!result.errorMessage.empty()) {
            skipReason += ": " + result.errorMessage;
            constexpr size_t kMaxSkipReasonChars = 240;
            if (skipReason.size() > kMaxSkipReasonChars) {
                skipReason.resize(kMaxSkipReasonChars - 3);
                skipReason += "...";
            }
        }

        while (currentToolCallIndex_ < static_cast<int>(pendingToolCalls_.size())) {
            const ToolCall& skipped = pendingToolCalls_[currentToolCallIndex_];
            appendTrace(out, AgentTraceType::ToolSkipped, &skipped, skipReason);
            out.messages.push_back(makeSkippedToolMessage(skipped, skipReason));
            ++currentToolCallIndex_;
        }

        pendingToolCalls_.clear();
        currentToolCallIndex_ = 0;
        awaitingConfirmation_ = false;
        appendTrace(out, AgentTraceType::ToolBatchComplete, nullptr);
        out.kind = OutcomeKind::ReadyForFollowUp;
        return out;
    }

    Outcome run = runUntilBlocked(config);
    out.messages.insert(out.messages.end(),
                        std::make_move_iterator(run.messages.begin()),
                        std::make_move_iterator(run.messages.end()));
    out.logs.insert(out.logs.end(),
                    std::make_move_iterator(run.logs.begin()),
                    std::make_move_iterator(run.logs.end()));
    out.traceEvents.insert(out.traceEvents.end(),
                           std::make_move_iterator(run.traceEvents.begin()),
                           std::make_move_iterator(run.traceEvents.end()));
    out.pendingToolCall = std::move(run.pendingToolCall);
    out.toolCallToExecute = std::move(run.toolCallToExecute);
    out.kind = run.kind;
    return out;
}

AgentRunner::Outcome AgentRunner::runUntilBlocked(const Config& config) {
    Outcome out;
    while (currentToolCallIndex_ < static_cast<int>(pendingToolCalls_.size())) {
        const ToolCall& tc = pendingToolCalls_[currentToolCallIndex_];
        const ToolSafety safety = ToolExecutor::getInstance().getToolSafety(tc.name);

        if (safety == ToolSafety::Write && !config.autoApproveWrites) {
            awaitingConfirmation_ = true;
            appendTrace(out,
                        AgentTraceType::AwaitingApproval,
                        &tc,
                        "write-classified tool requires approval");
            out.pendingToolCall = tc;
            out.kind = OutcomeKind::NeedsConfirmation;
            return out;
        }

        if (safety == ToolSafety::Write && config.autoApproveWrites) {
            out.logs.push_back("[AI Chat] auto-approving write tool '" + tc.name + "'");
            appendTrace(out, AgentTraceType::AutoApproved, &tc);
        }

        appendTrace(out, AgentTraceType::ToolStarted, &tc);
        out.toolCallToExecute = tc;
        out.kind = OutcomeKind::NeedsExecution;
        return out;
    }

    pendingToolCalls_.clear();
    currentToolCallIndex_ = 0;
    awaitingConfirmation_ = false;
    appendTrace(out, AgentTraceType::ToolBatchComplete, nullptr);
    out.kind = OutcomeKind::ReadyForFollowUp;
    return out;
}

void AgentRunner::appendTrace(Outcome& out,
                              AgentTraceType type,
                              const ToolCall* tc,
                              const std::string& detail,
                              long long durationMs) const {
    AgentTraceEvent ev;
    ev.type = type;
    ev.timestamp = nowUnixSeconds();
    ev.step = stepCount_;
    ev.index = currentToolCallIndex_ + 1;
    ev.total = static_cast<int>(pendingToolCalls_.size());
    if (tc) {
        ev.tool = tc->name;
    }
    ev.detail = detail;
    ev.durationMs = durationMs;
    out.traceEvents.push_back(std::move(ev));
}

ChatMessage AgentRunner::makeToolMessage(const ToolCall& tc,
                                         const ToolResult& result,
                                         long long durationMs) {
    ChatMessage toolMsg;
    toolMsg.role = Role::Tool;
    toolMsg.toolCallId = tc.id;
    toolMsg.name = tc.name;
    toolMsg.timestamp = nowUnixSeconds();
    toolMsg.durationMs = durationMs;

    nlohmann::json audit;
    audit["tool"] = tc.name;
    audit["success"] = result.success;
    audit["duration_ms"] = durationMs;
    audit["completion"] = toolCompletionStateName(result.completion);
    const std::string& auditArguments = toolCallArgumentsForDisplay(tc);
    try {
        audit["arguments"] = auditArguments.empty()
                                 ? nlohmann::json::object()
                                 : nlohmann::json::parse(auditArguments);
    } catch (const nlohmann::json::exception&) {
        audit["arguments_raw"] = auditArguments;
    }

    if (result.success) {
        try {
            audit["result"] = result.resultJson.empty()
                                  ? nlohmann::json::object()
                                  : nlohmann::json::parse(result.resultJson);
        } catch (const nlohmann::json::exception&) {
            audit["result_raw"] = result.resultJson;
        }
    } else {
        audit["error"] = result.errorMessage;
        if (!result.resultJson.empty()) {
            try {
                audit["details"] = nlohmann::json::parse(result.resultJson);
            } catch (const nlohmann::json::exception&) {
                audit["details_raw"] = result.resultJson;
            }
        }
    }

    toolMsg.content = audit.dump();
    return toolMsg;
}

ChatMessage AgentRunner::makeDeniedToolMessage(const ToolCall& tc) {
    nlohmann::json audit;
    audit["tool"] = tc.name;
    audit["success"] = false;
    audit["error"] = "tool execution denied by user";
    const std::string& auditArguments = toolCallArgumentsForDisplay(tc);
    try {
        audit["arguments"] = auditArguments.empty()
                                 ? nlohmann::json::object()
                                 : nlohmann::json::parse(auditArguments);
    } catch (const nlohmann::json::exception&) {
        audit["arguments_raw"] = auditArguments;
    }

    ChatMessage denied;
    denied.role = Role::Tool;
    denied.toolCallId = tc.id;
    denied.name = tc.name;
    denied.content = audit.dump();
    denied.timestamp = nowUnixSeconds();
    return denied;
}

ChatMessage AgentRunner::makeSkippedToolMessage(const ToolCall& tc,
                                                const std::string& reason) {
    nlohmann::json audit;
    audit["tool"] = tc.name;
    audit["success"] = false;
    audit["skipped"] = true;
    audit["error"] = reason;
    const std::string& auditArguments = toolCallArgumentsForDisplay(tc);
    try {
        audit["arguments"] = auditArguments.empty()
                                 ? nlohmann::json::object()
                                 : nlohmann::json::parse(auditArguments);
    } catch (const nlohmann::json::exception&) {
        audit["arguments_raw"] = auditArguments;
    }

    ChatMessage skipped;
    skipped.role = Role::Tool;
    skipped.toolCallId = tc.id;
    skipped.name = tc.name;
    skipped.content = audit.dump();
    skipped.timestamp = nowUnixSeconds();
    return skipped;
}

ChatMessage AgentRunner::makeSystemMessage(const std::string& text) {
    ChatMessage msg;
    msg.role = Role::System;
    msg.content = text;
    msg.timestamp = nowUnixSeconds();
    return msg;
}

} // namespace AI

#endif // HAVE_AI_CHAT
