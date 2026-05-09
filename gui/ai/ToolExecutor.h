#pragma once
#ifdef HAVE_AI_CHAT

#include "AIProvider.h"

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace AI {

// A registry entry describing a single tool: its AI-facing definition, safety
// classification, and the native executor that performs the work.
struct ToolRegistration {
    ToolDefinition definition;
    ToolSafety safety = ToolSafety::ReadOnly;
    std::function<std::string(const std::string& argsJson)> executor;
};

// Result returned by ToolExecutor::execute(). On success, resultJson contains
// the tool's output formatted as a JSON string; on failure, errorMessage is
// populated with a human-readable description that is forwarded back to the
// AI as a tool role message (Requirement 5.7 / 5.8).
struct ToolResult {
    bool success = false;
    std::string resultJson;
    std::string errorMessage;
};

// Meyer's singleton that owns the tool registry and drives tool execution on
// behalf of the AI. All public methods are thread-safe: tools may be executed
// from the UI thread while the provider background thread reads tool
// definitions when composing the next request.
class ToolExecutor {
public:
    // User confirmation state for a pending write-classified tool call. The
    // UI toggles this between Pending / Approved / Denied as the user
    // interacts with the confirmation modal.
    enum class ConfirmationState { Pending, Approved, Denied };

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
                      std::function<std::string(const std::string&)> executor);

    // Initialise the built-in tool set. Definition lives in
    // ToolDefinitions.cpp (task 5.2) so that the executor core stays
    // decoupled from the socket-backed tool implementations.
    void initBuiltinTools();

    // Execute a tool_call received from the AI. Performs: lookup,
    // JSON parse, schema validation, invocation with timeout, and error
    // wrapping. Never throws; any exception from the executor is captured
    // and surfaced as a ToolResult with success=false.
    ToolResult execute(const ToolCall& call);

    // Return a snapshot of every registered tool's AI-facing definition.
    // Used by ChatSession / providers to advertise tools on each request.
    std::vector<ToolDefinition> getToolDefinitions() const;

    // Safety classification for a given tool name. Returns ToolSafety::ReadOnly
    // as a safe default when the tool is not registered so that callers
    // never accidentally treat an unknown tool as write-classified.
    ToolSafety getToolSafety(const std::string& name) const;

    // Execution timeout configuration. setExecutionTimeout() clamps the
    // argument to the inclusive range [1, 300] seconds (AC 6.5).
    void setExecutionTimeout(int seconds);
    int getExecutionTimeout() const;

    // User confirmation state plumbing used by the UI to gate write tools
    // (AC 6.2 / 6.4). The executor itself does not block on confirmation;
    // that is the ChatWindow's responsibility.
    void setConfirmationState(ConfirmationState state);
    ConfirmationState getConfirmationState() const;

    // Pending tool_call awaiting user confirmation. The ChatWindow stores
    // the call here while showing the modal; the UI reads it back to render
    // the confirmation prompt. Passing nullptr clears the pending entry.
    void setPendingToolCall(const ToolCall* call);
    const ToolCall* getPendingToolCall() const;

private:
    ToolExecutor() = default;
    ToolExecutor(const ToolExecutor&) = delete;
    ToolExecutor& operator=(const ToolExecutor&) = delete;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, ToolRegistration> tools_;
    int executionTimeout_ = 30; // seconds, AC 6.5 default
    ConfirmationState confirmState_ = ConfirmationState::Pending;
    std::optional<ToolCall> pendingCall_;
};

} // namespace AI

#endif // HAVE_AI_CHAT
