#include "LuaJsonTool.h"

#include "../lua/LuaEngine.h"
#include "../socket/socket_io_timeout.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <stdexcept>
#include <string>

namespace Mem {
namespace {

using json = nlohmann::json;

constexpr size_t kMaxLuaCodeBytes = 256u * 1024u;
constexpr int kDefaultLuaTimeoutSeconds = 30;
constexpr int kMaxLuaTimeoutSeconds = 300;

std::string errorJson(const std::string &code, const std::string &message,
                      bool retryable, const char *completion,
                      const std::string *output = nullptr) {
  json value;
  value["success"] = false;
  value["error"] = {
      {"code", code},
      {"message", message},
      {"retryable", retryable},
  };
  value["completion"] = completion;
  if (output != nullptr) {
    value["output"] = *output;
  }
  return value.dump();
}

std::string contextErrorJson(const Mem::OperationContext &expected,
                             const Mem::OperationContext &current) {
  if (current.connectionGeneration != expected.connectionGeneration) {
    return errorJson("connection_changed",
                     "connection changed before Lua execution", true,
                     "rejected_before_start");
  }
  if (!expected.target || !expected.target->isAttached()) {
    return errorJson("no_target",
                     "Lua execution requires an attached target process", false,
                     "rejected_before_start");
  }
  if (expected.target->connectionGeneration != expected.connectionGeneration) {
    return errorJson("connection_changed",
                     "Lua target belongs to another connection generation",
                     true, "rejected_before_start");
  }
  if (!current.target || *current.target != *expected.target) {
    return errorJson("target_changed", "target changed before Lua execution",
                     false, "rejected_before_start");
  }
  return {};
}

std::string preflightJson(IMemService &service,
                          const OperationContext &context) {
  if (context.cancellation &&
      context.cancellation->load(std::memory_order_acquire)) {
    return errorJson("cancel_requested",
                     "Lua execution was cancelled before it started", false,
                     "cancelled_before_start");
  }
  if (std::chrono::steady_clock::now() >= context.deadline) {
    return errorJson("timeout",
                     "Lua execution deadline expired before it started", false,
                     "timed_out_before_start");
  }
  return contextErrorJson(context, service.captureContext(true));
}

std::string luaCode(const json &args) {
  if (!args.contains("code") || !args.at("code").is_string()) {
    throw std::runtime_error("code must be a string");
  }
  const std::string code = args.at("code").get<std::string>();
  if (code.empty() ||
      std::all_of(code.begin(), code.end(),
                  [](unsigned char byte) { return std::isspace(byte) != 0; })) {
    throw std::runtime_error("code must not be empty");
  }
  if (code.size() > kMaxLuaCodeBytes) {
    throw std::runtime_error("code exceeds 262144 bytes");
  }
  return code;
}

int luaTimeoutSeconds(const json &args) {
  if (!args.contains("timeout_seconds") ||
      args.at("timeout_seconds").is_null()) {
    return kDefaultLuaTimeoutSeconds;
  }
  const json &value = args.at("timeout_seconds");
  if (!value.is_number_integer() && !value.is_number_unsigned()) {
    throw std::runtime_error("timeout_seconds must be an integer");
  }
  const long long seconds = value.get<long long>();
  if (seconds < 1 || seconds > kMaxLuaTimeoutSeconds) {
    throw std::runtime_error("timeout_seconds must be between 1 and 300");
  }
  return static_cast<int>(seconds);
}

} // namespace

std::string executeLuaJson(IMemService &service, const std::string &argsJson,
                           const OperationContext &context) {
  try {
    const json args = json::parse(argsJson.empty() ? "{}" : argsJson);
    if (!args.is_object()) {
      throw std::runtime_error("arguments must be an object");
    }
    const std::string code = luaCode(args);
    const int timeoutSeconds = luaTimeoutSeconds(args);

    if (const std::string error = preflightJson(service, context);
        !error.empty()) {
      return error;
    }

    const auto requestedDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);
    const auto effectiveDeadline =
        (std::min)(requestedDeadline, context.deadline);
    if (std::chrono::steady_clock::now() >= effectiveDeadline) {
      return errorJson("timeout",
                       "Lua execution deadline expired before it started",
                       false, "timed_out_before_start");
    }

    SocketIoTimeout::ScopedTimeout luaTimeout(effectiveDeadline);
    LuaEngine &engine = LuaEngine::GetInstance();
    if (!engine.Initialize(service)) {
      return errorJson("internal_error",
                       "Lua engine initialization failed: " +
                           engine.GetLastError(),
                       false, "rejected_before_start");
    }
    if (const std::string error = preflightJson(service, context);
        !error.empty()) {
      return error;
    }

    std::string output;
    if (!engine.ExecuteStringCapture(
            code, "agent_tool", output,
            static_cast<int>(SocketIoTimeout::GetRemainingTimeoutMs()),
            &context)) {
      const bool timedOut = engine.GetLastError() == "Lua execution timed out";
      return errorJson(timedOut ? "timeout" : "internal_error",
                       engine.GetLastError(), false,
                       timedOut ? "timed_out" : "completed", &output);
    }

    const bool completedAfterCancel =
        context.cancellation &&
        context.cancellation->load(std::memory_order_acquire);
    const bool completedAfterDeadline =
        std::chrono::steady_clock::now() >= context.deadline;
    json result;
    result["success"] = true;
    result["output"] = output;
    result["completed_after_cancel_request"] = completedAfterCancel;
    result["completed_after_deadline"] = completedAfterDeadline;
    result["completion"] = completedAfterCancel
                               ? "completed_after_cancel_request"
                           : completedAfterDeadline ? "completed_after_deadline"
                                                    : "completed";
    return result.dump();
  } catch (const std::exception &error) {
    return errorJson("invalid_argument",
                     std::string("lua_execute: ") + error.what(), false,
                     "rejected_before_start");
  }
}

} // namespace Mem
