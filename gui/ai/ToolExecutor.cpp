#ifdef HAVE_AI_CHAT

#include "ToolExecutor.h"

#include "../../third_party/nlohmann/json.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <future>
#include <iterator>
#include <sstream>
#include <string>
#include <utility>

namespace AI {

namespace {

using nlohmann::json;

// Join a container of strings with ", " — used for the "available tools"
// hint returned when the AI invokes an unknown tool (AC 5.5).
std::string joinNames(const std::vector<std::string>& names) {
    std::string out;
    for (size_t i = 0; i < names.size(); ++i) {
        if (i) out += ", ";
        out += names[i];
    }
    return out;
}

// Return the JSON-Schema "type" string corresponding to an actual JSON value.
// Numbers are reported as "number" so that a schema expecting "integer" will
// not be silently accepted when it receives a float; validateType() handles
// the integer/number relationship explicitly.
std::string jsonTypeName(const json& value) {
    switch (value.type()) {
        case json::value_t::null:            return "null";
        case json::value_t::boolean:         return "boolean";
        case json::value_t::number_integer:
        case json::value_t::number_unsigned: return "integer";
        case json::value_t::number_float:    return "number";
        case json::value_t::string:          return "string";
        case json::value_t::array:           return "array";
        case json::value_t::object:          return "object";
        default:                             return "unknown";
    }
}

// Check whether an actual JSON value satisfies a schema's "type" string.
// Accepts the small subset needed by the built-in tools: object, string,
// integer, number, boolean, array. "number" matches both ints and floats;
// "integer" only matches whole-number values.
bool matchesType(const json& value, const std::string& expected) {
    if (expected == "object")  return value.is_object();
    if (expected == "string")  return value.is_string();
    if (expected == "boolean") return value.is_boolean();
    if (expected == "array")   return value.is_array();
    if (expected == "integer") return value.is_number_integer() || value.is_number_unsigned();
    if (expected == "number")  return value.is_number();
    // Unknown type keyword — be permissive rather than rejecting real data.
    return true;
}

// Minimal JSON-Schema validator. Supports: type, required, properties,
// minimum, maximum, minLength, maxLength. Returns an empty string on
// success, otherwise a human-readable description of the first violation
// encountered (AC 5.6). The path argument is used to build
// "root.foo.bar"-style property locators in error messages.
std::string validateAgainstSchema(const json& args, const json& schema, const std::string& path) {
    if (!schema.is_object()) {
        return ""; // nothing to validate against
    }

    // type check
    if (schema.contains("type") && schema["type"].is_string()) {
        const std::string expected = schema["type"].get<std::string>();
        if (!matchesType(args, expected)) {
            std::ostringstream oss;
            oss << "expected " << expected << " at " << path
                << " but got " << jsonTypeName(args);
            return oss.str();
        }
    }

    // string constraints
    if (args.is_string()) {
        const auto& s = args.get_ref<const std::string&>();
        if (schema.contains("minLength") && schema["minLength"].is_number_integer()) {
            const auto minLen = schema["minLength"].get<std::size_t>();
            if (s.size() < minLen) {
                std::ostringstream oss;
                oss << path << " shorter than minLength " << minLen;
                return oss.str();
            }
        }
        if (schema.contains("maxLength") && schema["maxLength"].is_number_integer()) {
            const auto maxLen = schema["maxLength"].get<std::size_t>();
            if (s.size() > maxLen) {
                std::ostringstream oss;
                oss << path << " longer than maxLength " << maxLen;
                return oss.str();
            }
        }
    }

    // numeric constraints
    if (args.is_number()) {
        const double v = args.get<double>();
        if (schema.contains("minimum") && schema["minimum"].is_number()) {
            const double minV = schema["minimum"].get<double>();
            if (v < minV) {
                std::ostringstream oss;
                oss << path << " below minimum " << minV;
                return oss.str();
            }
        }
        if (schema.contains("maximum") && schema["maximum"].is_number()) {
            const double maxV = schema["maximum"].get<double>();
            if (v > maxV) {
                std::ostringstream oss;
                oss << path << " above maximum " << maxV;
                return oss.str();
            }
        }
    }

    // object constraints: required + properties recursion
    if (args.is_object()) {
        if (schema.contains("required") && schema["required"].is_array()) {
            for (const auto& req : schema["required"]) {
                if (!req.is_string()) continue;
                const auto key = req.get<std::string>();
                if (!args.contains(key)) {
                    std::ostringstream oss;
                    oss << "missing required property '" << key << "' at " << path;
                    return oss.str();
                }
            }
        }
        if (schema.contains("properties") && schema["properties"].is_object()) {
            for (auto it = schema["properties"].begin(); it != schema["properties"].end(); ++it) {
                const auto& key = it.key();
                if (!args.contains(key)) continue; // absent optional fields skip further checks
                const std::string childPath = path + "." + key;
                const std::string err = validateAgainstSchema(args[key], it.value(), childPath);
                if (!err.empty()) return err;
            }
        }
    }

    return "";
}

// Entry point used by ToolExecutor::execute(). Parses the schema string from
// the ToolDefinition and dispatches to the recursive validator. A malformed
// schema is treated as "no constraints" to avoid blocking execution when the
// registration side has a bug; such cases are caught during development.
std::string validateAgainstSchema(const json& args, const std::string& schemaStr) {
    if (schemaStr.empty()) return "";
    json schema;
    try {
        schema = json::parse(schemaStr);
    } catch (const std::exception&) {
        return ""; // tolerate broken schema — registration-side concern
    }
    return validateAgainstSchema(args, schema, "root");
}

std::string extractToolError(const std::string& resultJson) {
    if (resultJson.empty()) return "";

    try {
        const json result = json::parse(resultJson);
        if (!result.is_object() || !result.contains("error")) {
            return "";
        }

        const json& err = result["error"];
        if (err.is_null()) {
            return "";
        }
        if (err.is_string()) {
            return err.get<std::string>();
        }
        if (err.is_boolean() && !err.get<bool>()) {
            return "";
        }
        return err.dump();
    } catch (const std::exception&) {
        return "";
    }
}

} // namespace

void ToolExecutor::registerTool(const std::string& name,
                                const std::string& description,
                                const std::string& parametersSchema,
                                ToolSafety safety,
                                std::function<std::string(const std::string&)> executor) {
    // Enforce AC 5.2 invariants by truncation so the registry can never hold
    // an over-length entry, regardless of caller discipline.
    ToolRegistration reg;
    reg.definition.name = name.size() > 64 ? name.substr(0, 64) : name;
    reg.definition.description = description.size() > 256 ? description.substr(0, 256) : description;
    reg.definition.parametersSchema = parametersSchema;
    reg.safety = safety;
    reg.executor = std::move(executor);

    std::lock_guard<std::mutex> lock(mutex_);
    tools_[reg.definition.name] = std::move(reg);
}

// Note: initBuiltinTools() is defined in ToolDefinitions.cpp (task 5.2) so
// that the executor core remains free of socket/tool-implementation
// dependencies. Intentionally not defined here.

ToolResult ToolExecutor::execute(const ToolCall& call) {
    // Snapshot the registration under the mutex, then release the lock
    // before invoking the executor — executors may take a long time and
    // must not serialise unrelated registry queries.
    ToolRegistration registration;
    int timeoutSeconds;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = tools_.find(call.name);
        if (it == tools_.end()) {
            std::vector<std::string> names;
            names.reserve(tools_.size());
            for (const auto& kv : tools_) names.push_back(kv.first);
            ToolResult result;
            result.success = false;
            result.errorMessage = "Unrecognized tool '" + call.name +
                                  "'. Available tools: " + joinNames(names);
            return result;
        }
        registration = it->second;
        timeoutSeconds = executionTimeout_;
    }

