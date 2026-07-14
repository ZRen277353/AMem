#ifdef HAVE_AI_CHAT

#include "AgentController.h"

#include "ContextBudget.h"
#include "ProviderRegistry.h"
#include "ProviderResponseLimits.h"
#include "ProviderTrust.h"
#include "ToolExecutor.h"
#include "../../mem/IMemService.h"
#include "../../third_party/nlohmann/json.hpp"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <utility>

namespace AI {

namespace {

long long nowUnixSeconds() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

std::string makeRunId() {
    static std::atomic<unsigned long long> counter{0};
    using namespace std::chrono;
    const long long nowMs =
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    const unsigned long long serial = counter.fetch_add(1, std::memory_order_relaxed) + 1;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "run-%lld-%llu", nowMs, serial);
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

bool startsWithICase(const std::string& s, const char* prefix) {
    const size_t n = std::strlen(prefix);
    if (s.size() < n) {
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        const unsigned char a = static_cast<unsigned char>(s[i]);
        const unsigned char b = static_cast<unsigned char>(prefix[i]);
        if (std::tolower(a) != std::tolower(b)) {
            return false;
        }
    }
    return true;
}

std::string validateProviderConfig(const AIProvider& provider,
                                   const std::string& modelOverride) {
    const ProviderConfig& cfg = provider.getConfig();
    if (cfg.apiKey.empty()) {
        return "Enter an API key in Settings before sending";
    }
    if (cfg.baseUrl.empty() || !startsWithICase(cfg.baseUrl, "https://")) {
        return "Configure a valid https:// endpoint in Settings before sending";
    }
    if (!isProviderEndpointTrusted(provider, cfg)) {
        return "Trust the custom endpoint in Settings before sending credentials";
    }
    if (modelOverride.empty() && cfg.model.empty()) {
        return "Enter a model name before sending";
    }
    return {};
}

ToolResult contextFailureResult(const Mem::Error& error,
                                const ToolResult* staleResult = nullptr) {
    nlohmann::json output;
    output["success"] = false;
    output["error"] = {
        {"code", Mem::errorCodeName(error.code)},
        {"message", error.message},
        {"retryable", error.retryable},
    };
    if (staleResult && staleResult->success) {
        output["stale_result_was_success"] = true;
        if (!staleResult->resultJson.empty()) {
            nlohmann::json staleResultJson;
            std::string parseError;
            if (parseToolResultJson(
                    staleResult->resultJson, staleResultJson, parseError)) {
                output["stale_result"] = std::move(staleResultJson);
            } else {
                output["stale_result_raw"] = staleResult->resultJson;
            }
        }
    }

    ToolResult result;
    result.success = false;
    result.resultJson = output.dump();
    result.errorMessage = error.message;
    result.completion = staleResult
        ? staleResult->completion
        : ToolCompletionState::RejectedBeforeStart;
    return result;
}

void appendOutcomePayload(AgentRunner::Outcome& destination,
                          AgentRunner::Outcome source) {
    destination.messages.insert(
        destination.messages.end(),
        std::make_move_iterator(source.messages.begin()),
        std::make_move_iterator(source.messages.end()));
    destination.logs.insert(
        destination.logs.end(),
        std::make_move_iterator(source.logs.begin()),
        std::make_move_iterator(source.logs.end()));
    destination.pendingToolCall = std::move(source.pendingToolCall);
    destination.toolCallToExecute = std::move(source.toolCallToExecute);
    destination.kind = source.kind;
}

} // namespace

AgentController::AgentController(Mem::IMemService& memService)
    : memService_(memService) {}

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
    CancellationToken cancelToken) {
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
    const std::string configError =
        validateProviderConfig(*provider, request.modelOverride);
    if (!configError.empty()) {
        result.error = configError;
        result.traceEvent =
            makeTrace(AgentTraceType::ProviderError,
                      request.providerName,
                      configError);
        appendTraceEvent(result.traceEvent);
        markFailed();
        return result;
    }

    CompletionRequest completion;
    completion.runId = run_.id;
    completion.tools = ToolExecutor::getInstance().getToolDefinitions();
    completion.model =
        request.modelOverride.empty() ? cfg.model : request.modelOverride;
    completion.stream = request.stream;

    const ProviderCapabilities capabilities = provider->getCapabilities();
    ContextBudgetConfig budgetConfig;
    budgetConfig.userTokenLimit = request.userTokenLimit > 0
        ? request.userTokenLimit
        : capabilities.maxContextTokens;
    budgetConfig.providerContextTokens = capabilities.maxContextTokens;
    budgetConfig.configuredContextTokens = cfg.contextWindowTokens;
    budgetConfig.configuredContextMayExceedProvider =
        !isDefaultProviderEndpoint(*provider, cfg.baseUrl);
    budgetConfig.providerMaxOutputTokens = capabilities.maxOutputTokens;
    ContextBudgetResult budget = prepareContextBudget(
        request.messages, completion.tools, budgetConfig);
    if (!budget.success) {
        result.error = "context budget rejected request: " + budget.error;
        result.traceEvent = makeTrace(AgentTraceType::ProviderError,
                                      request.providerName,
                                      result.error);
        appendTraceEvent(result.traceEvent);
        markFailed();
        return result;
    }
    completion.messages = std::move(budget.messages);
    completion.maxOutputTokens = budget.outputTokenReserve;
    result.contextWindowTokens = budget.contextWindowTokens;
    result.estimatedInputTokens = budget.estimatedInputTokens;
    result.outputTokenReserve = budget.outputTokenReserve;
    result.droppedMessages = budget.droppedMessages;

    result.model = completion.model;
    std::string dispatchDetail = completion.model.empty()
        ? "model request dispatched"
        : "model request dispatched: " + completion.model;
    dispatchDetail += " (input " +
        std::to_string(result.estimatedInputTokens) + "/" +
        std::to_string(budget.inputBudgetTokens) +
        ", output reserve " +
        std::to_string(result.outputTokenReserve);
    if (result.droppedMessages != 0) {
        dispatchDetail += ", dropped " +
            std::to_string(result.droppedMessages) + " old messages";
    }
    dispatchDetail += ")";
    result.traceEvent =
        makeTrace(AgentTraceType::ModelRequestDispatched,
                   request.providerName,
                   dispatchDetail);
    appendTraceEvent(result.traceEvent);
    if (!cancelToken) {
        cancelToken = std::make_shared<std::atomic<bool>>(false);
    }
    cancelToken->store(false);
    run_.context.operation.cancellation = cancelToken;
    provider->sendCompletion(completion, std::move(cancelToken));
    result.dispatched = true;
    markWaitingModel();
    return result;
}

