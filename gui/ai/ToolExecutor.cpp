#ifdef HAVE_AI_CHAT

#include "ToolExecutor.h"

#include "../../socket/socket_io_timeout.h"
#include "../../third_party/nlohmann/json.hpp"

#include <chrono>
#include <exception>
#include <future>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
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
// anyOf, items, minimum, maximum, minLength, maxLength, minItems, maxItems.
// Returns an empty string on
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

    // array constraints
    if (args.is_array()) {
        if (schema.contains("minItems") && schema["minItems"].is_number_integer()) {
            const auto minItems = schema["minItems"].get<std::size_t>();
            if (args.size() < minItems) {
                std::ostringstream oss;
                oss << path << " has fewer items than minItems " << minItems;
                return oss.str();
            }
        }
        if (schema.contains("maxItems") && schema["maxItems"].is_number_integer()) {
            const auto maxItems = schema["maxItems"].get<std::size_t>();
            if (args.size() > maxItems) {
                std::ostringstream oss;
                oss << path << " has more items than maxItems " << maxItems;
                return oss.str();
            }
        }
        if (schema.contains("items") && schema["items"].is_object()) {
            const json& itemSchema = schema["items"];
            for (std::size_t i = 0; i < args.size(); ++i) {
                const std::string itemPath = path + "[" + std::to_string(i) + "]";
                const std::string err = validateAgainstSchema(args[i], itemSchema, itemPath);
                if (!err.empty()) {
                    return err;
                }
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

    // composite constraints
    if (schema.contains("anyOf") && schema["anyOf"].is_array()) {
        std::string firstError;
        bool matched = false;
        int alternatives = 0;
        for (const auto& alternative : schema["anyOf"]) {
            if (!alternative.is_object()) {
                continue;
            }
            ++alternatives;
            const std::string err =
                validateAgainstSchema(args, alternative, path);
            if (err.empty()) {
                matched = true;
                break;
            }
            if (firstError.empty()) {
                firstError = err;
            }
        }
        if (!matched && alternatives > 0) {
            std::ostringstream oss;
            oss << path << " did not match any accepted argument form";
            if (!firstError.empty()) {
                oss << " (first mismatch: " << firstError << ")";
            }
            return oss.str();
        }
    }

    return "";
}

// Entry point used by ToolExecutor::execute(). Parses the schema string from
// the ToolDefinition and dispatches to the recursive validator.
std::string validateAgainstSchema(const json& args, const std::string& schemaStr) {
    if (schemaStr.empty()) return "";
    json schema;
    try {
        schema = json::parse(schemaStr);
    } catch (const std::exception& e) {
        return std::string("invalid tool parameter schema: ") + e.what();
    }
    if (!schema.is_object()) {
        return "invalid tool parameter schema: root schema must be an object";
    }
    return validateAgainstSchema(args, schema, "root");
}

std::string jsonValueToErrorString(const json& value) {
    if (value.is_null()) {
        return "";
    }
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_boolean() && !value.get<bool>()) {
        return "";
    }
    return value.dump();
}

std::string extractToolError(const std::string& resultJson) {
    if (resultJson.empty()) return "";

    try {
        const json result = json::parse(resultJson);
        if (!result.is_object()) {
            return "";
        }

        if (result.contains("success") && result["success"].is_boolean() &&
            !result["success"].get<bool>()) {
            if (result.contains("error")) {
                const std::string err = jsonValueToErrorString(result["error"]);
                if (!err.empty()) {
                    return err;
                }
            }
            return "tool returned success=false";
        }

        if (!result.contains("error")) {
            return "";
        }
        return jsonValueToErrorString(result["error"]);
    } catch (const std::exception& e) {
        return std::string("tool returned invalid JSON: ") + e.what();
    }
}

std::shared_future<std::string> runExecutorAsync(
    std::function<std::string(const std::string&)> executor,
    std::string argsJson,
    int timeoutSeconds) {
    auto promise = std::make_shared<std::promise<std::string>>();
    std::shared_future<std::string> future = promise->get_future().share();

    std::thread([promise,
                 executor = std::move(executor),
                 argsJson = std::move(argsJson),
                 timeoutSeconds]() mutable {
        try {
            SocketIoTimeout::ScopedTimeout socketTimeout(timeoutSeconds);
            promise->set_value(executor(argsJson));
        } catch (...) {
            promise->set_exception(std::current_exception());
        }
    }).detach();

    return future;
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

    const std::string normalizedArgsJson = args.dump();

    // Invoke the executor asynchronously. Read-only tools enforce a
    // wall-clock timeout (AC 6.6); write-classified tools wait for the real
    // result in this background thread so the agent never continues from an
    // ambiguous "timed out but may still commit" target state.
    ToolResult result;
    try {
        std::shared_future<std::string> fut =
            runExecutorAsync(registration.executor, normalizedArgsJson, timeoutSeconds);
        if (fut.wait_for(std::chrono::seconds(timeoutSeconds)) == std::future_status::timeout) {
            if (registration.safety == ToolSafety::Write) {
                result.resultJson = fut.get();
                result.errorMessage = extractToolError(result.resultJson);
                result.success = result.errorMessage.empty();
                return result;
            }

            result.success = false;
            result.errorMessage = "Tool '" + call.name + "' execution timed out after " +
                                  std::to_string(timeoutSeconds) + " seconds";
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
