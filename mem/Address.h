#pragma once

#include "MemResult.h"

#include <cstdint>
#include <string>

namespace Mem {

Result<uint64_t> parseAddress(const std::string& text,
                              bool requireHexPrefix = true);
std::string formatAddress(uint64_t address);

} // namespace Mem
