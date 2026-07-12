#pragma once

#include "IMemService.h"

#include <string>

namespace Mem {

std::string executeLuaJson(IMemService &service, const std::string &argsJson,
                           const OperationContext &context);

} // namespace Mem
