#include "ValueCodec.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace Mem {

namespace {

std::string trimAscii(const std::string& value) {
    size_t begin = 0;
    while (begin < value.size() &&
           (value[begin] == ' ' || value[begin] == '\t' ||
            value[begin] == '\r' || value[begin] == '\n')) {
        ++begin;
    }
    size_t end = value.size();
    while (end > begin &&
           (value[end - 1] == ' ' || value[end - 1] == '\t' ||
            value[end - 1] == '\r' || value[end - 1] == '\n')) {
        --end;
    }
    return value.substr(begin, end - begin);
}

std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) {
                       if (c >= 'A' && c <= 'Z') {
                           return static_cast<char>(c - 'A' + 'a');
                       }
                       return static_cast<char>(c);
                   });
    return value;
}

Result<uint64_t> parseIntegerBits(const std::string& input,
                                  unsigned int bits,
                                  const char* typeName) {
    std::string text = trimAscii(input);
    if (text.empty()) {
        return Result<uint64_t>::failure(
            ErrorCode::InvalidArgument,
            std::string(typeName) + " value is empty");
    }

    bool negative = false;
    if (text.front() == '-' || text.front() == '+') {
        negative = text.front() == '-';
        text.erase(text.begin());
    }

    int base = 10;
    if (text.size() >= 2 && text[0] == '0' &&
        (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        text.erase(0, 2);
    }
    if (text.empty()) {
        return Result<uint64_t>::failure(
            ErrorCode::InvalidArgument,
            std::string("invalid ") + typeName + " value");
    }

    unsigned long long magnitude = 0;
    size_t consumed = 0;
    try {
        magnitude = std::stoull(text, &consumed, base);
    } catch (const std::exception&) {
        return Result<uint64_t>::failure(
            ErrorCode::InvalidArgument,
            std::string("invalid ") + typeName + " value");
    }
    if (consumed != text.size()) {
        return Result<uint64_t>::failure(
            ErrorCode::InvalidArgument,
            std::string("invalid trailing characters in ") + typeName +
                " value");
    }

    if (negative) {
        const uint64_t maxMagnitude = bits == 64
            ? (uint64_t{1} << 63)
            : (uint64_t{1} << (bits - 1));
        if (magnitude > maxMagnitude) {
            return Result<uint64_t>::failure(
                ErrorCode::InvalidArgument,
                std::string(typeName) + " value is out of range");
        }
        return Result<uint64_t>::success(
            uint64_t{0} - static_cast<uint64_t>(magnitude));
    }

    const uint64_t maxValue = bits == 64
        ? (std::numeric_limits<uint64_t>::max)()
        : ((uint64_t{1} << bits) - uint64_t{1});
    if (magnitude > maxValue) {
        return Result<uint64_t>::failure(
            ErrorCode::InvalidArgument,
            std::string(typeName) + " value is out of range");
    }
    return Result<uint64_t>::success(static_cast<uint64_t>(magnitude));
}

std::vector<unsigned char> littleEndian(uint64_t value, size_t byteCount) {
    std::vector<unsigned char> bytes(byteCount);
    for (size_t i = 0; i < byteCount; ++i) {
        bytes[i] = static_cast<unsigned char>((value >> (i * 8)) & 0xFFu);
    }
    return bytes;
}

template <typename Float>
Result<std::vector<unsigned char>> encodeFloating(
    const std::string& input,
    const char* typeName) {
    const std::string text = trimAscii(input);
    if (text.empty()) {
        return Result<std::vector<unsigned char>>::failure(
            ErrorCode::InvalidArgument,
            std::string(typeName) + " value is empty");
    }

    char* end = nullptr;
    errno = 0;
    Float value;
    if constexpr (std::is_same<Float, float>::value) {
        value = std::strtof(text.c_str(), &end);
    } else {
        value = std::strtod(text.c_str(), &end);
    }
    if (end == text.c_str() || *end != '\0' || errno == ERANGE ||
        !std::isfinite(value)) {
        return Result<std::vector<unsigned char>>::failure(
            ErrorCode::InvalidArgument,
            std::string("invalid ") + typeName + " value");
    }

    std::vector<unsigned char> bytes(sizeof(Float));
    std::memcpy(bytes.data(), &value, sizeof(Float));
    return Result<std::vector<unsigned char>>::success(std::move(bytes));
}

} // namespace

const char* scalarTypeName(ScalarType type) {
    switch (type) {
        case ScalarType::Byte:   return "byte";
        case ScalarType::Word:   return "word";
        case ScalarType::Dword:  return "dword";
        case ScalarType::Qword:  return "qword";
        case ScalarType::Xor:    return "xor";
        case ScalarType::Float:  return "float";
        case ScalarType::Double: return "double";
        default:                 return "unknown";
    }
}