    // Parse the arguments JSON. Providers occasionally emit malformed JSON
    // (e.g. trailing text); we surface that as a structured error instead
    // of letting a parse exception escape (AC 5.6 / 5.8).
    json args;
    if (call.arguments.empty()) {
        args = json::object();
    } else {
        try {
            args = json::parse(call.arguments);
        } catch (const std::exception& e) {
            ToolResult result;
            result.success = false;
            result.errorMessage = std::string("Invalid JSON in arguments for tool '") +
                                  call.name + "': " + e.what();
            return result;
        }
    }

    // Schema validation (AC 5.6).
    const std::string validationError = validateAgainstSchema(args, registration.definition.parametersSchema);
    if (!validationError.empty()) {
        ToolResult result;
        result.success = false;
        result.errorMessage = "Validation failed: " + validationError;
        return result;
    }

    if (!registration.executor) {
        ToolResult result;
        result.success = false;
        result.errorMessage = "Tool '" + call.name + "' has no executor registered";
        return result;
    }

    // Invoke the executor asynchronously so we can enforce a wall-clock
    // timeout (AC 6.6). std::async with launch::async guarantees a separate
    // thread; the returned future's destructor will block if we abandoned
    // it, but that is acceptable because we always wait_for() here and only
    // give up on timeout — in which case we deliberately let the detached
    // work finish on its own rather than introducing a stuck join.
    ToolResult result;
    try {
        std::shared_future<std::string> fut =
            std::async(std::launch::async,
                       [executor = registration.executor, argsJson = call.arguments]() {
                           return executor(argsJson);
                       }).share();
        if (fut.wait_for(std::chrono::seconds(timeoutSeconds)) == std::future_status::timeout) {
            result.success = false;
            result.errorMessage = "Tool '" + call.name + "' execution timed out after " +
                                  std::to_string(timeoutSeconds) + " seconds";
            {
                std::lock_guard<std::mutex> lock(activeFuturesMutex_);
                activeFutures_.erase(
                    std::remove_if(activeFutures_.begin(), activeFutures_.end(),
                                   [](const std::shared_future<std::string>& f) {
                                       return f.wait_for(std::chrono::seconds(0)) ==
                                              std::future_status::ready;
                                   }),
                    activeFutures_.end());
                activeFutures_.push_back(fut);
            }
            return result;
        }
        result.resultJson = fut.get();
        result.errorMessage = extractToolError(result.resultJson);
        result.success = result.errorMessage.empty();
    } catch (const std::exception& e) {
        result.success = false;
        result.resultJson.clear();
        result.errorMessage = std::string("Tool execution error: ") + e.what();
    } catch (...) {
        result.success = false;
        result.resultJson.clear();
        result.errorMessage = "Tool execution error: unknown exception";
    }
    return result;
}

std::vector<ToolDefinition> ToolExecutor::getToolDefinitions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ToolDefinition> defs;
    defs.reserve(tools_.size());
    for (const auto& kv : tools_) {
        defs.push_back(kv.second.definition);
    }
    return defs;
}

ToolSafety ToolExecutor::getToolSafety(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tools_.find(name);
    if (it == tools_.end()) {
        // Safe default: unknown tools are treated as read-only rather than
        // write, because an unknown tool cannot execute anyway and we do
        // not want to trigger a confirmation prompt for something that
        // will be rejected by execute() with an "unrecognized" error.
        return ToolSafety::ReadOnly;
    }
    return it->second.safety;
}

void ToolExecutor::setExecutionTimeout(int seconds) {
    if (seconds < 1)   seconds = 1;
    if (seconds > 300) seconds = 300;
    std::lock_guard<std::mutex> lock(mutex_);
    executionTimeout_ = seconds;
}

int ToolExecutor::getExecutionTimeout() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return executionTimeout_;
}

} // namespace AI

#endif // HAVE_AI_CHAT
