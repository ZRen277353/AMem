#ifdef HAVE_AI_CHAT

#include "ToolExecutor.h"

#include "AiJsonLimits.h"
#include "AiLimits.h"
#include "../../socket/socket_io_timeout.h"
#include "../../third_party/nlohmann/json.hpp"

#include <chrono>
#include <exception>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>

namespace AI {

struct CompiledToolSchema {
    nlohmann::json document;
    std::string error;
};

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
    return false;
}

bool isSupportedSchemaType(const std::string& type) {
    return type == "object" || type == "string" || type == "boolean" ||
           type == "array" || type == "integer" || type == "number";
}

bool readNonNegativeSize(const json& value, std::size_t& output) {
    if (value.type() == json::value_t::number_integer) {
        const auto signedValue = value.get<long long>();
        if (signedValue < 0) {
            return false;
        }
        output = static_cast<std::size_t>(signedValue);
        return static_cast<unsigned long long>(signedValue) == output;
    }
    if (value.type() != json::value_t::number_unsigned) {
        return false;
    }
    const auto unsignedValue = value.get<unsigned long long>();
    output = static_cast<std::size_t>(unsignedValue);
    return static_cast<unsigned long long>(output) == unsignedValue;
}

std::string validateSchemaDefinition(const json& schema,
                                     const std::string& path,
                                     std::size_t depth = 0) {
    constexpr std::size_t kMaxSchemaDepth = 32;
    if (!schema.is_object()) {
        return "invalid tool parameter schema: " + path +
               " must be an object";
    }
    if (depth > kMaxSchemaDepth) {
        return "invalid tool parameter schema: nesting exceeds 32 levels at " +
               path;
    }

    static const std::unordered_set<std::string> kSupportedKeywords = {
        "type", "description", "required", "properties",
        "additionalProperties", "enum", "pattern", "anyOf", "oneOf",
        "items", "minimum", "maximum", "minLength", "maxLength",
        "minItems", "maxItems",
    };
    for (auto it = schema.begin(); it != schema.end(); ++it) {
        if (kSupportedKeywords.find(it.key()) == kSupportedKeywords.end()) {
            return "invalid tool parameter schema: unsupported keyword '" +
                   it.key() + "' at " + path;
        }
    }

    if (schema.contains("type") &&
        (!schema["type"].is_string() ||
         !isSupportedSchemaType(schema["type"].get<std::string>()))) {
        return "invalid tool parameter schema: unsupported type at " + path;
    }
    if (schema.contains("description") &&
        !schema["description"].is_string()) {
        return "invalid tool parameter schema: description must be a string at " +
               path;
    }
    if (schema.contains("required")) {
        if (!schema["required"].is_array()) {
            return "invalid tool parameter schema: required must be an array at " +
                   path;
        }
        for (const auto& required : schema["required"]) {
            if (!required.is_string()) {
                return "invalid tool parameter schema: required entries must be strings at " +
                       path;
            }
        }
    }
    if (schema.contains("properties")) {
        if (!schema["properties"].is_object()) {
            return "invalid tool parameter schema: properties must be an object at " +
                   path;
        }
        for (auto it = schema["properties"].begin();
             it != schema["properties"].end(); ++it) {
            const std::string error = validateSchemaDefinition(
                it.value(), path + ".properties." + it.key(), depth + 1);
            if (!error.empty()) {
                return error;
            }
        }
    }
    if (schema.contains("additionalProperties")) {
        const json& additional = schema["additionalProperties"];
        if (!additional.is_boolean() && !additional.is_object()) {
            return "invalid tool parameter schema: additionalProperties must be boolean or object at " +
                   path;
        }
        if (additional.is_object()) {
            const std::string error = validateSchemaDefinition(
                additional, path + ".additionalProperties", depth + 1);
            if (!error.empty()) {
                return error;
            }
        }
    }
    if (schema.contains("enum") &&
        (!schema["enum"].is_array() || schema["enum"].empty())) {
        return "invalid tool parameter schema: enum must be a non-empty array at " +
               path;
    }
    if (schema.contains("pattern")) {
        if (!schema["pattern"].is_string()) {
            return "invalid tool parameter schema: pattern must be a string at " +
                   path;
        }
        try {
            (void)std::regex(schema["pattern"].get<std::string>(),
                             std::regex::ECMAScript);
        } catch (const std::regex_error&) {
            return "invalid tool parameter schema: malformed pattern at " + path;
        }
    }

    for (const char* keyword : {"anyOf", "oneOf"}) {
        if (!schema.contains(keyword)) {
            continue;
        }
        const json& alternatives = schema[keyword];
        if (!alternatives.is_array() || alternatives.empty()) {
            return std::string("invalid tool parameter schema: ") + keyword +
                   " must be a non-empty array at " + path;
        }
        for (std::size_t i = 0; i < alternatives.size(); ++i) {
            const std::string error = validateSchemaDefinition(
                alternatives[i], path + "." + keyword + "[" +
                                     std::to_string(i) + "]",
                depth + 1);
            if (!error.empty()) {
                return error;
            }
        }
    }
    if (schema.contains("items")) {
        const std::string error = validateSchemaDefinition(
            schema["items"], path + ".items", depth + 1);
        if (!error.empty()) {
            return error;
        }
    }
    for (const char* keyword : {"minimum", "maximum"}) {
        if (schema.contains(keyword) && !schema[keyword].is_number()) {
            return std::string("invalid tool parameter schema: ") + keyword +
                   " must be numeric at " + path;
        }
    }
    if (schema.contains("minimum") && schema.contains("maximum") &&
        schema["minimum"].get<double>() > schema["maximum"].get<double>()) {
        return "invalid tool parameter schema: minimum exceeds maximum at " +
               path;
    }

    for (const auto& pair : {
             std::pair<const char*, const char*>{"minLength", "maxLength"},
             std::pair<const char*, const char*>{"minItems", "maxItems"}}) {
        std::size_t minimum = 0;
        std::size_t maximum = 0;
        if (schema.contains(pair.first) &&
            !readNonNegativeSize(schema[pair.first], minimum)) {
            return std::string("invalid tool parameter schema: ") + pair.first +
                   " must be a non-negative size at " + path;
        }
        if (schema.contains(pair.second) &&
            !readNonNegativeSize(schema[pair.second], maximum)) {
            return std::string("invalid tool parameter schema: ") + pair.second +
                   " must be a non-negative size at " + path;
        }
        if (schema.contains(pair.first) && schema.contains(pair.second) &&
            minimum > maximum) {
            return std::string("invalid tool parameter schema: ") + pair.first +
                   " exceeds " + pair.second + " at " + path;
        }
    }
    return {};
}

