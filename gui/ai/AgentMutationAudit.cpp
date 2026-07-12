#ifdef HAVE_AI_CHAT

#include "AgentMutationAudit.h"

#include "ToolCallSecurity.h"
#include "../../third_party/nlohmann/json.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>

namespace AI {

namespace {

using nlohmann::json;

constexpr size_t kMaxRecordBytes = 64u * 1024u;
constexpr size_t kMaxAuditErrorBytes = 2048;
constexpr size_t kMaxObjectFields = 64;
constexpr size_t kMaxArrayItems = 32;
constexpr int kMaxJsonDepth = 4;

long long nowUnixMilliseconds() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
               system_clock::now().time_since_epoch())
        .count();
}

const char* targetPolicyName(ToolTargetPolicy policy) {
    switch (policy) {
        case ToolTargetPolicy::None:      return "none";
        case ToolTargetPolicy::Bound:     return "bound";
        case ToolTargetPolicy::Selection: return "selection";
        default:                          return "none";
    }
}

const char* approvalName(MutationApproval approval) {
    switch (approval) {
        case MutationApproval::Approved:     return "approved";
        case MutationApproval::AutoApproved: return "auto_approved";
        case MutationApproval::Denied:       return "denied";
        case MutationApproval::NotRequired:  return "not_required";
        default:                             return "not_required";
    }
}

bool isLuaTool(const std::string& tool) {
    return tool == "lua_execute" || tool == "execute_lua";
}

bool isDriverTool(const std::string& tool) {
    return tool == "driver_initialize" || tool == "init_driver";
}

const char* effectName(const std::string& tool) {
    if (isDriverTool(tool)) return "connection_mutation";
    if (tool == "process_open" || tool == "open_process")
        return "target_selection";
    if (isLuaTool(tool)) return "host_execution";
    if (tool.rfind("scan_", 0) == 0 || tool == "clear_scan")
        return "session_mutation";
    return "target_mutation";
}

const char* resourceDomainName(const std::string& tool) {
    if (isDriverTool(tool)) return "connection";
    if (isLuaTool(tool)) return "lua";
    if (tool.rfind("scan_", 0) == 0 || tool == "clear_scan")
        return "scan";
    if (tool.find("breakpoint") != std::string::npos)
        return "breakpoint";
    return "process";
}

bool isSensitiveOrBulkField(const std::string& key) {
    return key == "card" || key == "card_name" || key == "code" ||
           key == "data" || key == "data_hex" || key == "bytes" ||
           key == "hex" || key == "spaced_hex" || key == "output" ||
           key == "registers" || key == "hits" || key == "items";
}

json omittedValue(const json& value) {
    json omitted;
    omitted["omitted"] = true;
    if (value.is_string()) {
        omitted["text_bytes"] = value.get_ref<const std::string&>().size();
    } else if (value.is_array() || value.is_object()) {
        omitted["item_count"] = value.size();
    }
    return omitted;
}

json sanitizeJson(const json& value,
                  int depth = 0,
                  const std::string& fieldName = {}) {
    if (isSensitiveOrBulkField(fieldName)) {
        return omittedValue(value);
    }
    if (depth >= kMaxJsonDepth) {
        return omittedValue(value);
    }
    if (value.is_string()) {
        const std::string& text = value.get_ref<const std::string&>();
        return text.size() <= 512 ? value : omittedValue(value);
    }
    if (value.is_array()) {
        if (value.size() > kMaxArrayItems) {
            return omittedValue(value);
        }
        json output = json::array();
        for (const auto& item : value) {
            output.push_back(sanitizeJson(item, depth + 1));
        }
        return output;
    }
    if (value.is_object()) {
        json output = json::object();
        size_t fields = 0;
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (fields++ >= kMaxObjectFields) {
                output["_omitted_fields"] = value.size() - kMaxObjectFields;
                break;
            }
            output[it.key()] = sanitizeJson(it.value(), depth + 1, it.key());
        }
        return output;
    }
    return value;
}

json parseAndSanitize(const std::string& text,
                      bool redactMessages = false) {
    if (text.empty()) {
        return json::object();
    }
    try {
        json sanitized = sanitizeJson(json::parse(text));
        if (redactMessages && sanitized.is_object()) {
            if (sanitized.contains("message")) {
                sanitized["message"] = json{{"omitted", true}};
            }
            if (sanitized.contains("error") &&
                sanitized.at("error").is_object() &&
                sanitized.at("error").contains("message")) {
                sanitized["error"]["message"] = json{{"omitted", true}};
            }
        }
        return sanitized;
    } catch (const json::exception&) {
        return json{{"invalid_json", true}, {"text_bytes", text.size()}};
    }
}

std::string boundedError(std::string error) {
    if (error.size() > kMaxAuditErrorBytes) {
        error.resize(kMaxAuditErrorBytes);
    }
    return error;
}

json targetJson(const Mem::TargetSnapshot& target) {
    return json{
        {"pid", target.pid},
        {"handle", target.processHandle},
        {"process_revision", target.processRevision},
        {"connection_generation", target.connectionGeneration},
    };
}