size_t scalarTypeSize(ScalarType type) {
    switch (type) {
        case ScalarType::Byte:   return 1;
        case ScalarType::Word:   return 2;
        case ScalarType::Dword:
        case ScalarType::Xor:
        case ScalarType::Float:  return 4;
        case ScalarType::Qword:
        case ScalarType::Double: return 8;
        default:                 return 0;
    }
}

Result<ScalarType> parseScalarType(const std::string& name) {
    if (name.size() > kMaxScalarTypeNameBytes) {
        return Result<ScalarType>::failure(
            ErrorCode::InvalidArgument,
            "data_type exceeds 64 bytes");
    }
    const std::string type = lowerAscii(trimAscii(name));
    if (type == "byte" || type == "u8" || type == "uint8" ||
        type == "int8") {
        return Result<ScalarType>::success(ScalarType::Byte);
    }
    if (type == "word" || type == "u16" || type == "uint16" ||
        type == "int16") {
        return Result<ScalarType>::success(ScalarType::Word);
    }
    if (type == "dword" || type == "u32" || type == "uint32" ||
        type == "int32") {
        return Result<ScalarType>::success(ScalarType::Dword);
    }
    if (type == "qword" || type == "u64" || type == "uint64" ||
        type == "int64") {
        return Result<ScalarType>::success(ScalarType::Qword);
    }
    if (type == "xor") {
        return Result<ScalarType>::success(ScalarType::Xor);
    }
    if (type == "float") {
        return Result<ScalarType>::success(ScalarType::Float);
    }
    if (type == "double") {
        return Result<ScalarType>::success(ScalarType::Double);
    }
    return Result<ScalarType>::failure(
        ErrorCode::InvalidArgument,
        "unsupported data_type '" + name +
            "' (supported: byte, word, dword, qword, xor, float, double)");
}

Result<std::vector<unsigned char>> encodeScalarValue(
    ScalarType type,
    const std::string& valueText) {
    if (valueText.size() > kMaxScalarValueTextBytes) {
        return Result<std::vector<unsigned char>>::failure(
            ErrorCode::InvalidArgument,
            "scalar value exceeds 256 bytes");
    }

    const size_t size = scalarTypeSize(type);
    if (size == 0) {
        return Result<std::vector<unsigned char>>::failure(
            ErrorCode::InvalidArgument,
            "invalid scalar type");
    }
    if (type == ScalarType::Float) {
        return encodeFloating<float>(valueText, scalarTypeName(type));
    }
    if (type == ScalarType::Double) {
        return encodeFloating<double>(valueText, scalarTypeName(type));
    }

    const auto value = parseIntegerBits(
        valueText, static_cast<unsigned int>(size * 8), scalarTypeName(type));
    if (!value.ok()) {
        return Result<std::vector<unsigned char>>::failure(value.error());
    }
    return Result<std::vector<unsigned char>>::success(
        littleEndian(value.value(), size));
}

Result<DecodedScalarValue> decodeScalarValue(
    ScalarType type,
    const std::vector<unsigned char>& bytes) {
    const size_t expectedSize = scalarTypeSize(type);
    if (expectedSize == 0 || bytes.size() != expectedSize) {
        return Result<DecodedScalarValue>::failure(
            ErrorCode::InvalidArgument,
            "scalar byte count does not match data_type");
    }

    DecodedScalarValue decoded;
    decoded.type = type;
    if (type == ScalarType::Float) {
        float value = 0.0f;
        std::memcpy(&value, bytes.data(), sizeof(value));
        decoded.floatingPoint = true;
        decoded.floatingValue = static_cast<double>(value);
        return Result<DecodedScalarValue>::success(decoded);
    }
    if (type == ScalarType::Double) {
        double value = 0.0;
        std::memcpy(&value, bytes.data(), sizeof(value));
        decoded.floatingPoint = true;
        decoded.floatingValue = value;
        return Result<DecodedScalarValue>::success(decoded);
    }

    for (size_t i = 0; i < bytes.size(); ++i) {
        decoded.integerValue |=
            static_cast<uint64_t>(bytes[i]) << (i * 8);
    }
    return Result<DecodedScalarValue>::success(decoded);
}

std::string formatScalarValue(const DecodedScalarValue& value) {
    if (!value.floatingPoint) {
        return std::to_string(value.integerValue);
    }
    if (std::isnan(value.floatingValue)) {
        return "nan";
    }
    if (std::isinf(value.floatingValue)) {
        return value.floatingValue < 0 ? "-inf" : "inf";
    }

    std::ostringstream output;
    output.imbue(std::locale::classic());
    const int precision = value.type == ScalarType::Float
        ? (std::numeric_limits<float>::max_digits10)
        : (std::numeric_limits<double>::max_digits10);
    output << std::setprecision(precision) << value.floatingValue;
    return output.str();
}

} // namespace Mem
