#include "LuaOperationContext.h"

#include "IMemService.h"

#include <chrono>

namespace Mem {

std::optional<Error> validateLuaOperationContext(
    IMemService& service,
    const OperationContext& context,
    bool requireAttachedTarget) {
    if (context.cancellation &&
        context.cancellation->load(std::memory_order_acquire)) {
        return Error{ErrorCode::CancelRequested,
                     "Lua execution was cancelled", false};
    }
    if (std::chrono::steady_clock::now() >= context.deadline) {
        return Error{ErrorCode::Timeout,
                     "Lua execution deadline has expired", true};
    }
    const OperationContext current = service.captureContext(true);
    if (current.connectionGeneration != context.connectionGeneration) {
        return Error{ErrorCode::ConnectionChanged,
                     "connection changed during Lua execution", true};
    }
    if (!context.target) {
        return Error{ErrorCode::TargetChanged,
                     "Lua target context is unavailable", false};
    }
    if (context.target->connectionGeneration !=
        context.connectionGeneration) {
        return Error{ErrorCode::ConnectionChanged,
                     "Lua target belongs to another connection generation",
                     true};
    }
    if (requireAttachedTarget && !context.target->isAttached()) {
        return Error{ErrorCode::NoTarget,
                     "Lua execution requires an attached target process",
                     false};
    }
    if (!current.target || *current.target != *context.target) {
        return Error{ErrorCode::TargetChanged,
                     "target changed outside the active Lua script", false};
    }
    return std::nullopt;
}

std::optional<Error> advanceLuaOperationTarget(
    IMemService& service,
    OperationContext& context,
    const TargetSnapshot& selectedTarget) {
    if (!selectedTarget.isAttached()) {
        return Error{ErrorCode::NoTarget,
                     "Lua process selection returned no attached target",
                     false};
    }
    if (selectedTarget.connectionGeneration !=
        context.connectionGeneration) {
        return Error{ErrorCode::ConnectionChanged,
                     "Lua process selection belongs to another connection generation",
                     true};
    }

    const OperationContext current = service.captureContext(true);
    if (current.connectionGeneration != context.connectionGeneration) {
        return Error{ErrorCode::ConnectionChanged,
                     "connection changed during Lua process selection", true};
    }
    if (!current.target || *current.target != selectedTarget) {
        return Error{ErrorCode::TargetChanged,
                     "Lua process selection result is no longer current",
                     false};
    }

    context.target = selectedTarget;
    return std::nullopt;
}

} // namespace Mem