AgentMutationAuditEntry entryFromJson(const json& record) {
    AgentMutationAuditEntry entry;
    entry.timestampMs = record.value("timestamp_ms", 0ll);
    entry.runId = record.value("run_id", std::string{});
    entry.toolCallId = record.value("tool_call_id", std::string{});
    entry.tool = record.value("tool", std::string{});
    entry.effect = record.value("effect", std::string{});
    entry.resourceDomain = record.value("resource_domain", std::string{});
    entry.approval = record.value("approval", std::string{});
    entry.success = record.value("success", false);
    entry.completion = record.value("completion", std::string{});
    entry.error = record.value("error", std::string{});
    entry.durationMs = record.value("duration_ms", 0ll);
    entry.connectionGeneration =
        record.value("connection_generation", uint64_t{0});
    if (record.contains("target") && record.at("target").is_object()) {
        const json& source = record.at("target");
        Mem::TargetSnapshot target;
        target.pid = source.value("pid", 0);
        target.processHandle = source.value("handle", 0);
        target.processRevision =
            source.value("process_revision", uint64_t{0});
        target.connectionGeneration =
            source.value("connection_generation", uint64_t{0});
        entry.target = target;
    }
    return entry;
}

} // namespace

AgentMutationAuditLog& AgentMutationAuditLog::getInstance() {
    // Intentionally process-lifetime: tool shutdown may append after the chat
    // window has been destroyed, and static destruction order is not stable.
    static AgentMutationAuditLog* instance = new AgentMutationAuditLog();
    return *instance;
}

AgentMutationAuditLog::AgentMutationAuditLog(
    std::string filepath,
    size_t maxFileBytes,
    size_t maxRecentEntries)
    : filepath_(std::move(filepath)),
      maxFileBytes_((std::max)(maxFileBytes, kMaxRecordBytes)),
      maxRecentEntries_((std::max)(size_t{1}, maxRecentEntries)) {
    loadRecent();
}

bool AgentMutationAuditLog::append(const AgentMutationAuditEvent& event,
                                   std::string* error) {
    if (event.safety != ToolSafety::Write) {
        return true;
    }

    json record;
    record["schema_version"] = 1;
    record["timestamp_ms"] = nowUnixMilliseconds();
    record["run_id"] = event.runId;
    record["tool_call_id"] = event.call.id;
    record["tool"] = event.call.name;
    record["safety"] = "write";
    record["effect"] = effectName(event.call.name);
    record["resource_domain"] = resourceDomainName(event.call.name);
    record["approval"] = approvalName(event.approval);
    record["target_policy"] = targetPolicyName(event.targetPolicy);
    record["success"] = event.result.success;
    record["completion"] = toolCompletionStateName(event.result.completion);
    record["duration_ms"] = event.durationMs;
    record["connection_generation"] = event.context.connectionGeneration;
    record["cancel_requested"] =
        event.context.cancellation &&
        event.context.cancellation->load(std::memory_order_acquire);
    if (event.context.target) {
        record["target"] = targetJson(*event.context.target);
    }
    record["arguments"] = parseAndSanitize(
        toolCallArgumentsForDisplay(event.call));
    const bool redactMessages =
        isLuaTool(event.call.name) || isDriverTool(event.call.name);
    if (!event.result.errorMessage.empty()) {
        record["error"] = redactMessages
            ? "[REDACTED]"
            : boundedError(event.result.errorMessage);
    }
    record["result"] = parseAndSanitize(
        event.result.resultJson, redactMessages);

    std::string line = record.dump();
    if (line.size() > kMaxRecordBytes) {
        record["result"] = json{{"omitted", true},
                                {"text_bytes", event.result.resultJson.size()}};
        line = record.dump();
    }
    if (line.size() > kMaxRecordBytes) {
        record["arguments"] = json{{"omitted", true}};
        line = record.dump();
    }
    if (line.size() > kMaxRecordBytes) {
        if (error) *error = "mutation audit record exceeds 64 KiB";
        return false;
    }
    line.push_back('\n');

    std::lock_guard<std::mutex> lock(mutex_);
    const std::filesystem::path path(filepath_);
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            if (error) *error = "cannot create mutation audit directory: " + ec.message();
            return false;
        }
    }

    uintmax_t currentSize = 0;
    if (std::filesystem::exists(path, ec) && !ec) {
        currentSize = std::filesystem::file_size(path, ec);
        if (ec) currentSize = 0;
    }
    if (currentSize > 0 && currentSize + line.size() > maxFileBytes_) {
        const std::filesystem::path backup = path.string() + ".1";
        std::filesystem::remove(backup, ec);
        ec.clear();
        std::filesystem::rename(path, backup, ec);
        if (ec) {
            if (error) *error = "cannot rotate mutation audit log: " + ec.message();
            return false;
        }
    }

    std::ofstream output(path, std::ios::binary | std::ios::app);
    if (!output) {
        if (error) *error = "cannot open mutation audit log";
        return false;
    }
    output.write(line.data(), static_cast<std::streamsize>(line.size()));
    output.flush();
    if (!output) {
        if (error) *error = "cannot write mutation audit log";
        return false;
    }

    recent_.push_back(entryFromJson(record));
    if (recent_.size() > maxRecentEntries_) {
        recent_.erase(
            recent_.begin(),
            recent_.begin() +
                static_cast<std::ptrdiff_t>(recent_.size() - maxRecentEntries_));
    }
    return true;
}

std::vector<AgentMutationAuditEntry> AgentMutationAuditLog::recent() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return recent_;
}

void AgentMutationAuditLog::loadRecent() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ifstream input(filepath_, std::ios::binary);
    if (!input) {
        return;
    }

    std::string line;
    while (std::getline(input, line)) {
        try {
            const json record = json::parse(line);
            if (!record.is_object() ||
                record.value("schema_version", 0) != 1) {
                continue;
            }
            recent_.push_back(entryFromJson(record));
            if (recent_.size() > maxRecentEntries_) {
                recent_.erase(recent_.begin());
            }
        } catch (const json::exception&) {
            // A crash may leave one partial trailing JSONL record. Keep prior
            // valid records and ignore malformed lines.
        }
    }
}

} // namespace AI

#endif // HAVE_AI_CHAT
