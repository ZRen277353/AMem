#pragma once
#ifdef HAVE_AI_CHAT

#include "AIProvider.h"
#include "AgentRunContext.h"

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace AI {

struct CompiledToolSchema;

enum class ToolCompletionState {
    Completed,
    RejectedBeforeStart,
    CancelledBeforeStart,
    TimedOutBeforeStart,
    TimedOut,
    CancelRequested,
    CompletionUnknown,
    CompletedAfterCancelRequest,
    CompletedAfterDeadline,
};

inline const char* toolCompletionStateName(ToolCompletionState state) {
    switch (state) {
        case ToolCompletionState::Completed:
            return "completed";
        case ToolCompletionState::RejectedBeforeStart:
            return "rejected_before_start";
        case ToolCompletionState::CancelledBeforeStart:
            return "cancelled_before_start";
        case ToolCompletionState::TimedOutBeforeStart:
            return "timed_out_before_start";
        case ToolCompletionState::TimedOut:
            return "timed_out";
        case ToolCompletionState::CancelRequested:
            return "cancel_requested";
        case ToolCompletionState::CompletionUnknown:
            return "completion_unknown";
        case ToolCompletionState::CompletedAfterCancelRequest:
            return "completed_after_cancel_request";
        case ToolCompletionState::CompletedAfterDeadline:
            return "completed_after_deadline";
        default:
            return "completed";
    }
}

// A registry entry describing a single tool: its AI-facing definition, safety
// classification, and the native executor that performs the work.
struct ToolRegistration {
    ToolDefinition definition;
    std::shared_ptr<const CompiledToolSchema> compiledSchema;
    ToolSafety safety = ToolSafety::ReadOnly;
    ToolTargetPolicy targetPolicy = ToolTargetPolicy::None;
    std::function<std::string(const std::string& argsJson,
                              const Mem::OperationContext& context)> executor;
    bool advertised = true;
};

// Result returned by ToolExecutor::execute(). On success, resultJson contains
// the tool's output formatted as a JSON string; on failure, errorMessage is
// populated with a human-readable description that is forwarded back to the
// AI as a tool role message (Requirement 5.7 / 5.8).
struct ToolResult {
    bool success = false;
    std::string resultJson;
    std::string errorMessage;
    std::optional<Mem::TargetSnapshot> selectedTarget;
    ToolCompletionState completion = ToolCompletionState::Completed;
};

// Meyer's singleton that owns the tool registry and drives tool execution on
// behalf of the AI. All public methods are thread-safe: tools may be executed
// from the UI thread while the provider background thread reads tool
// definitions when composing the next request.
class ToolExecutor {
public:
    static ToolExecutor& getInstance() {
        static ToolExecutor instance;
        return instance;
    }

    // Register a tool. If a tool with the same name already exists, the
    // existing entry is replaced. Enforces: name length <= 64 (AC 5.2),
    // description length <= 256 (AC 5.2). Over-length values are truncated
    // so that the registry invariants always hold.
    void registerTool(const std::string& name,
                      const std::string& description,
                      const std::string& parametersSchema,
                      ToolSafety safety,
                      std::function<std::string(const std::string&)> executor,
                      bool advertised = true,
                      ToolTargetPolicy targetPolicy = ToolTargetPolicy::None);

    // Context-aware overload used by tools migrated to MemService. The
    // supplied operation snapshot is owned by the current agent run and must
    // not be recaptured from global state inside the executor.
    void registerTool(
        const std::string& name,
        const std::string& description,
        const std::string& parametersSchema,
        ToolSafety safety,
        std::function<std::string(const std::string&,
                                  const Mem::OperationContext&)> executor,
        ToolTargetPolicy targetPolicy,
        bool advertised = true);

    // Initialise the built-in tool set. Definition lives in
    // ToolDefinitions.cpp (task 5.2) so that the executor core stays
    // decoupled from the socket-backed tool implementations.
    void initBuiltinTools();

    // Execute synchronously on the caller-owned worker. Performs lookup,
    // JSON parse, schema validation, deadline propagation, invocation, and
    // error wrapping. Thread ownership belongs to AgentTaskExecutor.
    ToolResult execute(const ToolCall& call);
    ToolResult execute(const ToolCall& call,
                       const Mem::OperationContext& context);

    // Return advertised AI-facing definitions. Hidden compatibility aliases
    // remain executable for saved sessions but are not sent to providers.
    std::vector<ToolDefinition> getToolDefinitions() const;

    // Safety classification for a given tool name. Unknown names cannot
    // execute; ReadOnly avoids presenting a misleading write confirmation
    // before execute() returns the unrecognized-tool error.
    ToolSafety getToolSafety(const std::string& name) const;
    ToolTargetPolicy getToolTargetPolicy(const std::string& name) const;

    // Execution timeout configuration. setExecutionTimeout() clamps the
    // argument to the inclusive range [1, 300] seconds (AC 6.5).
    void setExecutionTimeout(int seconds);
    int getExecutionTimeout() const;

private:
    ToolExecutor() = default;
    ToolExecutor(const ToolExecutor&) = delete;
    ToolExecutor& operator=(const ToolExecutor&) = delete;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, ToolRegistration> tools_;
    int executionTimeout_ = 30; // seconds, AC 6.5 default
};

} // namespace AI

#endif // HAVE_AI_CHAT
