#include "IpcMemServiceDispatcher.h"

#include "IpcApprovalBroker.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <exception>
#include <thread>
#include <utility>

namespace NativeIpc {
namespace {

using json = nlohmann::json;
using namespace std::chrono_literals;

RequestCompletion parseCompletion(const json& payload) {
    const auto completion = payload.find("completion");
    if (completion == payload.end() || !completion->is_string()) {
        return RequestCompletion::Completed;
    }
    const std::string value = completion->get<std::string>();
    if (value == "rejected_before_start") {
        return RequestCompletion::RejectedBeforeStart;
    }
    if (value == "cancelled_before_start") {
        return RequestCompletion::CancelledBeforeStart;
    }
    if (value == "cancelled_before_send") {
        return RequestCompletion::CancelledBeforeSend;
    }
    if (value == "cancel_requested") {
        return RequestCompletion::CancelRequested;
    }
    if (value == "completed_after_cancel_request") {
        return RequestCompletion::CompletedAfterCancelRequest;
    }
    if (value == "completion_unknown") {
        return RequestCompletion::CompletionUnknown;
    }
    return RequestCompletion::Completed;
}

IpcDispatchResult localError(const char* code,
                             const char* message,
                             RequestCompletion completion) {
    IpcDispatchResult result;
    result.errorCode = code;
    result.errorMessage = message;
    result.completion = completion;
    return result;
}

IpcDispatchResult approvalCancellation(RequestCancelReason reason) {
    if (reason == RequestCancelReason::Deadline) {
        return localError("approval_expired",
                          "request deadline expired before approval",
                          RequestCompletion::CancelledBeforeStart);
    }
    if (reason == RequestCancelReason::SessionInvalidated) {
        return localError("session_invalidated",
                          "session changed before approval completed",
                          RequestCompletion::CancelledBeforeStart);
    }
    return localError("cancelled", "request cancelled before approval",
                      RequestCompletion::CancelledBeforeStart);
}

} // namespace

IpcMemServiceDispatcher::IpcMemServiceDispatcher(
    Mem::IMemService& service,
    IpcApprovalBroker* approvalBroker,
    IpcExternalSession session)
    : service_(service), tools_(service), baseline_(service.captureContext(true)),
      approvalBroker_(approvalBroker), session_(std::move(session)) {
}

bool IpcMemServiceDispatcher::resolveCapability(
    const std::string& method,
    IpcCapability& capability) const {
    const IpcMethodDescriptor* descriptor = FindIpcMethod(method);
    if (descriptor == nullptr) {
        return false;
    }
    capability = descriptor->capability;
    return true;
}

bool IpcMemServiceDispatcher::canSubmitForApproval(
    const std::string& method) const {
    const IpcMethodDescriptor* descriptor = FindIpcMethod(method);
    return approvalBroker_ != nullptr && session_.sessionId != 0 &&
           !session_.clientName.empty() && descriptor != nullptr &&
           !descriptor->executableWithoutApproval &&
           descriptor->capability != IpcCapability::Observe;
}

IIpcRequestDispatcher::SessionValidation
IpcMemServiceDispatcher::validateSession() const {
    const Mem::OperationContext current = service_.captureContext(true);
    if (current.connectionGeneration != baseline_.connectionGeneration) {
        return {false, "connection_changed",
                "connection generation changed; reconnect the IPC session"};
    }
    if (current.target != baseline_.target) {
        return {false, "target_changed",
                "target snapshot changed; reconnect the IPC session"};
    }
    return {};
}

IpcDispatchResult IpcMemServiceDispatcher::validationFailure(
    const SessionValidation& validation,
    RequestCompletion completion) {
    IpcDispatchResult result;
    result.errorCode = validation.code.empty()
                           ? "session_invalidated"
                           : validation.code;
    result.errorMessage = validation.message.empty()
                              ? "IPC session state is no longer current"
                              : validation.message;
    result.completion = completion;
    return result;
}

IpcDispatchResult IpcMemServiceDispatcher::execute(
    const IpcRequestDto& request,
    const IpcRequestContext& context) {
    const IpcMethodDescriptor* descriptor = FindIpcMethod(request.method);
    if (descriptor == nullptr) {
        return localError("method_not_found", "request method is not registered",
                          RequestCompletion::RejectedBeforeStart);
    }
    if (!descriptor->executableWithoutApproval ||
        descriptor->capability != IpcCapability::Observe) {
        if (!canSubmitForApproval(request.method)) {
            return localError(
                "approval_required",
                "privileged method requires the GUI approval broker",
                RequestCompletion::RejectedBeforeStart);
        }
        return submitForApproval(request, context);
    }
    if (!descriptor->memServiceBacked) {
        return localError("unsupported", "method has no MemService adapter",
                          RequestCompletion::RejectedBeforeStart);
    }

    const SessionValidation before = validateSession();
    if (!before.valid) {
        return validationFailure(before,
                                 RequestCompletion::RejectedBeforeStart);
    }

    Mem::OperationContext operation = baseline_;
    operation.deadline = context.deadline;
    if (context.cancellation != nullptr) {
        operation.cancellation = context.cancellation->booleanToken();
    }

    IpcDispatchResult result;
    try {
        result = invokeObserve(request, operation);
    } catch (...) {
        return localError("dispatcher_exception",
                          "MemService adapter terminated with an exception",
                          RequestCompletion::CompletionUnknown);
    }

    const SessionValidation after = validateSession();
    if (!after.valid) {
        return validationFailure(after, RequestCompletion::CancelRequested);
    }
    return result;
}

IpcDispatchResult IpcMemServiceDispatcher::submitForApproval(
    const IpcRequestDto& request,
    const IpcRequestContext& context) {
    const SessionValidation before = validateSession();
    if (!before.valid) {
        return validationFailure(before,
                                 RequestCompletion::RejectedBeforeStart);
    }
    if (context.cancellationRequested()) {
        return approvalCancellation(context.cancellation->reason());
    }
    if (context.deadlineExceeded()) {
        return approvalCancellation(RequestCancelReason::Deadline);
    }

    IpcApprovalSubmission submission;
    submission.sessionId = session_.sessionId;
    submission.requestId = request.requestId;
    submission.clientName = session_.clientName;
    submission.clientVersion = session_.clientVersion;
    submission.method = request.method;
    submission.expected = baseline_;
    submission.deadline = context.deadline;
    const IpcApprovalResult submitted = approvalBroker_->submit(submission);
    if (!submitted.ok || !submitted.record.has_value()) {
        return localError(
            submitted.code.empty() ? "approval_submission_failed"
                                   : submitted.code.c_str(),
            submitted.message.empty() ? "approval request was not accepted"
                                      : submitted.message.c_str(),
            RequestCompletion::RejectedBeforeStart);
    }

    const uint64_t approvalId = submitted.record->approvalId;
    while (true) {
        if (context.cancellationRequested()) {
            const RequestCancelReason reason = context.cancellation->reason();
            if (reason == RequestCancelReason::Deadline) {
                approvalBroker_->expire();
            } else {
                approvalBroker_->cancelRequest(session_.sessionId,
                                               request.requestId);
            }
            return approvalCancellation(reason);
        }
        if (context.deadlineExceeded()) {
            approvalBroker_->expire();
            return approvalCancellation(RequestCancelReason::Deadline);
        }

        const auto record = approvalBroker_->find(approvalId);
        if (!record.has_value()) {
            return localError("approval_not_found",
                              "approval record disappeared before decision",
                              RequestCompletion::CompletionUnknown);
        }
        switch (record->state) {
        case IpcApprovalState::Pending:
            std::this_thread::sleep_for(20ms);
            continue;
        case IpcApprovalState::Approved:
            approvalBroker_->cancelRequest(session_.sessionId,
                                           request.requestId);
            return localError(
                "approval_execution_disabled",
                "approval was recorded but privileged execution is disabled",
                RequestCompletion::RejectedBeforeStart);
        case IpcApprovalState::Denied:
            return localError("approval_denied", "approval was denied",
                              RequestCompletion::RejectedBeforeStart);
        case IpcApprovalState::Invalidated:
            return localError("approval_invalidated",
                              "approval context is no longer current",
                              RequestCompletion::RejectedBeforeStart);
        case IpcApprovalState::Expired:
            return localError("approval_expired", "approval deadline expired",
                              RequestCompletion::CancelledBeforeStart);
        case IpcApprovalState::Cancelled:
            return localError("cancelled", "approval request was cancelled",
                              RequestCompletion::CancelledBeforeStart);
        case IpcApprovalState::Consumed:
            return localError("approval_state_error",
                              "approval was consumed outside the dispatcher",
                              RequestCompletion::CompletionUnknown);
        }
    }
}

IpcDispatchResult IpcMemServiceDispatcher::invokeObserve(
    const IpcRequestDto& request,
    const Mem::OperationContext& context) {
    std::string payload;
    if (request.method == "status") {
        payload = tools_.status(request.paramsJson, context);
    } else if (request.method == "process_list") {
        payload = tools_.processList(request.paramsJson, context);
    } else if (request.method == "module_list") {
        payload = tools_.moduleList(request.paramsJson, context);
    } else if (request.method == "module_resolve") {
        payload = tools_.moduleResolve(request.paramsJson, context);
    } else if (request.method == "pointer_resolve") {
        payload = tools_.pointerResolve(request.paramsJson, context);
    } else if (request.method == "disassemble") {
        payload = tools_.disassemble(request.paramsJson, context);
    } else if (request.method == "symbol_resolve") {
        payload = tools_.symbolResolve(request.paramsJson, context);
    } else if (request.method == "symbol_list") {
        payload = tools_.symbolList(request.paramsJson, context);
    } else if (request.method == "breakpoint_hits") {
        payload = tools_.breakpointHits(request.paramsJson, context);
    } else if (request.method == "scan_results") {
        payload = tools_.scanResults(request.paramsJson, context);
    } else if (request.method == "memory_read") {
        payload = tools_.memoryRead(request.paramsJson, context);
    } else if (request.method == "memory_read_value") {
        payload = tools_.memoryReadValue(request.paramsJson, context);
    } else {
        return localError("unsupported", "Observe method is not adapted",
                          RequestCompletion::RejectedBeforeStart);
    }
    return fromToolJson(payload);
}

IpcDispatchResult IpcMemServiceDispatcher::fromToolJson(
    const std::string& payload) {
    const json parsed = json::parse(payload, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return localError("internal_response_error",
                          "MemService adapter returned invalid JSON",
                          RequestCompletion::CompletionUnknown);
    }
    const auto success = parsed.find("success");
    if (success == parsed.end() || !success->is_boolean()) {
        return localError("internal_response_error",
                          "MemService adapter omitted success state",
                          RequestCompletion::CompletionUnknown);
    }

    IpcDispatchResult result;
    if (success->get<bool>()) {
        result.ok = true;
        result.resultJson = payload;
        const RequestCompletion completion = parseCompletion(parsed);
        result.completion =
            completion == RequestCompletion::CompletedAfterCancelRequest
                ? completion
                : RequestCompletion::Completed;
        return result;
    }

    const auto error = parsed.find("error");
    if (error == parsed.end() || !error->is_object()) {
        return localError("internal_response_error",
                          "MemService adapter omitted error details",
                          RequestCompletion::CompletionUnknown);
    }
    const auto code = error->find("code");
    const auto message = error->find("message");
    if (code == error->end() || !code->is_string() ||
        message == error->end() || !message->is_string()) {
        return localError("internal_response_error",
                          "MemService adapter returned invalid error details",
                          RequestCompletion::CompletionUnknown);
    }
    result.errorCode = code->get<std::string>();
    result.errorMessage = message->get<std::string>();
    const auto retryable = error->find("retryable");
    result.retryable = retryable != error->end() && retryable->is_boolean() &&
                       retryable->get<bool>();
    result.completion = parseCompletion(parsed);
    if (result.completion == RequestCompletion::Completed) {
        if (result.errorCode == "cancel_requested" ||
            result.errorCode == "timeout") {
            result.completion = RequestCompletion::CancelRequested;
        } else if (result.errorCode == "completion_unknown") {
            result.completion = RequestCompletion::CompletionUnknown;
        }
    }
    return result;
}

} // namespace NativeIpc
