#pragma once

#include "MemResult.h"
#include "MemTypes.h"

#include <optional>

namespace Mem {

class IMemService;

std::optional<Error> validateLuaOperationContext(
    IMemService& service,
    const OperationContext& context,
    bool requireAttachedTarget);

std::optional<Error> advanceLuaOperationTarget(
    IMemService& service,
    OperationContext& context,
    const TargetSnapshot& selectedTarget);

} // namespace Mem