AgentController::ToolOutcome AgentController::beginToolCalls(
    const std::vector<ToolCall>& calls,
    const ToolConfig& config) {
    markExecutingTools();
    return consumeToolOutcome(runner_.beginToolCalls(calls, config), config);
}

AgentController::ToolOutcome AgentController::resumeApprovedTool(
    const ToolConfig& config) {
    return consumeToolOutcome(runner_.resumeApproved(config), config);
}

AgentController::ToolOutcome AgentController::approvePendingTool(
    const ToolConfig& config) {
    return resumeApprovedTool(config);
}

AgentController::ToolOutcome AgentController::resumeDeniedTool(
    const ToolConfig& config) {
    return consumeToolOutcome(runner_.resumeDenied(config), config);
}

AgentController::ToolOutcome AgentController::completeToolExecution(
    const ToolCall& call,
    const ToolResult& result,
    long long durationMs,
    const ToolConfig& config) {
    ToolResult effectiveResult = result;
    std::optional<Mem::TargetSnapshot> selectedTarget;
    const ToolTargetPolicy policy =
        ToolExecutor::getInstance().getToolTargetPolicy(call.name);

    if (effectiveResult.success ||
        policy == ToolTargetPolicy::Selection) {
        const Mem::OperationContext current =
            memService_.captureContext(policy != ToolTargetPolicy::None);
        std::optional<Mem::Error> contextError;
        if (policy == ToolTargetPolicy::Selection) {
            if (effectiveResult.selectedTarget) {
                contextError = validateTargetSelectionResult(
                    run_.context.operation, current,
                    *effectiveResult.selectedTarget);
                if (!contextError) {
                    selectedTarget = effectiveResult.selectedTarget;
                }
            } else if (effectiveResult.success) {
                contextError = Mem::Error{
                    Mem::ErrorCode::InternalError,
                    "target-selection result is missing its target snapshot",
                    false};
            } else {
                contextError = validateAgentRunContext(
                    run_.context.operation, current, policy);
            }
        } else if (effectiveResult.success) {
            contextError = validateAgentRunContext(
                run_.context.operation, current, policy);
        }

        if (contextError) {
            effectiveResult =
                contextFailureResult(*contextError, &effectiveResult);
        }
    }

    AgentRunner::Outcome outcome = runner_.completeToolExecution(
        call, effectiveResult, durationMs, config);
    if (selectedTarget &&
        outcome.kind != AgentRunner::OutcomeKind::Stopped) {
        run_.context.operation.target = *selectedTarget;
    }
    return consumeToolOutcome(std::move(outcome), config);
}

