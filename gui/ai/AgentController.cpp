#ifdef HAVE_AI_CHAT

#include "AgentController.h"

#include "ProviderRegistry.h"
#include "ToolExecutor.h"

#include <chrono>
#include <utility>

namespace AI {

namespace {

long long nowUnixSeconds() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
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

AgentController::DispatchResult AgentController::dispatchModelRequest(
    const ModelRequest& request,
    std::atomic<bool>& cancelFlag) const {
    DispatchResult result;
    result.providerName = request.providerName;

    AIProvider* provider =
        ProviderRegistry::getInstance().getProvider(request.providerName);
    if (!provider) {
        result.error = "active provider unavailable";
        result.traceEvent =
            makeTrace(AgentTraceType::ProviderError, request.providerName, result.error);
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
    cancelFlag.store(false);
    provider->sendCompletion(completion, cancelFlag);
    result.dispatched = true;
    return result;
}

void AgentController::reset() {
    state_ = RunState::Idle;
}

void AgentController::markWaitingModel() {
    state_ = RunState::WaitingModel;
}

void AgentController::markExecutingTools() {
    state_ = RunState::ExecutingTools;
}

void AgentController::markWaitingApproval() {
    state_ = RunState::WaitingApproval;
}

void AgentController::markCompleted() {
    state_ = RunState::Completed;
}

void AgentController::markFailed() {
    state_ = RunState::Failed;
}

void AgentController::markCancelled() {
    state_ = RunState::Cancelled;
}

} // namespace AI

#endif // HAVE_AI_CHAT
