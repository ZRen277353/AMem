#include "Address.h"

#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace Mem {

namespace {

std::string trimAscii(const std::string& text) {
    size_t begin = 0;
    while (begin < text.size() &&
           std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }

    size_t end = text.size();
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return text.substr(begin, end - begin);
}

} // namespace

Result<uint64_t> parseAddress(const std::string& text, bool requireHexPrefix) {
    std::string value = trimAscii(text);
    if (value.empty()) {
        return Result<uint64_t>::failure(
            ErrorCode::InvalidArgument, "address is empty");
    }

    const bool hasPrefix = value.size() >= 2 && value[0] == '0' &&
                           (value[1] == 'x' || value[1] == 'X');
    if (requireHexPrefix && !hasPrefix) {
        return Result<uint64_t>::failure(
            ErrorCode::InvalidArgument,
            "address must be an explicit 0x-prefixed hexadecimal string");
    }
    if (hasPrefix) {
        value.erase(0, 2);
    }
    if (value.empty()) {
        return Result<uint64_t>::failure(
            ErrorCode::InvalidArgument, "address has no hexadecimal digits");
    }

    for (char c : value) {
        const bool isHex = (c >= '0' && c <= '9') ||
                           (c >= 'a' && c <= 'f') ||
                           (c >= 'A' && c <= 'F');
        if (!isHex) {
            return Result<uint64_t>::failure(
                ErrorCode::InvalidArgument,
                "address contains a non-hexadecimal character");
        }
    }

    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value.c_str(), &end, 16);
    if (errno == ERANGE || end == value.c_str() || *end != '\0' ||
        parsed > (std::numeric_limits<uint64_t>::max)()) {
        return Result<uint64_t>::failure(
            ErrorCode::InvalidArgument, "address is outside the uint64 range");
    }

    return Result<uint64_t>::success(static_cast<uint64_t>(parsed));
}

std::string formatAddress(uint64_t address) {
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "0x%llX",
                  static_cast<unsigned long long>(address));
    return buffer;
}

} // namespace Mem
