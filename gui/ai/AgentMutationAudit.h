#pragma once
#ifdef HAVE_AI_CHAT

#include "ToolExecutor.h"

#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace AI {

enum class MutationApproval {
    NotRequired,
    Approved,
    AutoApproved,
    Denied,
};

struct AgentMutationAuditEvent {
    std::string runId;
    ToolCall call;
    ToolResult result;
    Mem::OperationContext context;
    ToolSafety safety = ToolSafety::ReadOnly;
    ToolTargetPolicy targetPolicy = ToolTargetPolicy::None;
    MutationApproval approval = MutationApproval::NotRequired;
    long long durationMs = 0;
};

struct AgentMutationAuditEntry {
    long long timestampMs = 0;
    std::string runId;
    std::string toolCallId;
    std::string tool;
    std::string effect;
    std::string resourceDomain;
    std::string approval;
    bool success = false;
    std::string completion;
    std::string error;
    long long durationMs = 0;
    uint64_t connectionGeneration = 0;
    std::optional<Mem::TargetSnapshot> target;
};

bool shouldAuditMutationOutcome(ToolSafety safety,
                                const std::string& toolName);

class AgentMutationAuditLog {
public:
    static AgentMutationAuditLog& getInstance();

    explicit AgentMutationAuditLog(
        std::string filepath = "ai_mutation_audit.jsonl",
        size_t maxFileBytes = 4u * 1024u * 1024u,
        size_t maxRecentEntries = 100);

    bool append(const AgentMutationAuditEvent& event,
                std::string* error = nullptr);
    std::vector<AgentMutationAuditEntry> recent() const;
    const std::string& filepath() const { return filepath_; }

private:
    void loadRecent();

    std::string filepath_;
    size_t maxFileBytes_ = 0;
    size_t maxRecentEntries_ = 0;
    mutable std::mutex mutex_;
    std::vector<AgentMutationAuditEntry> recent_;
};

} // namespace AI

#endif // HAVE_AI_CHAT
