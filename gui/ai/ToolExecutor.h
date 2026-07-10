#pragma once
#ifdef HAVE_AI_CHAT

#include "AIProvider.h"

#include <condition_variable>
#include <functional>
#include <mutex>
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
                      bool advertised = true);

    // Initialise the built-in tool set. Definition lives in
    // ToolDefinitions.cpp (task 5.2) so that the executor core stays
    // decoupled from the socket-backed tool implementations.
    void initBuiltinTools();

    // Execute a tool_call received from the AI. Performs: lookup,
    // JSON parse, schema validation, invocation with timeout, and error
    // wrapping. Never throws; any exception from the executor is captured
    // and surfaced as a ToolResult with success=false.
    ToolResult execute(const ToolCall& call);

    // Return advertised AI-facing definitions. Hidden compatibility aliases
    // remain executable for saved sessions but are not sent to providers.
    std::vector<ToolDefinition> getToolDefinitions() const;

    // Safety classification for a given tool name. Returns ToolSafety::ReadOnly
    // as a safe default when the tool is not registered so that callers
    // never accidentally treat an unknown tool as write-classified.
    ToolSafety getToolSafety(const std::string& name) const;

    // Execution timeout configuration. setExecutionTimeout() clamps the
    // argument to the inclusive range [1, 300] seconds (AC 6.5).
    void setExecutionTimeout(int seconds);
    int getExecutionTimeout() const;

    // Drain in-flight tool-execution worker threads during app teardown so
    // a detached worker can't push to UIMessageQueue or touch socket
    // singletons after they've been destroyed. Bounded, best-effort, and
    // idempotent — mirrors HttpClient::shutdown(). Call from main() before
    // static destruction begins.
    void shutdown();

    // True once shutdown() has begun. Tool workers check this before
    // delivering a result so a late worker (one that outran shutdown()'s
    // bounded wait) does not touch already-destroyed singletons.
    bool isShuttingDown() const;

    // Register / retire a tool-execution worker around its full lifetime
    // (execute() + result delivery). beginToolWorker() returns false when a
    // shutdown is already in progress, signalling the caller not to start.
    bool beginToolWorker();
    void endToolWorker();

private:
    ToolExecutor() = default;
    ToolExecutor(const ToolExecutor&) = delete;
    ToolExecutor& operator=(const ToolExecutor&) = delete;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, ToolRegistration> tools_;
    int executionTimeout_ = 30; // seconds, AC 6.5 default

    // Lifecycle tracking for detached tool-execution workers (teardown
    // drain). Guarded independently of mutex_ so a long-running executor
    // never blocks registry queries.
    mutable std::mutex lifecycleMutex_;
    std::condition_variable lifecycleCv_;
    int inFlightWorkers_ = 0;   // detached workers still running
    bool shuttingDown_ = false; // set by shutdown()
};

} // namespace AI

#endif // HAVE_AI_CHAT