AgentController::ToolOutcome AgentController::denyPendingTool(
    const ToolConfig& config) {
    return resumeDeniedTool(config);
}

void AgentController::reset() {
    runner_.reset();
    run_.id.clear();
    run_.state = RunState::Idle;
    run_.modelTurns = 0;
    run_.toolSteps = 0;
    run_.pendingApproval.reset();
    run_.context = {};
}

void AgentController::resetForNewRun() {
    reset();
    run_.id = makeRunId();
    run_.context.runId = run_.id;
    run_.context.operation = memService_.captureContext(true);
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

AgentController::ToolOutcome AgentController::consumeToolOutcome(
    ToolOutcome outcome,
    const ToolConfig& config) {
    appendTraceEvents(std::move(outcome.traceEvents));
    outcome.traceEvents.clear();

    const ToolCall* blockedCall = nullptr;
    bool awaitingApproval = false;
    if (outcome.kind == ToolOutcomeKind::NeedsConfirmation &&
        outcome.pendingToolCall) {
        blockedCall = &*outcome.pendingToolCall;
        awaitingApproval = true;
    } else if (outcome.kind == ToolOutcomeKind::NeedsExecution &&
               outcome.toolCallToExecute) {
        blockedCall = &*outcome.toolCallToExecute;
    }

    if (blockedCall) {
        if (const auto error = validateToolContext(*blockedCall)) {
            const ToolResult failure = contextFailureResult(*error);
            ToolOutcome rejected = awaitingApproval
                ? runner_.failPendingTool(failure, 0, config)
                : runner_.completeToolExecution(
                      *blockedCall, failure, 0, config);
            appendTraceEvents(std::move(rejected.traceEvents));
            rejected.traceEvents.clear();
            appendOutcomePayload(outcome, std::move(rejected));
        }
    }

    updateRunFromToolOutcome(outcome);
    return outcome;
}

std::optional<Mem::Error> AgentController::validateToolContext(
    const ToolCall& call) const {
    const ToolTargetPolicy policy =
        ToolExecutor::getInstance().getToolTargetPolicy(call.name);
    const Mem::OperationContext current =
        memService_.captureContext(policy != ToolTargetPolicy::None);
    return validateAgentRunContext(run_.context.operation, current, policy);
}

void AgentController::updateRunFromToolOutcome(const ToolOutcome& outcome) {
    run_.toolSteps = runner_.stepCount();
    if (outcome.kind == ToolOutcomeKind::NeedsConfirmation &&
        outcome.pendingToolCall) {
        run_.pendingApproval = outcome.pendingToolCall;
    } else {
        run_.pendingApproval.reset();
    }

    switch (outcome.kind) {
        case ToolOutcomeKind::NeedsConfirmation:
            if (outcome.pendingToolCall) {
                markWaitingApproval();
            } else {
                finishFailed();
            }
            break;
        case ToolOutcomeKind::NeedsExecution:
            run_.state = RunState::ExecutingTools;
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
}

void AgentController::markFailed() {
    run_.state = RunState::Failed;
    run_.pendingApproval.reset();
}

void AgentController::markCancelled() {
    run_.state = RunState::Cancelled;
    run_.pendingApproval.reset();
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
    snap.context = run_.context;
    return snap;
}

const ToolCall* AgentController::pendingApproval() const {
    return run_.pendingApproval ? &*run_.pendingApproval : nullptr;
}

} // namespace AI

#endif // HAVE_AI_CHAT
