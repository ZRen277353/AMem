#ifdef HAVE_AI_CHAT

#include "AgentRunContext.h"

namespace AI {

std::optional<Mem::Error> validateAgentRunContext(
    const Mem::OperationContext& expected,
    const Mem::OperationContext& current,
    ToolTargetPolicy policy) {
    if (current.connectionGeneration != expected.connectionGeneration) {
        return Mem::Error{
            Mem::ErrorCode::ConnectionChanged,
            "connection changed since the agent run started",
            true};
    }

    if (policy == ToolTargetPolicy::None) {
        return std::nullopt;
    }

    if (!expected.target) {
        return Mem::Error{
            policy == ToolTargetPolicy::Bound
                ? Mem::ErrorCode::NoTarget
                : Mem::ErrorCode::TargetChanged,
            policy == ToolTargetPolicy::Bound
                ? "tool requires an attached target process"
                : "target selection state is unavailable",
            false};
    }
    if (expected.target->connectionGeneration !=
        expected.connectionGeneration) {
        return Mem::Error{
            Mem::ErrorCode::ConnectionChanged,
            "run target belongs to a different connection generation",
            true};
    }
    if (policy == ToolTargetPolicy::Bound &&
        !expected.target->isAttached()) {
        return Mem::Error{
            Mem::ErrorCode::NoTarget,
            "tool requires an attached target process",
            false};
    }
    if (!current.target || *current.target != *expected.target) {
        return Mem::Error{
            Mem::ErrorCode::TargetChanged,
            "target process changed since the agent run started",
            false};
    }
    return std::nullopt;
}

std::optional<Mem::Error> validateTargetSelectionResult(
    const Mem::OperationContext& expected,
    const Mem::OperationContext& current,
    const Mem::TargetSnapshot& selectedTarget) {
    if (const auto error = validateAgentRunContext(
            expected, current, ToolTargetPolicy::None)) {
        return error;
    }
    if (!selectedTarget.isAttached()) {
        return Mem::Error{
            Mem::ErrorCode::NoTarget,
            "target-selection tool did not return an attached process",
            false};
    }
    if (selectedTarget.connectionGeneration !=
        expected.connectionGeneration) {
        return Mem::Error{
            Mem::ErrorCode::ConnectionChanged,
            "selected target belongs to a different connection generation",
            true};
    }
    if (!current.target || *current.target != selectedTarget) {
        return Mem::Error{
            Mem::ErrorCode::TargetChanged,
            "target changed before the selection result was accepted",
            false};
    }
    return std::nullopt;
}

} // namespace AI

#endif // HAVE_AI_CHAT
