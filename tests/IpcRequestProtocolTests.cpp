#include "ipc/IpcRequestProtocol.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using json = nlohmann::json;
using namespace std::chrono_literals;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

NativeIpc::RequestPayloadParseResult parse(
    const json& payload,
    uint64_t requestId = 7,
    NativeIpc::RequestPayloadConfig config = {}) {
    return NativeIpc::ParseRequestPayload(requestId, payload.dump(), config);
}

void testValidRequestAndDefaultTimeout() {
    const auto defaulted = parse({{"method", "status"},
                                  {"params", json::object()}});
    expect(defaulted.valid && defaulted.request.requestId == 7 &&
               defaulted.request.method == "status" &&
               json::parse(defaulted.request.paramsJson).empty() &&
               defaulted.request.timeout == 30s,
           "valid request should use the default timeout");

    const auto explicitTimeout = parse(
        {{"method", "memory_read"},
         {"params", {{"address", "0x1000"}, {"size", 16}}},
         {"timeout_ms", 1250}});
    expect(explicitTimeout.valid &&
               explicitTimeout.request.timeout == 1250ms &&
               json::parse(explicitTimeout.request.paramsJson)["size"] == 16,
           "explicit timeout and params should survive parsing");
}

void testStrictRequestSchema() {
    expect(!NativeIpc::ParseRequestPayload(0, "{}").valid,
           "zero request id must be rejected");
    expect(!NativeIpc::ParseRequestPayload(1, "[]").valid,
           "request must be an object");
    expect(!parse({{"params", json::object()}}).valid,
           "missing method must be rejected");
    expect(!parse({{"method", "status"}, {"params", json::array()}}).valid,
           "params must be an object");
    expect(!parse({{"method", "status"},
                   {"params", json::object()},
                   {"capability", "Observe"}})
                .valid,
           "client-provided capability fields must be rejected");
    expect(!parse({{"method", std::string(129, 'a')},
                   {"params", json::object()}})
                .valid,
           "oversized method must be rejected");
    expect(!parse({{"method", std::string("bad\nmethod")},
                   {"params", json::object()}})
                .valid,
           "method controls must be rejected");
}

void testTimeoutBoundsAndConfiguration() {
    expect(!parse({{"method", "status"},
                   {"params", json::object()},
                   {"timeout_ms", 0}})
                .valid,
           "zero timeout must be rejected");
    expect(!parse({{"method", "status"},
                   {"params", json::object()},
                   {"timeout_ms", 300001}})
                .valid,
           "timeout above the hard default maximum must be rejected");
    expect(!parse({{"method", "status"},
                   {"params", json::object()},
                   {"timeout_ms", 1.5}})
                .valid,
           "fractional timeout must be rejected");
    expect(!parse({{"method", "status"},
                   {"params", json::object()}},
                  7, {5s, 1s})
                .valid,
           "invalid timeout configuration must fail closed");
}

void testCancelPayloadIsStrictEmptyObject() {
    std::string error;
    expect(NativeIpc::ValidateCancelPayload("{}", error),
           "empty cancel object should be valid");
    expect(!NativeIpc::ValidateCancelPayload("", error) &&
               !NativeIpc::ValidateCancelPayload("[]", error) &&
               !NativeIpc::ValidateCancelPayload("{\"reason\":\"stop\"}",
                                                  error),
           "cancel payload must be exactly an empty object");
}

void testResponseEnvelopeAndCompletionNames() {
    std::string payload;
    std::string error;
    NativeIpc::IpcDispatchResult success;
    success.ok = true;
    success.resultJson = "{\"connected\":true}";
    success.completion = NativeIpc::RequestCompletion::Completed;
    expect(NativeIpc::BuildResponsePayload(success, payload, error),
           "success response should serialize: " + error);
    const json successJson = json::parse(payload);
    expect(successJson["ok"] == true &&
               successJson["completion"] == "completed" &&
               successJson["result"]["connected"] == true,
           "success response fields should be stable");

    NativeIpc::IpcDispatchResult failure;
    failure.errorCode = "cancelled";
    failure.errorMessage = "request cancellation was observed";
    failure.retryable = true;
    failure.completion = NativeIpc::RequestCompletion::CancelRequested;
    expect(NativeIpc::BuildResponsePayload(failure, payload, error),
           "failure response should serialize: " + error);
    const json failureJson = json::parse(payload);
    expect(failureJson["ok"] == false &&
               failureJson["completion"] == "cancel_requested" &&
               failureJson["error"]["code"] == "cancelled" &&
               failureJson["error"]["retryable"] == true,
           "failure response fields should be stable");

    expect(std::string(NativeIpc::RequestCompletionName(
               NativeIpc::RequestCompletion::CompletionUnknown)) ==
               "completion_unknown",
           "completion names must match native operation outcomes");
    expect(std::string(NativeIpc::RequestCompletionName(
               NativeIpc::RequestCompletion::RejectedBeforeStart)) ==
               "rejected_before_start",
           "approval rejection must have a stable completion name");
}

void testResponseValidationAndLimits() {
    std::string payload;
    std::string error;
    NativeIpc::IpcDispatchResult invalidJson;
    invalidJson.ok = true;
    invalidJson.resultJson = "{";
    expect(!NativeIpc::BuildResponsePayload(invalidJson, payload, error),
           "invalid dispatcher JSON must be rejected");

    NativeIpc::IpcDispatchResult invalidCompletion;
    invalidCompletion.ok = true;
    invalidCompletion.resultJson = "null";
    invalidCompletion.completion =
        NativeIpc::RequestCompletion::CompletionUnknown;
    expect(!NativeIpc::BuildResponsePayload(invalidCompletion, payload, error),
           "success must not claim an unknown completion");

    NativeIpc::IpcDispatchResult invalidError;
    invalidError.errorCode = "Bad-Code";
    invalidError.errorMessage = "bad";
    expect(!NativeIpc::BuildResponsePayload(invalidError, payload, error),
           "error codes must use stable lowercase tokens");

    expect(!NativeIpc::BuildErrorPayload("invalid_request", "bad request",
                                        payload, error, 8),
           "protocol errors must honor the configured output limit");
}

} // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"valid request and default timeout",
         &testValidRequestAndDefaultTimeout},
        {"strict request schema", &testStrictRequestSchema},
        {"timeout bounds and configuration",
         &testTimeoutBoundsAndConfiguration},
        {"strict cancel payload", &testCancelPayloadIsStrictEmptyObject},
        {"response envelope and completion names",
         &testResponseEnvelopeAndCompletionNames},
        {"response validation and limits", &testResponseValidationAndLimits},
    };

    int failures = 0;
    for (const auto& test : tests) {
        try {
            test.second();
            std::cout << "[PASS] " << test.first << '\n';
        } catch (const std::exception& exception) {
            ++failures;
            std::cerr << "[FAIL] " << test.first << ": "
                      << exception.what() << '\n';
        }
    }
    if (failures != 0) {
        std::cerr << failures << " test group(s) failed\n";
        return 1;
    }
    std::cout << tests.size() << " test groups passed\n";
    return 0;
}
