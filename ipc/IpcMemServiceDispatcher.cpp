#include "IpcMemServiceDispatcher.h"

#include "IpcApprovalBroker.h"
#include "IpcJsonLimits.h"

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
    if (value == "timed_out_before_start") {
      return RequestCompletion::TimedOutBeforeStart;
    }
    if (value == "timed_out") {
      return RequestCompletion::TimedOut;
    }
    if (value == "cancel_requested") {
        return RequestCompletion::CancelRequested;
    }
    if (value == "completed_after_cancel_request") {
        return RequestCompletion::CompletedAfterCancelRequest;
    }
    if (value == "completed_after_deadline") {
      return RequestCompletion::CompletedAfterDeadline;
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
                        RequestCompletion::TimedOutBeforeStart);
    }
    if (reason == RequestCancelReason::SessionInvalidated) {
        return localError("session_invalidated",
                          "session changed before approval completed",
                          RequestCompletion::CancelledBeforeStart);
    }
    return localError("cancelled", "request cancelled before approval",
                      RequestCompletion::CancelledBeforeStart);
}

IpcDispatchResult executionCancellation(RequestCancelReason reason) {
  if (reason == RequestCancelReason::Deadline) {
    return localError("timeout", "request deadline expired before send",
                      RequestCompletion::TimedOutBeforeStart);
  }
  if (reason == RequestCancelReason::SessionInvalidated) {
    return localError("session_invalidated",
                      "session changed before privileged send",
                      RequestCompletion::CancelledBeforeSend);
  }
  return localError("cancelled", "request cancelled before privileged send",
                    RequestCompletion::CancelledBeforeSend);
}

bool successfulCompletion(RequestCompletion completion) {
  return completion == RequestCompletion::Completed ||
         completion == RequestCompletion::CompletedAfterCancelRequest ||
         completion == RequestCompletion::CompletedAfterDeadline;
}

std::optional<Mem::TargetSnapshot> selectedTarget(const std::string &payload) {
  json parsed;
  std::string error;
  if (!utils::parseBoundedJson(
          payload, kResponseJsonLimits, parsed, error) ||
      !parsed.is_object()) {
    return std::nullopt;
  }
  try {
    Mem::TargetSnapshot target;
    target.pid = parsed.at("pid").get<int>();
    target.processHandle = parsed.at("handle").get<int>();
    target.processRevision = parsed.at("process_revision").get<uint64_t>();
    target.connectionGeneration =
        parsed.at("connection_generation").get<uint64_t>();
    if (!target.isAttached()) {
      return std::nullopt;
    }
    return target;
  } catch (...) {
    return std::nullopt;
  }
}

bool applySelectionCompletion(IpcDispatchResult &result,
                              const IpcRequestContext &context) {
  if (!result.ok) {
    return true;
  }
  const bool cancelled = context.cancellationRequested();
  const bool deadline = context.deadlineExceeded();
  if (!cancelled && !deadline) {
    return true;
  }
  json payload;
  std::string error;
  if (!utils::parseBoundedJson(
          result.resultJson, kResponseJsonLimits, payload, error) ||
      !payload.is_object()) {
    return false;
  }
  const bool cancelledAfterStart =
      cancelled &&
      context.cancellation->reason() != RequestCancelReason::Deadline;
  payload["completed_after_cancel_request"] = cancelledAfterStart;
  payload["completed_after_deadline"] = !cancelledAfterStart && deadline;
  if (cancelledAfterStart) {
    payload["completion"] = "completed_after_cancel_request";
    result.completion = RequestCompletion::CompletedAfterCancelRequest;
  } else {
    payload["completion"] = "completed_after_deadline";
    result.completion = RequestCompletion::CompletedAfterDeadline;
  }
  result.resultJson = payload.dump();
  return true;
}

IpcDispatchResult selectionFailure(
    const IpcDispatchResult &result,
    const char *code,
    const char *message,
    RequestCompletion fallbackCompletion) {
  IpcDispatchResult failed = localError(
      code, message,
      result.completion == RequestCompletion::Completed
          ? fallbackCompletion
          : result.completion);
  return failed;
}

} // namespace

IpcMemServiceDispatcher::IpcMemServiceDispatcher(
    Mem::IMemService &service, IpcApprovalBroker *approvalBroker,
    IpcExternalSession session, IIpcHostMethodExecutor *hostExecutor,
    IIpcExecutionAuditSink *executionAuditSink,
    std::function<bool()> autoApproveLuaExecution)
    : service_(service), tools_(service),
      baseline_(service.captureContext(true)), approvalBroker_(approvalBroker),
      session_(std::move(session)), hostExecutor_(hostExecutor),
      executionAuditSink_(executionAuditSink),
      autoApproveLuaExecution_(std::move(autoApproveLuaExecution)) {}