// Bounded JSON-Schema subset used by every built-in tool. Unsupported or
// malformed schema keywords fail closed before an executor can run.
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

    if (schema.contains("enum")) {
        bool matched = false;
        for (const auto& accepted : schema["enum"]) {
            if (args == accepted) {
                matched = true;
                break;
            }
        }
        if (!matched) {
            return path + " is not one of the allowed enum values";
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
        if (schema.contains("pattern")) {
            const std::regex expression(schema["pattern"].get<std::string>(),
                                        std::regex::ECMAScript);
            if (!std::regex_search(s, expression)) {
                return path + " does not match the required pattern";
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
        if (schema.contains("additionalProperties")) {
            const json& additional = schema["additionalProperties"];
            const json* properties = schema.contains("properties")
                                         ? &schema["properties"]
                                         : nullptr;
            for (auto it = args.begin(); it != args.end(); ++it) {
                if (properties && properties->contains(it.key())) {
                    continue;
                }
                if (additional.is_boolean() && !additional.get<bool>()) {
                    return "unexpected property '" + it.key() + "' at " + path;
                }
                if (additional.is_object()) {
                    const std::string error = validateAgainstSchema(
                        it.value(), additional, path + "." + it.key());
                    if (!error.empty()) {
                        return error;
                    }
                }
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

    if (schema.contains("oneOf")) {
        int matches = 0;
        for (const auto& alternative : schema["oneOf"]) {
            if (validateAgainstSchema(args, alternative, path).empty()) {
                ++matches;
            }
        }
        if (matches != 1) {
            return path + " must match exactly one accepted argument form";
        }
    }

    return "";
}

std::shared_ptr<const CompiledToolSchema> compileToolSchema(
    const std::string& schemaText) {
    auto compiled = std::make_shared<CompiledToolSchema>();
    if (schemaText.empty()) {
        compiled->document = json::object();
        return compiled;
    }
    std::string parseError;
    if (!utils::parseBoundedJson(
            schemaText, kToolSchemaJsonLimits, compiled->document,
            parseError)) {
        compiled->error = "invalid tool parameter schema: " + parseError;
        return compiled;
    }
    if (!compiled->document.is_object()) {
        compiled->error =
            "invalid tool parameter schema: root schema must be an object";
        return compiled;
    }
    compiled->error = validateSchemaDefinition(compiled->document, "root");
    return compiled;
}

std::string validateAgainstSchema(
    const json& args,
    const std::shared_ptr<const CompiledToolSchema>& compiled) {
    if (!compiled) {
        return "invalid tool parameter schema: schema was not compiled";
    }
    if (!compiled->error.empty()) {
        return compiled->error;
    }
    return validateAgainstSchema(args, compiled->document, "root");
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

std::string extractToolError(const json& result) {
    if (!result.is_object()) {
        return "tool result must be a JSON object";
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
}

ToolCompletionState extractCompletionState(const json& result) {
    if (!result.is_object()) {
        return ToolCompletionState::Completed;
    }

    if (result.contains("completion") &&
        result["completion"].is_string()) {
        const std::string completion =
            result["completion"].get<std::string>();
        if (completion == "rejected_before_start")
            return ToolCompletionState::RejectedBeforeStart;
        if (completion == "cancelled_before_start")
            return ToolCompletionState::CancelledBeforeStart;
        if (completion == "timed_out_before_start")
            return ToolCompletionState::TimedOutBeforeStart;
        if (completion == "timed_out")
            return ToolCompletionState::TimedOut;
        if (completion == "cancel_requested")
            return ToolCompletionState::CancelRequested;
        if (completion == "completion_unknown")
            return ToolCompletionState::CompletionUnknown;
        if (completion == "completed_after_cancel_request")
            return ToolCompletionState::CompletedAfterCancelRequest;
        if (completion == "completed_after_deadline")
            return ToolCompletionState::CompletedAfterDeadline;
    }

    if (result.contains("error") && result["error"].is_object() &&
        result["error"].contains("code") &&
        result["error"]["code"].is_string()) {
        const std::string code =
            result["error"]["code"].get<std::string>();
        if (code == "cancel_requested")
            return ToolCompletionState::CancelRequested;
        if (code == "timeout")
            return ToolCompletionState::TimedOut;
        if (code == "completion_unknown")
            return ToolCompletionState::CompletionUnknown;
    }
    return ToolCompletionState::Completed;
}

std::optional<Mem::TargetSnapshot> extractSelectedTarget(
    const json& result,
    std::string& error) {
    try {
        if (!result.is_object() ||
            !result.contains("pid") ||
            !result.contains("handle") ||
            !result.contains("process_revision") ||
            !result.contains("connection_generation")) {
            error = "target-selection tool did not return a complete target snapshot";
            return std::nullopt;
        }

        Mem::TargetSnapshot target;
        target.pid = result.at("pid").get<int>();
        target.processHandle = result.at("handle").get<int>();
        target.processRevision = result.at("process_revision").get<uint64_t>();
        target.connectionGeneration =
            result.at("connection_generation").get<uint64_t>();
        if (!target.isAttached()) {
            error = "target-selection tool returned an unattached target snapshot";
            return std::nullopt;
        }
        return target;
    } catch (const std::exception& exception) {
        error = std::string("invalid target-selection result: ") + exception.what();
        return std::nullopt;
    }
}

} // namespace

void ToolExecutor::registerTool(const std::string& name,
                                const std::string& description,
                                const std::string& parametersSchema,
                                ToolSafety safety,
                                std::function<std::string(const std::string&)> executor,
                                bool advertised,
                                ToolTargetPolicy targetPolicy) {
    // Enforce AC 5.2 invariants by truncation so the registry can never hold
    // an over-length entry, regardless of caller discipline.
    ToolRegistration reg;
    reg.definition.name = name.size() > 64 ? name.substr(0, 64) : name;
    reg.definition.description = description.size() > 256 ? description.substr(0, 256) : description;
    reg.definition.parametersSchema = parametersSchema;
    reg.compiledSchema = compileToolSchema(parametersSchema);
    reg.safety = safety;
    reg.targetPolicy = targetPolicy;
    if (executor) {
        reg.executor = [executor = std::move(executor)](
                           const std::string& argsJson,
                           const Mem::OperationContext&) {
            return executor(argsJson);
        };
    }
    reg.advertised = advertised && reg.compiledSchema->error.empty();

    std::lock_guard<std::mutex> lock(mutex_);
    tools_[reg.definition.name] = std::move(reg);
}

void ToolExecutor::registerTool(
    const std::string& name,
    const std::string& description,
    const std::string& parametersSchema,
    ToolSafety safety,
    std::function<std::string(const std::string&,
                              const Mem::OperationContext&)> executor,
    ToolTargetPolicy targetPolicy,
    bool advertised) {
    ToolRegistration reg;
    reg.definition.name = name.size() > 64 ? name.substr(0, 64) : name;
    reg.definition.description =
        description.size() > 256 ? description.substr(0, 256) : description;
    reg.definition.parametersSchema = parametersSchema;
    reg.compiledSchema = compileToolSchema(parametersSchema);
    reg.safety = safety;
    reg.targetPolicy = targetPolicy;
    reg.executor = std::move(executor);
    reg.advertised = advertised && reg.compiledSchema->error.empty();

    std::lock_guard<std::mutex> lock(mutex_);
    tools_[reg.definition.name] = std::move(reg);
}

// Note: initBuiltinTools() is defined in ToolDefinitions.cpp (task 5.2) so
// that the executor core remains free of socket/tool-implementation
// dependencies. Intentionally not defined here.

ToolResult ToolExecutor::execute(const ToolCall& call) {
    return execute(call, Mem::OperationContext{});
}

ToolResult ToolExecutor::execute(const ToolCall& call,
                                 const Mem::OperationContext& context) {
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
        std::string parseError;
        if (!utils::parseBoundedJson(
                call.arguments, kToolArgumentJsonLimits, args, parseError)) {
            ToolResult result;
            result.success = false;
            result.errorMessage = std::string("Invalid JSON in arguments for tool '") +
                                  call.name + "': " + parseError;
            return result;
        }
    }

    // Schema validation (AC 5.6).
    const std::string validationError =
        validateAgainstSchema(args, registration.compiledSchema);
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

    Mem::OperationContext executionContext = context;
    const auto timeoutDeadline =
        std::chrono::steady_clock::now() +
        std::chrono::seconds(timeoutSeconds);
    if (executionContext.deadline > timeoutDeadline) {
        executionContext.deadline = timeoutDeadline;
    }

    if (std::chrono::steady_clock::now() >= executionContext.deadline) {
        ToolResult result;
        result.success = false;
        result.errorMessage = "Tool '" + call.name +
                              "' deadline expired before execution";
        result.completion = ToolCompletionState::TimedOutBeforeStart;
        return result;
    }

    // AgentTaskExecutor owns the worker thread. Execute synchronously here so
    // no untracked inner task can outlive shutdown; the same absolute
    // deadline flows into MemService and socket lock/I/O budgets.
    ToolResult result;
    try {
        SocketIoTimeout::ScopedTimeout socketTimeout(
            executionContext.deadline);
        result.resultJson =
            registration.executor(normalizedArgsJson, executionContext);
        if (result.resultJson.size() > Limits::kMaxToolResultBytes) {
            std::string().swap(result.resultJson);
            result.success = false;
            result.errorMessage = "Tool result exceeds 4 MiB output limit; "
                                  "operation completion is unknown";
            result.completion = ToolCompletionState::CompletionUnknown;
            return result;
        }
        json resultDocument;
        std::string resultParseError;
        if (!utils::parseBoundedJson(result.resultJson,
                                     kToolResultJsonLimits,
                                     resultDocument,
                                     resultParseError)) {
            result.success = false;
            result.errorMessage =
                "Tool returned invalid or over-complex JSON: " +
                resultParseError;
            result.completion = registration.safety == ToolSafety::Write
                ? ToolCompletionState::CompletionUnknown
                : ToolCompletionState::Completed;
            return result;
        }
        result.errorMessage = extractToolError(resultDocument);
        result.success = result.errorMessage.empty();
        result.completion = extractCompletionState(resultDocument);
        const bool hasAnyTargetField =
            resultDocument.contains("pid") ||
            resultDocument.contains("handle") ||
            resultDocument.contains("process_revision") ||
            resultDocument.contains("connection_generation");
        if (registration.targetPolicy == ToolTargetPolicy::Selection &&
            (result.success || hasAnyTargetField)) {
            std::string targetError;
            result.selectedTarget =
                extractSelectedTarget(resultDocument, targetError);
            if (!result.selectedTarget) {
                result.success = false;
                result.errorMessage = std::move(targetError);
            }
        }

        if (std::chrono::steady_clock::now() >= executionContext.deadline &&
            result.success) {
            if (registration.safety == ToolSafety::Write) {
                if (result.completion == ToolCompletionState::Completed) {
                    result.completion =
                        ToolCompletionState::CompletedAfterDeadline;
                }
            } else {
                result.success = false;
                result.errorMessage = "Tool '" + call.name +
                                      "' completed after its deadline";
                result.completion = ToolCompletionState::TimedOut;
            }
        }
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
        if (kv.second.advertised) {
            defs.push_back(kv.second.definition);
        }
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

ToolTargetPolicy ToolExecutor::getToolTargetPolicy(
    const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tools_.find(name);
    return it == tools_.end() ? ToolTargetPolicy::None
                              : it->second.targetPolicy;
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
