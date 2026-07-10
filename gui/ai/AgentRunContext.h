#pragma once
#ifdef HAVE_AI_CHAT

#include "../../mem/MemResult.h"
#include "../../mem/MemTypes.h"

#include <optional>
#include <string>

namespace AI {

enum class ToolTargetPolicy {
    None,
    Bound,
    Selection,
};

struct AgentRunContext {
    std::string runId;
    Mem::OperationContext operation;
};

std::optional<Mem::Error> validateAgentRunContext(
    const Mem::OperationContext& expected,
    const Mem::OperationContext& current,
    ToolTargetPolicy policy);

std::optional<Mem::Error> validateTargetSelectionResult(
    const Mem::OperationContext& expected,
    const Mem::OperationContext& current,
    const Mem::TargetSnapshot& selectedTarget);

} // namespace AI

#endif // HAVE_AI_CHAT
