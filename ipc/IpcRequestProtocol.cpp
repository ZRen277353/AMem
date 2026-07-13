#include "IpcRequestProtocol.h"
#include "IpcJsonLimits.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <limits>

namespace NativeIpc {
namespace {

using json = nlohmann::json;

bool hasAsciiControl(const std::string& value) {
    return std::any_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte < 0x20u || byte == 0x7fu;
    });
}

bool isErrorCode(const std::string& value) {
    if (value.empty() || value.size() > kMaxErrorCodeBytes) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return (byte >= 'a' && byte <= 'z') ||
               (byte >= '0' && byte <= '9') || byte == '_';
    });
}

bool validateErrorFields(const std::string& code,
                         const std::string& message,
                         std::string& error) {
    if (!isErrorCode(code)) {
        error = "error code must be 1..64 lowercase token bytes";
        return false;
    }
    if (message.empty() || message.size() > kMaxErrorMessageBytes) {
        error = "error message must be 1..1024 bytes";
        return false;
    }
    return true;
}

bool dumpBounded(const json& value,
                 uint32_t maxPayloadBytes,
                 std::string& payload,
                 std::string& error) {
    try {
        payload = value.dump();
    } catch (const std::exception& exception) {
        payload.clear();
        error = std::string("JSON serialization failed: ") + exception.what();
        return false;
    }
    const uint32_t limit =
        std::min(maxPayloadBytes, IpcProtocol::kMaxResponsePayloadBytes);
    if (payload.size() > limit) {
        payload.clear();
        error = "response payload exceeds configured limit";
        return false;
    }
    return true;
}

bool isSuccessfulCompletion(RequestCompletion completion) {
  return completion == RequestCompletion::Completed ||
         completion == RequestCompletion::CompletedAfterCancelRequest ||
         completion == RequestCompletion::CompletedAfterDeadline;
}

} // namespace

RequestPayloadParseResult ParseRequestPayload(
    uint64_t requestId,
    const std::string& payload,
    const RequestPayloadConfig& config) {
    RequestPayloadParseResult result;
    if (requestId == 0) {
        result.error = "request id must be non-zero";
        return result;
    }
    if (config.defaultTimeout.count() <= 0 ||
        config.maxTimeout.count() <= 0 ||
        config.defaultTimeout > config.maxTimeout ||
        config.maxTimeout.count() >
            static_cast<int64_t>((std::numeric_limits<uint32_t>::max)())) {
        result.error = "request timeout configuration is invalid";
        return result;
    }

    json request;
    if (!utils::parseBoundedJson(
            payload, kRequestJsonLimits, request, result.error) ||
        !request.is_object()) {
        if (result.error.empty()) {
            result.error = "request payload must be a JSON object";
        }
        return result;
    }
    constexpr std::array<const char*, 3> allowedFields = {
        "method", "params", "timeout_ms"};
    for (auto it = request.begin(); it != request.end(); ++it) {
        if (std::find(allowedFields.begin(), allowedFields.end(), it.key()) ==
            allowedFields.end()) {
            result.error = "request contains an unknown field";
            return result;
        }
    }

    const auto methodIt = request.find("method");
    if (methodIt == request.end() || !methodIt->is_string()) {
        result.error = "method must be a string";
        return result;
    }
    result.request.method = methodIt->get<std::string>();
    if (result.request.method.empty() ||
        result.request.method.size() > kMaxRequestMethodBytes ||
        hasAsciiControl(result.request.method)) {
        result.error =
            "method must be 1..128 bytes without control characters";
        return result;
    }

    const auto paramsIt = request.find("params");
    if (paramsIt == request.end() || !paramsIt->is_object()) {
        result.error = "params must be a JSON object";
        return result;
    }
    result.request.paramsJson = paramsIt->dump();

    result.request.timeout = config.defaultTimeout;
    const auto timeoutIt = request.find("timeout_ms");
    if (timeoutIt != request.end()) {
        if (!timeoutIt->is_number_unsigned()) {
            result.error = "timeout_ms must be an unsigned integer";
            return result;
        }
        const uint64_t timeoutMs = timeoutIt->get<uint64_t>();
        if (timeoutMs == 0 ||
            timeoutMs > static_cast<uint64_t>(config.maxTimeout.count())) {
            result.error = "timeout_ms is outside the allowed range";
            return result;
        }
        result.request.timeout =
            std::chrono::milliseconds(static_cast<int64_t>(timeoutMs));
    }

    result.request.requestId = requestId;
    result.valid = true;
    return result;
}

bool ValidateCancelPayload(const std::string& payload,
                           std::string& error) {
    error.clear();
    json cancel;
    if (!utils::parseBoundedJson(
            payload, kRequestJsonLimits, cancel, error)) {
        return false;
    }
    if (!cancel.is_object() || !cancel.empty()) {
        error = "cancel payload must be an empty JSON object";
        return false;
    }
    return true;
}

const char* RequestCompletionName(RequestCompletion completion) {
    switch (completion) {
    case RequestCompletion::RejectedBeforeStart:
        return "rejected_before_start";
    case RequestCompletion::CancelledBeforeStart:
        return "cancelled_before_start";
    case RequestCompletion::CancelledBeforeSend:
        return "cancelled_before_send";
    case RequestCompletion::TimedOutBeforeStart:
      return "timed_out_before_start";
    case RequestCompletion::TimedOut:
      return "timed_out";
    case RequestCompletion::CancelRequested:
        return "cancel_requested";
    case RequestCompletion::CompletedAfterCancelRequest:
        return "completed_after_cancel_request";
    case RequestCompletion::CompletedAfterDeadline:
      return "completed_after_deadline";
    case RequestCompletion::CompletionUnknown:
        return "completion_unknown";
    case RequestCompletion::Completed:
        return "completed";
    }
    return "completion_unknown";
}

bool BuildResponsePayload(const IpcDispatchResult& result,
                          std::string& payload,
                          std::string& error,
                          uint32_t maxPayloadBytes) {
    payload.clear();
    error.clear();
    if (result.ok && !isSuccessfulCompletion(result.completion)) {
        error = "successful response has an incompatible completion state";
        return false;
    }

    json response = {
        {"ok", result.ok},
        {"completion", RequestCompletionName(result.completion)},
    };
    if (result.ok) {
        if (result.resultJson.size() > IpcProtocol::kMaxResponsePayloadBytes) {
            error = "result JSON exceeds the response limit";
            return false;
        }
        json value;
        if (!utils::parseBoundedJson(
                result.resultJson, kResponseJsonLimits, value, error)) {
            return false;
        }
        response["result"] = std::move(value);
    } else {
        if (!validateErrorFields(result.errorCode, result.errorMessage,
                                 error)) {
            return false;
        }
        response["error"] = {
            {"code", result.errorCode},
            {"message", result.errorMessage},
            {"retryable", result.retryable},
        };
    }
    return dumpBounded(response, maxPayloadBytes, payload, error);
}

bool BuildErrorPayload(const std::string& code,
                       const std::string& message,
                       std::string& payload,
                       std::string& error,
                       uint32_t maxPayloadBytes) {
    payload.clear();
    error.clear();
    if (!validateErrorFields(code, message, error)) {
        return false;
    }
    return dumpBounded({{"code", code}, {"message", message}},
                       maxPayloadBytes, payload, error);
}

} // namespace NativeIpc
