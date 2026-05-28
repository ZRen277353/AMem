#ifdef HAVE_AI_CHAT

#include "AgentController.h"

#include "ProviderRegistry.h"
#include "ToolExecutor.h"

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <utility>

namespace AI {

namespace {

long long nowUnixSeconds() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

std::string makeRunId() {
    using namespace std::chrono;
    const long long nowMs =
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    char buf[32];
    std::snprintf(buf, sizeof(buf), "run-%lld", nowMs);
    return std::string(buf);
}

AgentTraceEvent makeTrace(AgentTraceType type,
                          const std::string& provider,
                          const std::string& detail) {
    AgentTraceEvent ev;
    ev.type = type;
    ev.timestamp = nowUnixSeconds();
    ev.tool = provider;
    ev.detail = detail;
    return ev;
}

} // namespace

void AgentController::trimTrace() {
    constexpr size_t kMaxTraceEvents = 128;
    if (run_.trace.size() > kMaxTraceEvents) {
        run_.trace.erase(run_.trace.begin(),
                         run_.trace.begin() +
                             static_cast<std::ptrdiff_t>(run_.trace.size() -
                                                         kMaxTraceEvents));
    }
}

AgentController::DispatchResult AgentController::dispatchModelRequest(
    const ModelRequest& request,
    std::atomic<bool>& cancelFlag) {
    DispatchResult result;
    result.providerName = request.providerName;

    AIProvider* provider =
        ProviderRegistry::getInstance().getProvider(request.providerName);
    if (!provider) {
        result.error = "active provider unavailable";
        result.traceEvent =
            makeTrace(AgentTraceType::ProviderError,
                      request.providerName,
                      request.failureDetail.empty() ? result.error : request.failureDetail);
        appendTraceEvent(result.traceEvent);
        markFailed();
        return result;
    }

    const ProviderConfig& cfg = provider->getConfig();
    CompletionRequest completion;
    completion.messages = request.messages;
    completion.tools = ToolExecutor::getInstance().getToolDefinitions();
    completion.model =
        request.modelOverride.empty() ? cfg.model : request.modelOverride;
    completion.stream = request.stream;

    result.model = completion.model;
    result.traceEvent =
        makeTrace(AgentTraceType::ModelRequestDispatched,
                  request.providerName,
                  completion.model.empty()
                      ? "model request dispatched"
                      : "model request dispatched: " + completion.model);
    appendTraceEvent(result.traceEvent);
    cancelFlag.store(false);
    provider->sendCompletion(completion, cancelFlag);
    result.dispatched = true;
    markWaitingModel();
    return result;
}

AgentController::ToolOutcome AgentController::beginToolCalls(
    const std::vector<ToolCall>& calls,
    const ToolConfig& config) {
    markExecutingTools();
    AgentRunner::Outcome outcome = runner_.beginToolCalls(calls, config);
    appendTraceEvents(outcome.traceEvents);
    outcome.traceEvents.clear();
    updateRunFromToolOutcome(outcome);
    return outcome;
}

AgentController::ToolOutcome AgentController::resumeApprovedTool(
    const ToolConfig& config) {
    AgentRunner::Outcome outcome = runner_.resumeApproved(config);
    appendTraceEvents(outcome.traceEvents);
    outcome.traceEvents.clear();
    updateRunFromToolOutcome(outcome);
    return outcome;
}

AgentController::ToolOutcome AgentController::approvePendingTool(
    const ToolConfig& config) {
    run_.approvalDecision = AgentApprovalDecision::Approved;
    return resumeApprovedTool(config);
}

AgentController::ToolOutcome AgentController::resumeDeniedTool(
    const ToolConfig& config) {
    AgentRunner::Outcome outcome = runner_.resumeDenied(config);
    appendTraceEvents(outcome.traceEvents);
    outcome.traceEvents.clear();
    updateRunFromToolOutcome(outcome);
    return outcome;
}

AgentController::ToolOutcome AgentController::denyPendingTool(
    const ToolConfig& config) {
    run_.approvalDecision = AgentApprovalDecision::Denied;
    return resumeDeniedTool(config);
}

void AgentController::reset() {
    runner_.reset();
    run_.id.clear();
    run_.state = RunState::Idle;
    run_.modelTurns = 0;
    run_.toolSteps = 0;
    run_.pendingApproval.reset();
    run_.approvalDecision = AgentApprovalDecision::Pending;
}

void AgentController::resetForNewRun() {
    reset();
    run_.id = makeRunId();
    clearTrace();
}

void AgentController::clearTrace() {
    run_.trace.clear();
}

void AgentController::addTraceEvent(AgentTraceType type,
                                    const std::string& detail,
                                    const std::string& tool,
                                    long long durationMs) {
    AgentTraceEvent ev;
    ev.type = type;
    ev.timestamp = nowUnixSeconds();
    ev.step = runner_.stepCount();
    ev.tool = tool;
    ev.detail = detail;
    ev.durationMs = durationMs;
    run_.trace.push_back(std::move(ev));
    trimTrace();
}

void AgentController::appendTraceEvents(std::vector<AgentTraceEvent> events) {
    for (auto& ev : events) {
        appendTraceEvent(std::move(ev));
    }
}

void AgentController::appendTraceEvent(AgentTraceEvent event) {
    run_.trace.push_back(std::move(event));
    trimTrace();
}

void AgentController::updateRunFromToolOutcome(const ToolOutcome& outcome) {
    run_.toolSteps = runner_.stepCount();
    if (outcome.pendingToolCall) {
        run_.pendingApproval = outcome.pendingToolCall;
        run_.approvalDecision = AgentApprovalDecision::Pending;
    } else {
        run_.pendingApproval.reset();
        run_.approvalDecision = AgentApprovalDecision::Pending;
    }

    switch (outcome.kind) {
        case ToolOutcomeKind::NeedsConfirmation:
            if (outcome.pendingToolCall) {
                markWaitingApproval();
            } else {
                finishFailed();
            }
            break;
        case ToolOutcomeKind::ReadyForFollowUp:
            run_.state = RunState::ExecutingTools;
            break;
        case ToolOutcomeKind::Stopped:
        case ToolOutcomeKind::Idle:
        default:
            finishFailed();
            break;
    }
}

void AgentController::markWaitingModel() {
    run_.state = RunState::WaitingModel;
    ++run_.modelTurns;
}

void AgentController::markExecutingTools() {
    run_.state = RunState::ExecutingTools;
}

void AgentController::markWaitingApproval() {
    run_.state = RunState::WaitingApproval;
}

void AgentController::markCompleted() {
    run_.state = RunState::Completed;
    run_.pendingApproval.reset();
    run_.approvalDecision = AgentApprovalDecision::Pending;
}

void AgentController::markFailed() {
    run_.state = RunState::Failed;
    run_.pendingApproval.reset();
    run_.approvalDecision = AgentApprovalDecision::Pending;
}

void AgentController::markCancelled() {
    run_.state = RunState::Cancelled;
    run_.pendingApproval.reset();
    run_.approvalDecision = AgentApprovalDecision::Pending;
}

void AgentController::finishCompleted() {
    run_.toolSteps = runner_.stepCount();
    runner_.reset();
    markCompleted();
}

void AgentController::finishFailed() {
    run_.toolSteps = runner_.stepCount();
    runner_.reset();
    markFailed();
}

void AgentController::finishCancelled() {
    run_.toolSteps = runner_.stepCount();
    runner_.reset();
    markCancelled();
}

AgentRunSnapshot AgentController::snapshot() const {
    AgentRunSnapshot snap;
    snap.id = run_.id;
    snap.state = run_.state;
    snap.modelTurns = run_.modelTurns;
    snap.toolSteps = run_.toolSteps;
    snap.trace = run_.trace;
    snap.pendingApproval = run_.pendingApproval;
    snap.approvalDecision = run_.approvalDecision;
    return snap;
}

const ToolCall* AgentController::pendingApproval() const {
    return run_.pendingApproval ? &*run_.pendingApproval : nullptr;
}

} // namespace AI

#endif // HAVE_AI_CHAT
