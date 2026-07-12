#pragma once

#include "IpcProtocol.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace NativeIpc {

constexpr size_t kMaxRequestMethodBytes = 128;
constexpr uint32_t kDefaultRequestTimeoutMs = 30u * 1000u;
constexpr uint32_t kMaxRequestTimeoutMs = 5u * 60u * 1000u;
constexpr size_t kMaxErrorCodeBytes = 64;
constexpr size_t kMaxErrorMessageBytes = 1024;

struct RequestPayloadConfig {
    std::chrono::milliseconds defaultTimeout{kDefaultRequestTimeoutMs};
    std::chrono::milliseconds maxTimeout{kMaxRequestTimeoutMs};
};

struct IpcRequestDto {
    uint64_t requestId = 0;
    std::string method;
    std::string paramsJson;
    std::chrono::milliseconds timeout{0};
};

struct RequestPayloadParseResult {
    bool valid = false;
    IpcRequestDto request;
    std::string error;
};

RequestPayloadParseResult ParseRequestPayload(
    uint64_t requestId,
    const std::string& payload,
    const RequestPayloadConfig& config = {});

bool ValidateCancelPayload(const std::string& payload,
                           std::string& error);

enum class RequestCompletion {
    CancelledBeforeStart,
    CancelledBeforeSend,
    CancelRequested,
    CompletedAfterCancelRequest,
    CompletionUnknown,
    Completed,
};

const char* RequestCompletionName(RequestCompletion completion);

struct IpcDispatchResult {
    bool ok = false;
    std::string resultJson = "null";
    std::string errorCode;
    std::string errorMessage;
    RequestCompletion completion = RequestCompletion::Completed;
};

bool BuildResponsePayload(
    const IpcDispatchResult& result,
    std::string& payload,
    std::string& error,
    uint32_t maxPayloadBytes = IpcProtocol::kMaxResponsePayloadBytes);

bool BuildErrorPayload(
    const std::string& code,
    const std::string& message,
    std::string& payload,
    std::string& error,
    uint32_t maxPayloadBytes = IpcProtocol::kMaxResponsePayloadBytes);

} // namespace NativeIpc
