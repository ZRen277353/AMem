#pragma once

#include "MemResult.h"
#include "MemTypes.h"

#include <string>
#include <vector>

namespace Mem {

const char* scalarTypeName(ScalarType type);
size_t scalarTypeSize(ScalarType type);

Result<ScalarType> parseScalarType(const std::string& name);
Result<std::vector<unsigned char>> encodeScalarValue(
    ScalarType type,
    const std::string& valueText);
Result<DecodedScalarValue> decodeScalarValue(
    ScalarType type,
    const std::vector<unsigned char>& bytes);
std::string formatScalarValue(const DecodedScalarValue& value);

} // namespace Mem