Mem::OperationContext IpcMemServiceDispatcher::baselineContext() const {
  std::lock_guard<std::mutex> lock(baselineMutex_);
  return baseline_;
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
           descriptor->capability != IpcCapability::Observe &&
           supportsExecution(*descriptor);
}

bool IpcMemServiceDispatcher::shouldAutoApproveLua(
    const std::string &method) const {
  if (method != "lua_execute" || !autoApproveLuaExecution_) {
    return false;
  }
  try {
    return autoApproveLuaExecution_();
  } catch (...) {
    return false;
  }
}

IIpcRequestDispatcher::SessionValidation
IpcMemServiceDispatcher::validateSession() const {
  std::lock_guard<std::mutex> lock(baselineMutex_);
  const Mem::OperationContext current = service_.captureContext(true);
  return validateCurrentLocked(current);
}

IIpcRequestDispatcher::SessionValidation
IpcMemServiceDispatcher::validateCurrentLocked(
    const Mem::OperationContext &current) const {
  if (current.connectionGeneration != baseline_.connectionGeneration) {
    return {false, "connection_changed",
            "connection generation changed; reconnect the IPC session"};
  }
  if (selectionInFlight_) {
    return {};
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
      if (!supportsExecution(*descriptor)) {
        return localError("unsupported",
                          "privileged method has no executable adapter",
                          RequestCompletion::RejectedBeforeStart);
      }
        if (!canSubmitForApproval(request.method)) {
            return localError(
                "approval_required",
                "privileged method requires the GUI approval broker",
                RequestCompletion::RejectedBeforeStart);
        }
        return submitForApproval(request, context, *descriptor);
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

    Mem::OperationContext operation = baselineContext();
    operation.deadline = context.deadline;
    if (context.cancellation != nullptr) {
        operation.cancellation = context.cancellation->booleanToken();
    }

    IpcDispatchResult result;
    try {
      result = invokeMemService(request, operation);
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
    const IpcRequestDto &request, const IpcRequestContext &context,
    const IpcMethodDescriptor &descriptor) {
  const SessionValidation before = validateSession();
  if (!before.valid) {
    return validationFailure(before, RequestCompletion::RejectedBeforeStart);
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
  submission.expected = baselineContext();
  submission.deadline = context.deadline;
  const IpcApprovalSubmissionMode submissionMode =
      shouldAutoApproveLua(request.method)
          ? IpcApprovalSubmissionMode::AutoApproved
          : IpcApprovalSubmissionMode::Pending;
  const IpcApprovalResult submitted =
      approvalBroker_->submit(submission, submissionMode);
  if (!submitted.ok || !submitted.record.has_value()) {
    return localError(submitted.code.empty() ? "approval_submission_failed"
                                             : submitted.code.c_str(),
                      submitted.message.empty()
                          ? "approval request was not accepted"
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
        approvalBroker_->cancelRequest(session_.sessionId, request.requestId);
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
    case IpcApprovalState::Approved: {
      const Mem::OperationContext current = service_.captureContext(true);
      const IpcApprovalResult consumed = approvalBroker_->consume(
          approvalId, session_.sessionId, request.requestId, current);
      if (!consumed.ok || !consumed.grant.has_value()) {
        RequestCompletion completion = RequestCompletion::RejectedBeforeStart;
        if (consumed.code == "approval_expired") {
          completion = RequestCompletion::TimedOutBeforeStart;
        }
        return localError(consumed.code.empty() ? "approval_consume_failed"
                                                : consumed.code.c_str(),
                          consumed.message.empty()
                              ? "approval could not be consumed"
                              : consumed.message.c_str(),
                          completion);
      }
      return executeApproved(request, context, descriptor, *consumed.grant);
    }
    case IpcApprovalState::Denied:
      return localError("approval_denied", "approval was denied",
                        RequestCompletion::RejectedBeforeStart);
    case IpcApprovalState::Invalidated:
      return localError("approval_invalidated",
                        "approval context is no longer current",
                        RequestCompletion::RejectedBeforeStart);
    case IpcApprovalState::Expired:
      return localError("approval_expired", "approval deadline expired",
                        RequestCompletion::TimedOutBeforeStart);
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

IpcDispatchResult IpcMemServiceDispatcher::executeApproved(
    const IpcRequestDto &request, const IpcRequestContext &context,
    const IpcMethodDescriptor &descriptor, const IpcApprovalGrant &grant) {
  if (context.cancellationRequested()) {
    return auditApproved(grant, descriptor,
                         executionCancellation(context.cancellation->reason()));
  }
  if (context.deadlineExceeded()) {
    return auditApproved(
        grant, descriptor,
        executionCancellation(RequestCancelReason::Deadline));
  }
  if (grant.sessionId != session_.sessionId ||
      grant.requestId != request.requestId || grant.method != request.method ||
      grant.capability != descriptor.capability ||
      grant.targetPolicy != descriptor.targetPolicy) {
    return auditApproved(
        grant, descriptor,
        localError("approval_grant_mismatch",
                   "approval grant does not match the active request",
                   RequestCompletion::RejectedBeforeStart));
  }

  Mem::OperationContext operation;
  operation.connectionGeneration = grant.connectionGeneration;
  operation.target = grant.target;
  operation.deadline = context.deadline;
  if (context.cancellation != nullptr) {
    operation.cancellation = context.cancellation->booleanToken();
  }

  const bool selection =
      descriptor.targetPolicy == IpcMethodTargetPolicy::Selection;
  if (selection) {
    SessionValidation validation;
    if (!beginSelection(operation, validation)) {
      return auditApproved(
          grant, descriptor,
          validationFailure(validation, RequestCompletion::RejectedBeforeStart));
    }
  }

  IpcDispatchResult result;
  try {
    result = descriptor.memServiceBacked ? invokeMemService(request, operation)
                                         : invokeHost(request, operation);
  } catch (...) {
    result = localError("dispatcher_exception",
                        "privileged adapter terminated with an exception",
                        RequestCompletion::CompletionUnknown);
  }

  if (selection) {
    if (!applySelectionCompletion(result, context)) {
      result = localError("internal_response_error",
                          "selection adapter returned invalid JSON",
                          RequestCompletion::CompletionUnknown);
    }
    result = finishSelection(result, operation);
  }
  return auditApproved(grant, descriptor, std::move(result));
}

IpcDispatchResult IpcMemServiceDispatcher::auditApproved(
    const IpcApprovalGrant &grant, const IpcMethodDescriptor &descriptor,
    IpcDispatchResult result) const {
  if (executionAuditSink_ == nullptr) {
    return result;
  }

  IpcExecutionAuditRecord record;
  record.approvalId = grant.approvalId;
  record.sessionId = grant.sessionId;
  record.requestId = grant.requestId;
  record.clientName = session_.clientName;
  record.clientVersion = session_.clientVersion;
  record.method = descriptor.name;
  record.capability = descriptor.capability;
  record.targetPolicy = descriptor.targetPolicy;
  record.authorizedConnectionGeneration = grant.connectionGeneration;
  record.authorizedTarget = grant.target;
  record.autoApproved = grant.autoApproved;
  record.success = result.ok;
  record.completion = result.completion;
  if (!result.ok) {
    record.errorCode =
        result.errorCode.empty() || result.errorCode.size() > kMaxErrorCodeBytes
            ? "invalid_error_code"
            : result.errorCode;
  }

  try {
    const Mem::OperationContext observed = service_.captureContext(true);
    record.observedConnectionGeneration = observed.connectionGeneration;
    record.observedTarget = observed.target;
  } catch (...) {
    record.observedConnectionGeneration = 0;
    record.observedTarget.reset();
  }

  try {
    std::string ignoredError;
    executionAuditSink_->recordExecution(record, &ignoredError);
  } catch (...) {
    // An outcome already exists and may represent a sent mutation. Audit
    // failure cannot rewrite that completion or roll the operation back.
  }
  return result;
}

IpcDispatchResult IpcMemServiceDispatcher::invokeMemService(
    const IpcRequestDto &request, const Mem::OperationContext &context) {
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
  } else if (request.method == "driver_initialize") {
    payload = tools_.driverInitialize(request.paramsJson, context);
  } else if (request.method == "process_open") {
    payload = tools_.processOpen(request.paramsJson, context);
  } else if (request.method == "memory_write") {
    payload = tools_.memoryWrite(request.paramsJson, context);
  } else if (request.method == "memory_write_value") {
    payload = tools_.memoryWriteValue(request.paramsJson, context);
  } else if (request.method == "scan_start") {
    payload = tools_.scanStart(request.paramsJson, context);
  } else if (request.method == "scan_refine") {
    payload = tools_.scanRefine(request.paramsJson, context);
  } else if (request.method == "scan_clear") {
    payload = tools_.scanClear(request.paramsJson, context);
  } else if (request.method == "breakpoint_set") {
    payload = tools_.breakpointSet(request.paramsJson, context);
  } else if (request.method == "breakpoint_remove") {
    payload = tools_.breakpointRemove(request.paramsJson, context);
  } else if (request.method == "breakpoint_suspend") {
    payload = tools_.breakpointSuspend(request.paramsJson, context);
  } else if (request.method == "breakpoint_resume") {
    payload = tools_.breakpointResume(request.paramsJson, context);
  } else {
    return localError("unsupported", "MemService method is not adapted",
                      RequestCompletion::RejectedBeforeStart);
  }
  return fromToolJson(payload);
}

IpcDispatchResult
IpcMemServiceDispatcher::invokeHost(const IpcRequestDto &request,
                                    const Mem::OperationContext &context) {
  if (hostExecutor_ == nullptr || !hostExecutor_->supports(request.method)) {
    return localError("unsupported", "host method is not available",
                      RequestCompletion::RejectedBeforeStart);
  }
  return fromToolJson(
      hostExecutor_->execute(request.method, request.paramsJson, context));
}

bool IpcMemServiceDispatcher::supportsExecution(
    const IpcMethodDescriptor &descriptor) const {
  return descriptor.memServiceBacked ||
         (hostExecutor_ != nullptr && hostExecutor_->supports(descriptor.name));
}

bool IpcMemServiceDispatcher::beginSelection(
    const Mem::OperationContext &expected, SessionValidation &validation) {
  std::lock_guard<std::mutex> lock(baselineMutex_);
  const Mem::OperationContext current = service_.captureContext(true);
  validation = validateCurrentLocked(current);
  if (!validation.valid) {
    return false;
  }
  if (selectionInFlight_ ||
      expected.connectionGeneration != baseline_.connectionGeneration ||
      expected.target != baseline_.target) {
    validation = {false, "selection_context_changed",
                  "selection grant no longer matches the IPC baseline"};
    return false;
  }
  selectionInFlight_ = true;
  return true;
}

IpcDispatchResult IpcMemServiceDispatcher::finishSelection(
    const IpcDispatchResult &result, const Mem::OperationContext &expected) {
  std::lock_guard<std::mutex> lock(baselineMutex_);
  selectionInFlight_ = false;
  const Mem::OperationContext current = service_.captureContext(true);

  if (!result.ok) {
    const SessionValidation validation = validateCurrentLocked(current);
    if (validation.valid) {
      return result;
    }
    const auto failedTarget = selectedTarget(result.resultJson);
    if (!failedTarget.has_value() ||
        failedTarget->connectionGeneration != expected.connectionGeneration ||
        current.connectionGeneration != expected.connectionGeneration ||
        !current.target.has_value() ||
        *current.target != *failedTarget) {
      return selectionFailure(
          result, validation.code.empty() ? "session_invalidated"
                                          : validation.code.c_str(),
          validation.message.empty()
              ? "selection failed after the IPC target changed"
              : validation.message.c_str(),
          RequestCompletion::CompletionUnknown);
    }
    baseline_ = current;
    return result;
  }

  const auto target = selectedTarget(result.resultJson);
  if (!target.has_value() ||
      target->connectionGeneration != expected.connectionGeneration ||
      current.connectionGeneration != expected.connectionGeneration ||
      !current.target.has_value() || *current.target != *target) {
    return selectionFailure(
        result, "selection_result_invalid",
        "selected target is incomplete or no longer current",
        RequestCompletion::CompletionUnknown);
  }
  baseline_ = current;
  return result;
}

IpcDispatchResult IpcMemServiceDispatcher::fromToolJson(
    const std::string& payload) {
    json parsed;
    std::string parseError;
    if (!utils::parseBoundedJson(
            payload, kResponseJsonLimits, parsed, parseError) ||
        !parsed.is_object()) {
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
        result.completion = parseCompletion(parsed);
        if (!successfulCompletion(result.completion)) {
          return localError("internal_response_error",
                            "successful adapter returned invalid completion",
                            RequestCompletion::CompletionUnknown);
        }
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
    result.resultJson = payload;
    result.errorMessage = message->get<std::string>();
    const auto retryable = error->find("retryable");
    result.retryable = retryable != error->end() && retryable->is_boolean() &&
                       retryable->get<bool>();
    result.completion = parseCompletion(parsed);
    if (result.completion == RequestCompletion::Completed) {
      if (result.errorCode == "cancel_requested" ||
          result.errorCode == "cancelled") {
        result.completion = RequestCompletion::CancelRequested;
      } else if (result.errorCode == "timeout") {
        result.completion = RequestCompletion::TimedOut;
      } else if (result.errorCode == "completion_unknown") {
        result.completion = RequestCompletion::CompletionUnknown;
      }
    }
    return result;
}

} // namespace NativeIpc
