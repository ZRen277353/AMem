#include "IpcMemServiceDispatcher.h"

#include <nlohmann/json.hpp>

#include <exception>

namespace NativeIpc {
namespace {

using json = nlohmann::json;

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

} // namespace

IpcMemServiceDispatcher::IpcMemServiceDispatcher(Mem::IMemService& service)
    : service_(service), tools_(service), baseline_(service.captureContext(true)) {
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
        return localError(
            "approval_required",
            "privileged method requires the GUI approval broker",
            RequestCompletion::RejectedBeforeStart);
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
