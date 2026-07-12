#ifdef HAVE_AI_CHAT

// ToolDefinitions.cpp
//
// Tool registry and legacy tool implementations. Migrated tools delegate to
// AgentMemTools -> MemService; tools not migrated yet still wrap the socket
// command layer directly. Each registration provides a JSON Schema and may
// be hidden from providers when retained only as a compatibility alias.
//
// Design notes:
//   * Socket command functions (ReadProcessMemoryBytes, ScanValue, etc.)
//     already serialise their request/response pairs internally via
//     SocketRequestManager, so the executors below just call them directly
//     (see AGENTS.md — "Socket thread safety").
//   * Schema validation is performed by ToolExecutor::execute() before the
//     lambda fires, but we still wrap all JSON access in try/catch and
//     defensively check bounds: the goal is a clear diagnostic for the AI
//     rather than a crash when inputs are unexpected.
//   * read_disassembly currently returns the raw memory bytes plus a note
//     explaining that rich disassembly rendering is a future enhancement —
//     this keeps the tool usable without pulling in the Capstone-gated
//     DisassemblyHelper header from the AI chat module.

#include "ToolExecutor.h"
#include "AgentMemTools.h"

#include "../AppContext.h"
#include "../MemoryTypes.h"
#include "../../socket/client_singleton.h"
#include "../../socket/socket_io_timeout.h"
#include "../../third_party/nlohmann/json.hpp"

#ifdef HAVE_LUAJIT
#include "../../lua/LuaEngine.h"
#endif

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <cmath>
#include <exception>
#include <initializer_list>
#include <limits>
#include <string>
#include <vector>

namespace AI {

namespace {

using nlohmann::json;

constexpr size_t kMaxToolStringParamBytes = 4096;
constexpr size_t kMaxToolHexStringBytes = 16 * 1024;
constexpr size_t kMaxToolLuaCodeBytes = 256 * 1024;
constexpr size_t kMaxToolScanHexBytes = 4096;
constexpr int kDefaultToolLuaTimeoutSeconds = 30;
constexpr int kMaxToolLuaTimeoutSeconds = 300;
constexpr int kKnownMemoryTypeMask =
    Anonymous | C_Alloc | C_Heap | C_Data | C_Bss | Java_Heap |
    Java | Stack | Video | Code_App | Code_System | Ashmem | Bad;

// ---------------------------------------------------------------------------
// JSON helpers
// ---------------------------------------------------------------------------

// Return a JSON error payload as a string. Any tool returning a non-empty
// "error" field is treated as a failure by the chat UI / model.
std::string makeError(const std::string& msg) {
    json j;
    j["error"] = msg;
    return j.dump();
}

// Wrap a successful JSON value for return. Kept as a thin helper so the
// executors read uniformly.
std::string makeOk(const json& value) {
    return value.dump();
}

// ---------------------------------------------------------------------------
// Numeric / hex parsing helpers
// ---------------------------------------------------------------------------

// Parse a hex-formatted address string such as "0x7FF00000" or "7FF00000".
// Throws std::runtime_error on malformed input so the executor can surface a
// descriptive error instead of silently interpreting as 0.
uint64_t parseHexAddress(const std::string& s) {
    if (s.empty()) throw std::runtime_error("address is empty");
    size_t begin = 0;
    while (begin < s.size() && std::isspace(static_cast<unsigned char>(s[begin]))) {
        ++begin;
    }
    size_t end = s.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    std::string trimmed = s.substr(begin, end - begin);
    if (trimmed.empty()) throw std::runtime_error("address is empty");
    // strip optional 0x/0X prefix
    if (trimmed.size() >= 2 && trimmed[0] == '0' && (trimmed[1] == 'x' || trimmed[1] == 'X')) {
        trimmed = trimmed.substr(2);
    }
    if (trimmed.empty()) throw std::runtime_error("address has no digits");
    // Validate hex digits
    for (char c : trimmed) {
        const bool ok = (c >= '0' && c <= '9') ||
                        (c >= 'a' && c <= 'f') ||
                        (c >= 'A' && c <= 'F');
        if (!ok) throw std::runtime_error("invalid hex character in address: '" + s + "'");
    }
    // std::stoull with base=16 for portability
    try {
        return std::stoull(trimmed, nullptr, 16);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("failed to parse address '") + s + "': " + e.what());
    }
}

// Format a 64-bit address as "0x" + uppercase hex (no padding).
std::string toHexAddress(uint64_t addr) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%llX", static_cast<unsigned long long>(addr));
    return std::string(buf);
}

bool addAddressOffset(uint64_t base, uint64_t offset, uint64_t& result) {
    if (base > UINT64_MAX - offset) {
        result = 0;
        return false;
    }

    result = base + offset;
    return true;
}

std::string lowerCopy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

uint64_t parseAddressJson(const json& value) {
    if (value.is_string()) {
        return parseHexAddress(value.get<std::string>());
    }
    if (value.is_number_unsigned()) {
        return value.get<uint64_t>();
    }
    if (value.is_number_integer()) {
        const auto v = value.get<long long>();
        if (v < 0) throw std::runtime_error("address must be non-negative");
        return static_cast<uint64_t>(v);
    }
    throw std::runtime_error("address must be a hex string or integer");
}

uint64_t parseOptionalAddress(const json& args,
                              const char* key,
                              uint64_t fallback) {
    if (!args.contains(key) || args[key].is_null()) {
        return fallback;
    }
    return parseAddressJson(args[key]);
}

// Parse a whitespace-separated hex-byte string (e.g. "48 65 6C 6C" or "48656C6C")
// into the corresponding byte sequence. Tolerates arbitrary ASCII whitespace and
// optional 0x prefixes between pairs.
std::vector<unsigned char> parseHexBytes(const std::string& s) {
    std::vector<unsigned char> out;
    std::string hex;
    hex.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ',') continue;
        if (c == '0' && i + 1 < s.size() && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
            ++i; // skip '0x' prefix
            continue;
        }
        hex.push_back(c);
    }
    if (hex.size() % 2 != 0) {
        throw std::runtime_error("hex byte string has odd number of nibbles");
    }
    out.reserve(hex.size() / 2);
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        return -1;
    };
    for (size_t i = 0; i < hex.size(); i += 2) {
        const int hi = nibble(hex[i]);
        const int lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) {
            throw std::runtime_error(std::string("invalid hex byte: ") +
                                     hex[i] + hex[i + 1]);
        }
        out.push_back(static_cast<unsigned char>((hi << 4) | lo));
    }
    return out;
}

// Space-separated uppercase hex representation of a byte buffer.
std::string bytesToHex(const std::vector<unsigned char>& data) {
    std::string out;
    out.reserve(data.size() * 3);
    char buf[4];
    for (size_t i = 0; i < data.size(); ++i) {
        std::snprintf(buf, sizeof(buf), "%02X", data[i]);
        if (i) out.push_back(' ');
        out.append(buf);
    }
    return out;
}

std::string bytesToCompactHex(const std::vector<unsigned char>& data) {
    std::string out;
    out.reserve(data.size() * 2);
    char buf[4];
    for (unsigned char b : data) {
        std::snprintf(buf, sizeof(buf), "%02X", b);
        out.append(buf);
    }
    return out;
}

std::string firstStringArg(const json& args,
                           std::initializer_list<const char*> names,
                           const char* fieldName) {
    for (const char* name : names) {
        if (!args.contains(name) || args[name].is_null()) continue;
        if (!args[name].is_string()) {
            throw std::runtime_error(std::string(fieldName) + " must be a string");
        }
        return args[name].get<std::string>();
    }
    throw std::runtime_error(std::string("missing required property '") + fieldName + "'");
}

std::string trimAsciiCopy(const std::string& s) {
    size_t begin = 0;
    while (begin < s.size() && std::isspace(static_cast<unsigned char>(s[begin]))) {
        ++begin;
    }
    size_t end = s.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return s.substr(begin, end - begin);
}

bool isBlankString(const std::string& value) {
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    });
}

std::string requiredStringArg(const json& args,
                              std::initializer_list<const char*> names,
                              const char* fieldName,
                              size_t maxBytes,
                              bool allowBlank = false) {
    std::string value = firstStringArg(args, names, fieldName);
    if (!allowBlank && isBlankString(value)) {
        throw std::runtime_error(std::string(fieldName) + " must not be empty");
    }
    if (value.size() > maxBytes) {
        throw std::runtime_error(std::string(fieldName) + " is too long");
    }
    return value;
}

std::string optionalStringArg(const json& args,
                              const char* key,
                              const std::string& fallback,
                              size_t maxBytes,
                              bool allowBlank = true) {
    if (!args.contains(key) || args[key].is_null()) {
        return fallback;
    }
    if (!args[key].is_string()) {
        throw std::runtime_error(std::string(key) + " must be a string");
    }
    std::string value = args[key].get<std::string>();
    if (!allowBlank && isBlankString(value)) {
        throw std::runtime_error(std::string(key) + " must not be empty");
    }
    if (value.size() > maxBytes) {
        throw std::runtime_error(std::string(key) + " is too long");
    }
    return value;
}

long long readIntegerValue(const json& value, const char* key) {
    if (value.is_number_unsigned()) {
        const uint64_t v = value.get<uint64_t>();
        if (v > static_cast<uint64_t>((std::numeric_limits<long long>::max)())) {
            throw std::runtime_error(std::string(key) + " out of range");
        }
        return static_cast<long long>(v);
    }
    if (value.is_number_integer()) {
        return value.get<long long>();
    }
    throw std::runtime_error(std::string(key) + " must be an integer");
}

int requiredIntArg(const json& args, const char* key, int minValue, int maxValue) {
    if (!args.contains(key) || args[key].is_null()) {
        throw std::runtime_error(std::string("missing required property '") + key + "'");
    }
    const long long value = readIntegerValue(args[key], key);
    if (value < minValue || value > maxValue) {
        throw std::runtime_error(std::string(key) + " out of range");
    }
    return static_cast<int>(value);
}

int optionalIntArg(const json& args,
                   const char* key,
                   int fallback,
                   int minValue,
                   int maxValue) {
    if (!args.contains(key) || args[key].is_null()) {
        return fallback;
    }
    const long long value = readIntegerValue(args[key], key);
    if (value < minValue || value > maxValue) {
        throw std::runtime_error(std::string(key) + " out of range");
    }
    return static_cast<int>(value);
}

int optionalPositiveClampedIntArg(const json& args,
                                  const char* key,
                                  int fallback,
                                  int maxValue) {
    if (!args.contains(key) || args[key].is_null()) {
        return fallback;
    }
    const long long value = readIntegerValue(args[key], key);
    if (value <= 0) {
        throw std::runtime_error(std::string(key) + " must be positive");
    }
    if (value > maxValue) {
        return maxValue;
    }
    return static_cast<int>(value);
}

bool optionalBoolArg(const json& args, const char* key, bool fallback) {
    if (!args.contains(key) || args[key].is_null()) {
        return fallback;
    }
    if (!args[key].is_boolean()) {
        throw std::runtime_error(std::string(key) + " must be a boolean");
    }
    return args[key].get<bool>();
}

std::vector<unsigned char> requiredHexBytesArg(const json& args,
                                               const char* key,
                                               size_t maxInputBytes,
                                               size_t maxDecodedBytes) {
    const std::string hex = optionalStringArg(args, key, "", maxInputBytes, false);
    std::vector<unsigned char> bytes = parseHexBytes(hex);
    if (bytes.empty()) {
        throw std::runtime_error(std::string(key) + " is empty");
    }
    if (bytes.size() > maxDecodedBytes) {
        throw std::runtime_error(std::string(key) + " exceeds maximum byte length");
    }
    return bytes;
}

std::vector<unsigned char> integerToLittleEndian(uint64_t value, int byteCount) {
    std::vector<unsigned char> out;
    out.reserve(static_cast<size_t>(byteCount));
    for (int i = 0; i < byteCount; ++i) {
        out.push_back(static_cast<unsigned char>((value >> (8 * i)) & 0xFFu));
    }
    return out;
}

uint64_t parseIntegerBits(const std::string& text, int bits, const std::string& typeName) {
    const std::string s = trimAsciiCopy(text);
    if (s.empty()) {
        throw std::runtime_error(typeName + " value is empty");
    }

    size_t consumed = 0;
    if (s[0] == '-') {
        const long long v = std::stoll(s, &consumed, 0);
        if (consumed != s.size()) {
            throw std::runtime_error("invalid trailing characters in " + typeName + " value");
        }
        const long long minValue =
            bits >= 64 ? INT64_MIN : -(1LL << (bits - 1));
        if (v < minValue) {
            throw std::runtime_error(typeName + " value out of range");
        }
        return static_cast<uint64_t>(v);
    }

    const unsigned long long v = std::stoull(s, &consumed, 0);
    if (consumed != s.size()) {
        throw std::runtime_error("invalid trailing characters in " + typeName + " value");
    }
    const uint64_t maxValue =
        bits >= 64 ? UINT64_MAX : ((uint64_t{1} << bits) - uint64_t{1});
    if (v > maxValue) {
        throw std::runtime_error(typeName + " value out of range");
    }
    return static_cast<uint64_t>(v);
}

float parseFloatValueStrict(const std::string& text, const std::string& typeName) {
    const std::string s = trimAsciiCopy(text);
    if (s.empty()) {
        throw std::runtime_error(typeName + " value is empty");
    }

    char* end = nullptr;
    errno = 0;
    const float value = std::strtof(s.c_str(), &end);
    if (end == s.c_str() || *end != '\0' || errno == ERANGE || !std::isfinite(value)) {
        throw std::runtime_error("invalid " + typeName + " value");
    }
    return value;
}

double parseDoubleValueStrict(const std::string& text, const std::string& typeName) {
    const std::string s = trimAsciiCopy(text);
    if (s.empty()) {
        throw std::runtime_error(typeName + " value is empty");
    }

    char* end = nullptr;
    errno = 0;
    const double value = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || *end != '\0' || errno == ERANGE || !std::isfinite(value)) {
        throw std::runtime_error("invalid " + typeName + " value");
    }
    return value;
}

int dataTypeToSize(const std::string& valueType) {
    const std::string t = lowerCopy(valueType);
    if (t == "byte" || t == "u8" || t == "uint8" || t == "int8") return 1;
    if (t == "word" || t == "u16" || t == "uint16" || t == "int16") return 2;
    if (t == "dword" || t == "int32" || t == "uint32" || t == "float" || t == "xor") return 4;
    if (t == "qword" || t == "int64" || t == "uint64" || t == "double") return 8;
    throw std::runtime_error("unsupported data_type '" + valueType +
                             "' (supported: byte, word, dword, qword, xor, float, double)");
}

int dataTypeToFlag(const std::string& valueType) {
    const std::string t = lowerCopy(valueType);
    if (t == "byte" || t == "u8" || t == "uint8" || t == "int8") return BYTE_;
    if (t == "word" || t == "u16" || t == "uint16" || t == "int16") return WORD_;
    if (t == "dword" || t == "int32" || t == "uint32") return DWORD_;
    if (t == "qword" || t == "int64" || t == "uint64") return QWORD_;
    if (t == "float") return FLOAT_;
    if (t == "double") return DOUBLE_;
    if (t == "xor") return XOR_;
    if (t == "bytes" || t == "string") return 0;
    throw std::runtime_error("unsupported data_type '" + valueType + "'");
}

int scanTypeToFlag(const std::string& scanType) {
    const std::string t = lowerCopy(scanType.empty() ? std::string("exact") : scanType);
    if (t == "exact") return _ACCURATE_VAL;
    if (t == "unknown") return _UNKNOW_VAL;
    if (t == "greater") return _LARGER_THAN_VAL;
    if (t == "less") return _LESS_THAN_VAL;
    if (t == "between") return _BETWEEN_VAL;
    if (t == "increased") return _ADD_UNKNOW_VAL;
    if (t == "increased_by") return _ADD_ACCURATE_VAL;
    if (t == "decreased") return _SUB_UNKNOW_VAL;
    if (t == "decreased_by") return _SUB_ACCURATE_VAL;
    if (t == "changed") return _CHANGED_VAL;
    if (t == "unchanged") return _UNCHANGED_VAL;
    throw std::runtime_error("unsupported scan_type '" + scanType + "'");
}

constexpr uint32_t kValueScanFlags =
    _ACCURATE_VAL | _LARGER_THAN_VAL |
    _LESS_THAN_VAL | _BETWEEN_VAL;
constexpr uint32_t kNextScanFlags =
    _ACCURATE_VAL | _LARGER_THAN_VAL | _LESS_THAN_VAL | _BETWEEN_VAL |
    _ADD_UNKNOW_VAL | _ADD_ACCURATE_VAL |
    _SUB_UNKNOW_VAL | _SUB_ACCURATE_VAL |
    _CHANGED_VAL | _UNCHANGED_VAL;
constexpr uint32_t kFuzzyScanFlags =
    _UNKNOW_VAL | _ADD_UNKNOW_VAL | _SUB_UNKNOW_VAL |
    _CHANGED_VAL | _UNCHANGED_VAL;
constexpr uint32_t kDataTypeFlags =
    BYTE_ | WORD_ | DWORD_ | XOR_ | FLOAT_ | QWORD_ | DOUBLE_;

int fuzzyScanTypeToFlag(const std::string& scanType) {
    const std::string t = lowerCopy(scanType.empty() ? std::string("unknown") : scanType);
    if (t == "unknown") return _UNKNOW_VAL;
    if (t == "increased") return _ADD_UNKNOW_VAL;
    if (t == "decreased") return _SUB_UNKNOW_VAL;
    if (t == "changed") return _CHANGED_VAL;
    if (t == "unchanged") return _UNCHANGED_VAL;
    throw std::runtime_error(
        "unsupported fuzzy scan_type '" + scanType +
        "' (supported: unknown, increased, decreased, changed, unchanged)");
}

bool isUntypedScanValue(const std::string& valueType) {
    const std::string t = lowerCopy(valueType);
    return t == "bytes" || t == "string";
}

void validateScanFlags(uint32_t flags,
                       uint32_t allowedScanFlags,
                       const std::string& valueType,
                       const char* toolName,
                       bool allowMissingScanMode) {
    if ((flags & ~(allowedScanFlags | kDataTypeFlags)) != 0) {
        throw std::runtime_error(std::string(toolName) + " flags contain unsupported bits");
    }

    const uint32_t scanModeBits = flags & allowedScanFlags;
    if (scanModeBits == 0 && !allowMissingScanMode) {
        throw std::runtime_error(std::string(toolName) + " flags must contain a scan type");
    }
    if (scanModeBits != 0 && (scanModeBits & (scanModeBits - 1)) != 0) {
        throw std::runtime_error(std::string(toolName) + " flags contain multiple scan types");
    }

    const uint32_t dataTypeBits = flags & kDataTypeFlags;
    if (isUntypedScanValue(valueType)) {
        if ((dataTypeBits & (dataTypeBits - 1)) != 0) {
            throw std::runtime_error(std::string(toolName) +
                                     " flags contain multiple data types");
        }
        return;
    }

    if (dataTypeBits == 0 || (dataTypeBits & (dataTypeBits - 1)) != 0) {
        throw std::runtime_error(std::string(toolName) +
                                 " flags must contain exactly one data type");
    }
}

void validateFuzzyScanFlags(uint32_t flags) {
    constexpr uint32_t kValueBearingScanFlags =
        _ACCURATE_VAL | _LARGER_THAN_VAL | _LESS_THAN_VAL | _BETWEEN_VAL |
        _ADD_ACCURATE_VAL | _SUB_ACCURATE_VAL | _GROUP_VALUE;
    if ((flags & kValueBearingScanFlags) != 0) {
        throw std::runtime_error(
            "scan_fuzzy cannot use scan types that require a comparison value");
    }
    validateScanFlags(flags, kFuzzyScanFlags, "dword", "scan_fuzzy", true);
}

int memoryTypeToFlag(const std::string& memoryType) {
    const std::string t = lowerCopy(memoryType.empty() ? std::string("all") : memoryType);
    if (t == "all") return All;
    if (t == "anonymous") return Anonymous;
    if (t == "c_alloc") return C_Alloc;
    if (t == "c_heap") return C_Heap;
    if (t == "c_data") return C_Data;
    if (t == "c_bss") return C_Bss;
    if (t == "java_heap") return Java_Heap;
    if (t == "java") return Java;
    if (t == "stack") return Stack;
    if (t == "code_app") return Code_App;
    if (t == "code_system") return Code_System;
    if (t == "video") return Video;
    if (t == "ashmem") return Ashmem;
    if (t == "bad") return Bad;
    if (t == "other") return Other;
    throw std::runtime_error("unsupported memory_type '" + memoryType + "'");
}

std::string valueTypeFromArgs(const json& args, const std::string& fallback = "dword") {
    if (args.contains("value_type") && !args["value_type"].is_null()) {
        return optionalStringArg(args, "value_type", fallback, 64, false);
    }
    return optionalStringArg(args, "data_type", fallback, 64, false);
}

bool isValidMemoryTypeFlags(int type) {
    if (type == All || type == Other) {
        return true;
    }
    return type > 0 && (type & ~kKnownMemoryTypeMask) == 0;
}

int checkedMemoryType(int type) {
    if (!isValidMemoryTypeFlags(type)) {
        throw std::runtime_error("memory type contains unsupported flags");
    }
    return type;
}

bool isValidBreakpointSize(int size) {
    return size == 1 || size == 2 || size == 4 || size == 8;
}

int memoryTypeFromArgs(const json& args) {
    const char* rawKeys[] = {"memory_type_raw", "type"};
    for (const char* key : rawKeys) {
        if (!args.contains(key) || args[key].is_null()) {
            continue;
        }
        const long long raw = readIntegerValue(args[key], key);
        if (raw < (std::numeric_limits<int>::min)() ||
            raw > (std::numeric_limits<int>::max)()) {
            throw std::runtime_error(std::string(key) + " out of range");
        }
        return checkedMemoryType(static_cast<int>(raw));
    }
    return checkedMemoryType(
        memoryTypeToFlag(optionalStringArg(args, "memory_type", "all", 64, false)));
}

bool readRawFlagArg(const json& source, const char* key, uint32_t& out) {
    if (!source.contains(key) || source[key].is_null()) {
        return false;
    }
    if (source[key].is_number_unsigned()) {
        const auto f = source[key].get<unsigned long long>();
        if (f > UINT32_MAX) {
            throw std::runtime_error(std::string(key) + " out of range");
        }
        out = static_cast<uint32_t>(f);
        return true;
    }
    if (source[key].is_number_integer()) {
        const long long f = source[key].get<long long>();
        if (f < 0 || f > UINT32_MAX) {
            throw std::runtime_error(std::string(key) + " out of range");
        }
        out = static_cast<uint32_t>(f);
        return true;
    }
    throw std::runtime_error(std::string(key) + " must be an integer");
}

bool readRawScanFlagsArg(const json& source, uint32_t& out) {
    uint32_t flagsValue = 0;
    uint32_t scanFlagValue = 0;
    const bool hasFlags = readRawFlagArg(source, "flags", flagsValue);
    const bool hasScanFlag = readRawFlagArg(source, "scan_flag", scanFlagValue);
    if (hasFlags && hasScanFlag && flagsValue != scanFlagValue) {
        throw std::runtime_error(
            "flags and scan_flag must match when both are provided");
    }
    if (hasFlags) {
        out = flagsValue;
        return true;
    }
    if (hasScanFlag) {
        out = scanFlagValue;
        return true;
    }
    return false;
}

bool scanNextFlagRequiresValue(uint32_t flag) {
    return (flag & (_ADD_UNKNOW_VAL | _SUB_UNKNOW_VAL |
                    _CHANGED_VAL | _UNCHANGED_VAL)) == 0;
}

size_t scanValueSizeFromFlags(uint32_t flags) {
    if ((flags & BYTE_) != 0) return 1;
    if ((flags & WORD_) != 0) return 2;
    if ((flags & (DWORD_ | XOR_ | FLOAT_)) != 0) return 4;
    if ((flags & (QWORD_ | DOUBLE_)) != 0) return 8;
    return 0;
}

size_t scanSingleValueSize(const std::string& valueType, uint32_t flags) {
    const size_t flagSize = scanValueSizeFromFlags(flags);
    if (flagSize != 0) {
        return flagSize;
    }
    return static_cast<size_t>(dataTypeToSize(valueType));
}

uint32_t scanFlagsFromArgs(const json& args, const std::string& valueType) {
    uint32_t rawFlag = 0;
    if (readRawScanFlagsArg(args, rawFlag)) {
        return rawFlag;
    }
    const std::string scanType = optionalStringArg(args, "scan_type", "exact", 64, false);
    return static_cast<uint32_t>(scanTypeToFlag(scanType) | dataTypeToFlag(valueType));
}

void parseScanRange(const json& args, uint64_t& start, uint64_t& end) {
    start = parseOptionalAddress(args, "start", 0);
    end = parseOptionalAddress(args, "end", UINT64_MAX);
    if (start > end) {
        throw std::runtime_error("scan start must be <= end");
    }
}

uint32_t fuzzyScanFlagsFromArgs(const json& args, const std::string& valueType) {
    uint32_t rawFlag = 0;
    if (readRawScanFlagsArg(args, rawFlag)) {
        validateFuzzyScanFlags(rawFlag);
        return rawFlag;
    }
    const std::string scanType = optionalStringArg(args, "scan_type", "unknown", 64, false);
    return static_cast<uint32_t>(fuzzyScanTypeToFlag(scanType) | dataTypeToFlag(valueType));
}

// Encode a value (int/float/etc.) as a byte vector for scan/write tools.
// Supports both legacy value_type names (int32/int64/bytes/string) and
// MCP-style data_type names (byte/word/dword/qword/xor/float/double).
std::vector<unsigned char> encodeScanValue(const std::string& valueType, const json& value) {
    const std::string t = lowerCopy(valueType);

    auto asString = [&]() -> std::string {
        if (value.is_string()) return value.get<std::string>();
        if (value.is_number_unsigned()) return std::to_string(value.get<unsigned long long>());
        if (value.is_number_integer()) return std::to_string(value.get<long long>());
        if (value.is_number_float()) return std::to_string(value.get<double>());
        throw std::runtime_error("value must be string or number for type '" + t + "'");
    };

    std::vector<unsigned char> out;

    if (t == "byte" || t == "u8" || t == "uint8") {
        out = integerToLittleEndian(parseIntegerBits(asString(), 8, valueType), 1);
    } else if (t == "int8") {
        out = integerToLittleEndian(parseIntegerBits(asString(), 8, valueType), 1);
    } else if (t == "word" || t == "u16" || t == "uint16") {
        out = integerToLittleEndian(parseIntegerBits(asString(), 16, valueType), 2);
    } else if (t == "int16") {
        out = integerToLittleEndian(parseIntegerBits(asString(), 16, valueType), 2);
    } else if (t == "dword" || t == "uint32" || t == "xor") {
        out = integerToLittleEndian(parseIntegerBits(asString(), 32, valueType), 4);
    } else if (t == "int32") {
        out = integerToLittleEndian(parseIntegerBits(asString(), 32, valueType), 4);
    } else if (t == "qword" || t == "uint64") {
        out = integerToLittleEndian(parseIntegerBits(asString(), 64, valueType), 8);
    } else if (t == "int64") {
        out = integerToLittleEndian(parseIntegerBits(asString(), 64, valueType), 8);
    } else if (t == "float") {
        // Take a JSON number directly rather than round-tripping through
        // std::to_string(): std::to_string(double) formats with "%f" (6
        // fractional digits), which would silently truncate the value before
        // it is re-parsed. Only string inputs go through the strict parser.
        float fv;
        if (value.is_number()) {
            fv = value.get<float>();
            if (!std::isfinite(fv)) {
                throw std::runtime_error("invalid " + valueType + " value");
            }
        } else {
            fv = parseFloatValueStrict(asString(), valueType);
        }
        out.resize(sizeof(fv));
        std::memcpy(out.data(), &fv, sizeof(fv));
    } else if (t == "double") {
        // See the float branch above: avoid the lossy std::to_string round-trip.
        double dv;
        if (value.is_number()) {
            dv = value.get<double>();
            if (!std::isfinite(dv)) {
                throw std::runtime_error("invalid " + valueType + " value");
            }
        } else {
            dv = parseDoubleValueStrict(asString(), valueType);
        }
        out.resize(sizeof(dv));
        std::memcpy(out.data(), &dv, sizeof(dv));
    } else if (t == "bytes") {
        out = parseHexBytes(asString());
        if (out.empty()) throw std::runtime_error("bytes value is empty");
    } else if (t == "string") {
        const std::string s = asString();
        out.assign(s.begin(), s.end());
        if (out.empty()) throw std::runtime_error("string value is empty");
    } else {
        throw std::runtime_error("unsupported value_type '" + valueType +
                                 "' (supported: byte, word, dword, qword, xor, int32, int64, float, double, bytes, string)");
    }
    return out;
}

std::vector<unsigned char> scanBytesFromArgs(const json& args,
                                             const std::string& valueType,
                                             const char* valueFieldName) {
    if (args.contains("value_hex") && !args["value_hex"].is_null()) {
        return requiredHexBytesArg(args, "value_hex", kMaxToolHexStringBytes, kMaxToolScanHexBytes);
    }
    if (args.contains("hex") && !args["hex"].is_null()) {
        return requiredHexBytesArg(args, "hex", kMaxToolHexStringBytes, kMaxToolScanHexBytes);
    }
    if (!args.contains(valueFieldName)) {
        throw std::runtime_error(std::string("missing required property '") +
                                 valueFieldName +
                                 "' (or provide value_hex/hex)");
    }
    return encodeScanValue(valueType, args.at(valueFieldName));
}

void validateScanBytesForFlags(const std::string& valueType,
                               uint32_t flags,
                               const std::vector<unsigned char>& bytes) {
    if (bytes.empty()) {
        throw std::runtime_error("scan value is empty");
    }
    if (bytes.size() > kMaxToolScanHexBytes) {
        throw std::runtime_error("scan value exceeds maximum byte length");
    }

    const std::string t = lowerCopy(valueType);
    if (t == "bytes" || t == "string") {
        return;
    }

    const size_t singleValueSize = scanSingleValueSize(valueType, flags);
    const size_t expected =
        (flags & _BETWEEN_VAL) != 0 ? singleValueSize * 2 : singleValueSize;
    if (bytes.size() != expected) {
        throw std::runtime_error("scan value size must match data_type");
    }
}

void appendBetweenUpperBound(const json& args,
                             const std::string& valueType,
                             uint32_t flags,
                             std::vector<unsigned char>& bytes) {
    const size_t singleValueSize = scanSingleValueSize(valueType, flags);
    const bool hasUpper =
        args.contains("value2") || args.contains("value2_hex") ||
        args.contains("max_value") ||
        args.contains("upper_value");

    if (!hasUpper) {
        if (bytes.size() == singleValueSize * 2) {
            return;
        }
        throw std::runtime_error(
            "between scan requires value2, max_value, or upper_value");
    }

    if (bytes.size() != singleValueSize) {
        throw std::runtime_error(
            "between scan lower value must encode exactly one value");
    }

    const json* upper = nullptr;
    if (args.contains("value2_hex") && !args["value2_hex"].is_null()) {
        std::vector<unsigned char> upperBytes =
            requiredHexBytesArg(args, "value2_hex", kMaxToolHexStringBytes, kMaxToolScanHexBytes);
        if (upperBytes.size() != bytes.size()) {
            throw std::runtime_error("between scan values must use the same encoded size");
        }
        bytes.insert(bytes.end(), upperBytes.begin(), upperBytes.end());
        return;
    }

    if (args.contains("value2")) {
        upper = &args.at("value2");
    } else if (args.contains("max_value")) {
        upper = &args.at("max_value");
    } else {
        upper = &args.at("upper_value");
    }

    std::vector<unsigned char> upperBytes = encodeScanValue(valueType, *upper);
    if (upperBytes.size() != bytes.size()) {
        throw std::runtime_error("between scan values must use the same encoded size");
    }
    bytes.insert(bytes.end(), upperBytes.begin(), upperBytes.end());
}

std::vector<unsigned char> placeholderScanValue(const std::string& valueType, uint32_t flags) {
    return std::vector<unsigned char>(scanSingleValueSize(valueType, flags), 0);
}

// ---------------------------------------------------------------------------
// Individual tool executors
// ---------------------------------------------------------------------------

// status / get_status compatibility alias
std::string execGetStatus(const std::string& /*argsJson*/,
                          const Mem::OperationContext& context) {
    return getAgentMemTools().status("{}", context);
}

// get_server_version
std::string execGetServerVersion(const std::string& /*argsJson*/,
                                 const Mem::OperationContext& context) {
    return getAgentMemTools().serverVersion("{}", context);
}

// get_architecture
std::string execGetArchitecture(const std::string& /*argsJson*/,
                                const Mem::OperationContext& context) {
    return getAgentMemTools().architecture("{}", context);
}

// init_driver
std::string execInitDriver(const std::string& argsJson) {
    try {
        const json args = json::parse(argsJson.empty() ? std::string("{}") : argsJson);
        std::string card = requiredStringArg(
            args, {"card_name", "card"}, "card_name", kMaxToolStringParamBytes);
        std::string message;
        if (!InitDriver(card, message, PORT_MAIN)) {
            return makeError("socket communication error: init_driver");
        }
        json result;
        result["message"] = message;
        return makeOk(result);
    } catch (const std::exception& e) {
        return makeError(std::string("init_driver: ") + e.what());
    }
}

// canonical memory_read
std::string execMemoryRead(const std::string& argsJson,
                           const Mem::OperationContext& context) {
    return getAgentMemTools().memoryRead(argsJson, false, context);
}

// hidden read_memory compatibility alias
std::string execReadMemory(const std::string& argsJson,
                           const Mem::OperationContext& context) {
    return getAgentMemTools().memoryRead(argsJson, true, context);
}

// canonical memory_read_value
std::string execMemoryReadValue(const std::string& argsJson,
                                const Mem::OperationContext& context) {
    return getAgentMemTools().memoryReadValue(argsJson, false, context);
}

// hidden read_value compatibility alias
std::string execReadValue(const std::string& argsJson,
                          const Mem::OperationContext& context) {
    return getAgentMemTools().memoryReadValue(argsJson, true, context);
}

// memory_write
std::string execMemoryWrite(const std::string& argsJson,
                            const Mem::OperationContext& context) {
    return getAgentMemTools().memoryWrite(argsJson, false, context);
}

// hidden write_bytes compatibility alias
std::string execWriteBytes(const std::string& argsJson,
                           const Mem::OperationContext& context) {
    return getAgentMemTools().memoryWrite(argsJson, true, context);
}

// canonical memory_write_value
std::string execMemoryWriteValue(const std::string& argsJson,
                                 const Mem::OperationContext& context) {
    return getAgentMemTools().memoryWriteValue(argsJson, false, context);
}

// hidden write_value compatibility alias
std::string execWriteValue(const std::string& argsJson,
                           const Mem::OperationContext& context) {
    return getAgentMemTools().memoryWriteValue(argsJson, true, context);
}

// scan_value
std::string execScanStart(const std::string& argsJson,
                          const Mem::OperationContext& context) {
    return getAgentMemTools().scanStart(argsJson, context);
}

std::string execScanRefine(const std::string& argsJson,
                           const Mem::OperationContext& context) {
    return getAgentMemTools().scanRefine(argsJson, context);
}

std::string execScanResults(const std::string& argsJson,
                            const Mem::OperationContext& context) {
    return getAgentMemTools().scanResults(argsJson, context);
}

std::string execScanClear(const std::string& argsJson,
                          const Mem::OperationContext& context) {
    return getAgentMemTools().scanClear(argsJson, context);
}

// hidden scan_value compatibility alias
std::string execScanValue(const std::string& argsJson) {
    try {
        const json args = json::parse(argsJson.empty() ? std::string("{}") : argsJson);
        const std::string valueType = valueTypeFromArgs(args);
        std::vector<unsigned char> bytes = scanBytesFromArgs(args, valueType, "value");

        const uint32_t flags = scanFlagsFromArgs(args, valueType);
        validateScanFlags(flags, kValueScanFlags, valueType, "scan_value", false);
        if ((flags & _BETWEEN_VAL) != 0) {
            appendBetweenUpperBound(args, valueType, flags, bytes);
        }
        validateScanBytesForFlags(valueType, flags, bytes);
        uint64_t start = 0;
        uint64_t end = UINT64_MAX;
        parseScanRange(args, start, end);
        const int memoryType = memoryTypeFromArgs(args);

        if (!ScanSetRange(memoryType, PORT_MAIN)) {
            return makeError("socket communication error: scan_set_range");
        }
        const int count = ScanValue(flags, bytes, start, end, PORT_MAIN);
        if (count < 0) {
            return makeError("socket communication error: scan_value");
        }

        json result;
        result["count"] = count;
        result["result_count"] = count;
        return makeOk(result);
    } catch (const std::exception& e) {
        return makeError(std::string("scan_value: ") + e.what());
    }
}

// scan_next
std::string execScanNext(const std::string& argsJson) {
    try {
        const json args = json::parse(argsJson.empty() ? std::string("{}") : argsJson);
        const std::string valueType = valueTypeFromArgs(args);
        uint32_t rawFlag = 0;
        const uint32_t flags = readRawScanFlagsArg(args, rawFlag)
                                   ? rawFlag
                                   : scanFlagsFromArgs(args, valueType);
        validateScanFlags(flags, kNextScanFlags, valueType, "scan_next", false);
        if (flags > static_cast<uint32_t>(INT32_MAX)) {
            return makeError("scan_flag out of range");
        }
        const bool hasValue =
            args.contains("value") || args.contains("value_hex") || args.contains("hex");
        std::vector<unsigned char> bytes;
        if (hasValue) {
            bytes = scanBytesFromArgs(args, valueType, "value");
            if ((flags & _BETWEEN_VAL) != 0) {
                appendBetweenUpperBound(args, valueType, flags, bytes);
            }
        } else if (scanNextFlagRequiresValue(flags)) {
            return makeError("scan_next requires value/value_hex/hex for this scan_type");
        } else {
            bytes = placeholderScanValue(valueType, flags);
        }
        validateScanBytesForFlags(valueType, flags, bytes);
        uint64_t start = 0;
        uint64_t end = UINT64_MAX;
        parseScanRange(args, start, end);

        const int count = ScanNextValue(bytes, static_cast<int>(flags), start, end, PORT_MAIN);
        if (count < 0) {
            return makeError("socket communication error: scan_next");
        }

        json result;
        result["count"] = count;
        result["result_count"] = count;
        return makeOk(result);
    } catch (const std::exception& e) {
        return makeError(std::string("scan_next: ") + e.what());
    }
}

// scan_fuzzy
std::string execScanFuzzy(const std::string& argsJson) {
    try {
        const json args = json::parse(argsJson.empty() ? std::string("{}") : argsJson);
        const std::string valueType = valueTypeFromArgs(args);
        const uint32_t flags = fuzzyScanFlagsFromArgs(args, valueType);
        uint64_t start = 0;
        uint64_t end = UINT64_MAX;
        parseScanRange(args, start, end);
        const int memoryType = memoryTypeFromArgs(args);

        if (!ScanSetRange(memoryType, PORT_MAIN)) {
            return makeError("socket communication error: scan_set_range");
        }
        const int count = ScanFuzzyValueWithProgress(flags, nullptr, nullptr, start, end, PORT_MAIN);
        if (count < 0) {
            return makeError("socket communication error: scan_fuzzy");
        }

        json result;
        result["count"] = count;
        result["result_count"] = count;
        return makeOk(result);
    } catch (const std::exception& e) {
        return makeError(std::string("scan_fuzzy: ") + e.what());
    }
}

// scan_hex
std::string execScanHex(const std::string& argsJson) {
    try {
        const json args = json::parse(argsJson.empty() ? std::string("{}") : argsJson);
        const std::string pattern = requiredStringArg(
            args, {"hex_pattern", "pattern_hex", "value"}, "hex_pattern", kMaxToolHexStringBytes);
        std::vector<unsigned char> bytes = parseHexBytes(pattern);
        if (bytes.empty()) {
            return makeError("hex_pattern must contain at least one byte");
        }
        if (bytes.size() > kMaxToolScanHexBytes) {
            return makeError("hex_pattern exceeds maximum byte length");
        }
        uint64_t start = 0;
        uint64_t end = UINT64_MAX;
        parseScanRange(args, start, end);
        const int memoryType = memoryTypeFromArgs(args);

        if (!ScanSetRange(memoryType, PORT_MAIN)) {
            return makeError("socket communication error: scan_set_range");
        }
        const int count = ScanHEXValueWithProgress(start, end, bytes, nullptr, nullptr, PORT_MAIN);
        if (count < 0) {
            return makeError("socket communication error: scan_hex");
        }
        json result;
        result["count"] = count;
        result["result_count"] = count;
        return makeOk(result);
    } catch (const std::exception& e) {
        return makeError(std::string("scan_hex: ") + e.what());
    }
}

// scan_set_range
std::string execScanSetRange(const std::string& argsJson) {
    try {
        const json args = json::parse(argsJson.empty() ? std::string("{}") : argsJson);
        const int type = memoryTypeFromArgs(args);
        if (!ScanSetRange(type, PORT_MAIN)) {
            return makeError("socket communication error: scan_set_range");
        }
        json result;
        result["type"] = type;
        result["set"] = true;
        return makeOk(result);
    } catch (const std::exception& e) {
        return makeError(std::string("scan_set_range: ") + e.what());
    }
}

// get_scan_count
std::string execGetScanCount(const std::string& /*argsJson*/) {
    try {
        const int count = GetScanResultCount(PORT_MAIN);
        if (count < 0) {
            return makeError("socket communication error: get_scan_count");
        }
        json result;
        result["count"] = count;
        return makeOk(result);
    } catch (const std::exception& e) {
        return makeError(std::string("get_scan_count: ") + e.what());
    }
}

// get_scan_results
std::string execGetScanResults(const std::string& argsJson) {
    try {
        const json args = argsJson.empty() ? json::object() : json::parse(argsJson);

        // Fetch the total first so the offset can be clamped against it. This
        // avoids sending an out-of-range offset to the device (which surfaces
        // as an opaque socket error) and mirrors the IPC get_scan_results path.
        const int total = GetScanResultCount(PORT_MAIN);
        if (total < 0) {
            return makeError("socket communication error: get_scan_count");
        }
        // Clamp a past-the-end offset to `total` (an empty page) instead of
        // erroring, matching get_module_list and the IPC get_scan_results path.
        int offset = optionalIntArg(args, "offset", 0, 0, (std::numeric_limits<int>::max)());
        if (offset > total) offset = total;
        const int count = optionalIntArg(args, "count", 100, 1, 1000);

        json entries = json::array();
        if (offset < total) {
            std::vector<std::pair<uint64_t, uint64_t>> raw;
            if (!GetScanResult(offset, count, raw, PORT_MAIN)) {
                return makeError("socket communication error: get_scan_results");
            }
            for (const auto& kv : raw) {
                json entry;
                entry["address"] = toHexAddress(kv.first);
                entry["value"] = static_cast<uint64_t>(kv.second);
                entries.push_back(std::move(entry));
            }
        }

        json result;
        result["total"] = total;
        result["offset"] = offset;
        result["results"] = std::move(entries);
        result["items"] = result["results"];
        return makeOk(result);
    } catch (const std::exception& e) {
        return makeError(std::string("get_scan_results: ") + e.what());
    }
}

// clear_scan
std::string execClearScan(const std::string& /*argsJson*/) {
    try {
        if (!ClearScanResult(PORT_MAIN)) {
            return makeError("socket communication error: clear_scan");
        }
        json result;
        result["cleared"] = true;
        return makeOk(result);
    } catch (const std::exception& e) {
        return makeError(std::string("clear_scan: ") + e.what());
    }
}

// canonical module_list
std::string execModuleList(const std::string& argsJson,
                           const Mem::OperationContext& context) {
    return getAgentMemTools().moduleList(argsJson, false, context);
}

// hidden get_module_list compatibility alias
std::string execGetModuleList(const std::string& argsJson,
                              const Mem::OperationContext& context) {
    return getAgentMemTools().moduleList(argsJson, true, context);
}

// hidden list_modules compatibility alias
std::string execListModules(const std::string& argsJson,
                            const Mem::OperationContext& context) {
    return getAgentMemTools().moduleList(argsJson, true, context);
}

// process_list / get_process_list compatibility alias
std::string execGetProcessList(const std::string& /*argsJson*/,
                               const Mem::OperationContext& context) {
    return getAgentMemTools().processList("{}", context);
}

// list_processes
std::string execListProcesses(const std::string& argsJson,
                              const Mem::OperationContext& context) {
    return getAgentMemTools().processList(argsJson, context);
}

// process_open / open_process compatibility alias. MemService resolves the
// optional name, performs the target switch, and returns a new snapshot.
std::string execOpenProcess(const std::string& argsJson,
                            const Mem::OperationContext& context) {
    return getAgentMemTools().processOpen(argsJson, context);
}

// canonical module_resolve
std::string execModuleResolve(const std::string& argsJson,
                              const Mem::OperationContext& context) {
    return getAgentMemTools().moduleResolve(argsJson, false, context);
}

// hidden get_module_base compatibility alias
std::string execGetModuleBase(const std::string& argsJson,
                              const Mem::OperationContext& context) {
    return getAgentMemTools().moduleResolve(argsJson, true, context);
}

// canonical pointer_resolve
std::string execPointerResolve(const std::string& argsJson,
                               const Mem::OperationContext& context) {
    return getAgentMemTools().pointerResolve(argsJson, false, context);
}

// hidden resolve_offset_chain compatibility alias
std::string execResolveOffsetChain(const std::string& argsJson,
                                   const Mem::OperationContext& context) {
    return getAgentMemTools().pointerResolve(argsJson, true, context);
}

std::string execDisassemble(
    const std::string& argsJson,
    const Mem::OperationContext& context) {
    return getAgentMemTools().disassemble(argsJson, context);
}

// Hidden compatibility implementation for read_disassembly.
//
// The AI chat module is compiled independently of HAVE_CAPSTONE, so rather
// than pulling in DisassemblyHelper we return the raw instruction bytes and
// note that full disassembly rendering is not yet wired through the tool
// interface. The AI still receives enough to reason about instruction
// encodings or to display them to the user.
std::string execReadDisassemblyLegacy(const std::string& argsJson) {
    try {
        const json args = json::parse(argsJson.empty() ? std::string("{}") : argsJson);
        const uint64_t address = parseAddressJson(args.at("address"));
        const int countRaw = requiredIntArg(args, "count", 1, 512);

        // ARM64 instructions are fixed 4 bytes wide; read count * 4 bytes.
        const uint32_t size = static_cast<uint32_t>(countRaw) * 4u;
        std::vector<unsigned char> bytes;
        if (!ReadProcessMemoryBytes(address, size, bytes, PORT_MAIN)) {
            return makeError("socket communication error: read_disassembly");
        }

        // TODO: wire DisassemblyHelper through the tool layer once Capstone
        // is unconditionally available in this translation unit. Until then,
        // return raw 32-bit instruction words plus an explanatory note so
        // the AI can still operate on the bytes.
        json instructions = json::array();
        for (size_t i = 0; i + 4 <= bytes.size(); i += 4) {
            uint64_t instructionAddress = 0;
            if (!addAddressOffset(address, static_cast<uint64_t>(i), instructionAddress)) {
                break;
            }
            uint32_t insn = static_cast<uint32_t>(bytes[i]) |
                            (static_cast<uint32_t>(bytes[i + 1]) << 8) |
                            (static_cast<uint32_t>(bytes[i + 2]) << 16) |
                            (static_cast<uint32_t>(bytes[i + 3]) << 24);
            char hexBuf[16];
            std::snprintf(hexBuf, sizeof(hexBuf), "0x%08X", insn);
            json entry;
            entry["address"] = toHexAddress(instructionAddress);
            entry["encoding"] = hexBuf;
            instructions.push_back(std::move(entry));
        }

        json result;
        result["address"] = toHexAddress(address);
        result["count"] = countRaw;
        result["instructions"] = std::move(instructions);
        result["raw_bytes"] = bytesToHex(bytes);
        result["note"] = "Raw ARM64 instruction words (little-endian). "
                         "Symbolic disassembly rendering via the tool interface "
                         "is a future enhancement.";
        return makeOk(result);
    } catch (const std::exception& e) {
        return makeError(std::string("read_disassembly: ") + e.what());
    }
}

std::string execBreakpointSet(
    const std::string& argsJson,
    const Mem::OperationContext& context) {
    return getAgentMemTools().breakpointSet(argsJson, context);
}

std::string execBreakpointRemove(
    const std::string& argsJson,
    const Mem::OperationContext& context) {
    return getAgentMemTools().breakpointRemove(argsJson, context);
}

std::string execBreakpointSuspend(
    const std::string& argsJson,
    const Mem::OperationContext& context) {
    return getAgentMemTools().breakpointSuspend(argsJson, context);
}

std::string execBreakpointResume(
    const std::string& argsJson,
    const Mem::OperationContext& context) {
    return getAgentMemTools().breakpointResume(argsJson, context);
}

std::string execBreakpointHits(
    const std::string& argsJson,
    const Mem::OperationContext& context) {
    return getAgentMemTools().breakpointHits(argsJson, context);
}

// Hidden compatibility implementation for set_breakpoint.
std::string execSetBreakpointLegacy(const std::string& argsJson) {
    try {
        const json args = json::parse(argsJson.empty() ? std::string("{}") : argsJson);
        const uint64_t address = parseAddressJson(args.at("address"));
        const int bpType = optionalIntArg(args, "bp_type", 2, 1, 4);
        const int bpSize = optionalIntArg(args, "bp_size", 4, 1, 8);
        if (!isValidBreakpointSize(bpSize)) {
            return makeError("bp_size must be 1, 2, 4, or 8");
        }
        const int effectiveSize = (bpType == 4) ? 4 : bpSize;

        const bool ok = SetKernelBreakpoint(address,
                                            static_cast<uint32_t>(bpType),
                                            static_cast<uint32_t>(effectiveSize),
                                            PORT_MAIN);
        if (!ok) {
            return makeError("socket communication error: set_breakpoint");
        }

        json result;
        result["address"] = toHexAddress(address);
        result["bp_type"] = bpType;
        result["bp_size"] = effectiveSize;
        result["set"] = true;
        return makeOk(result);
    } catch (const std::exception& e) {
        return makeError(std::string("set_breakpoint: ") + e.what());
    }
}

// Hidden compatibility implementation for remove_breakpoint.
std::string execRemoveBreakpointLegacy(const std::string& argsJson) {
    try {
        const json args = json::parse(argsJson.empty() ? std::string("{}") : argsJson);
        const uint64_t address = parseAddressJson(args.at("address"));

        const bool ok = RemoveKernelBreakpoint(address, PORT_MAIN);
        if (!ok) {
            return makeError("socket communication error: remove_breakpoint");
        }

        json result;
        result["address"] = toHexAddress(address);
        result["removed"] = true;
        return makeOk(result);
    } catch (const std::exception& e) {
        return makeError(std::string("remove_breakpoint: ") + e.what());
    }
}

// Hidden compatibility implementation for suspend_breakpoint.
std::string execSuspendBreakpointLegacy(const std::string& argsJson) {
    try {
        const json args = json::parse(argsJson.empty() ? std::string("{}") : argsJson);
        const uint64_t address = parseAddressJson(args.at("address"));
        if (!SuspendKernelBreakpoint(address, PORT_MAIN)) {
            return makeError("socket communication error: suspend_breakpoint");
        }
        json result;
        result["address"] = toHexAddress(address);
        result["suspended"] = true;
        return makeOk(result);
    } catch (const std::exception& e) {
        return makeError(std::string("suspend_breakpoint: ") + e.what());
    }
}

// Hidden compatibility implementation for resume_breakpoint.
std::string execResumeBreakpointLegacy(const std::string& argsJson) {
    try {
        const json args = json::parse(argsJson.empty() ? std::string("{}") : argsJson);
        const uint64_t address = parseAddressJson(args.at("address"));
        if (!ResumeKernelBreakpoint(address, PORT_MAIN)) {
            return makeError("socket communication error: resume_breakpoint");
        }
        json result;
        result["address"] = toHexAddress(address);
        result["resumed"] = true;
        return makeOk(result);
    } catch (const std::exception& e) {
        return makeError(std::string("resume_breakpoint: ") + e.what());
    }
}

// Hidden compatibility implementation for read_breakpoint_info.
std::string execReadBreakpointInfoLegacy(const std::string& argsJson) {
    try {
        const json args = json::parse(argsJson.empty() ? std::string("{}") : argsJson);
        const uint64_t address = parseAddressJson(args.at("address"));
        std::vector<HW_HIT_INFO> infos;
        if (!ReadKernelBreakpointInfo(address, infos, PORT_MAIN)) {
            return makeError("socket communication error: read_breakpoint_info");
        }
        json hits = json::array();
        for (const auto& h : infos) {
            json regs = json::array();
            for (int i = 0; i < 31; ++i) {
                regs.push_back(h.regs_info.regs[i]);
            }
            json item;
            item["hit_addr"] = toHexAddress(h.hit_addr);
            item["hit_time"] = h.hit_time;
            item["pc"] = toHexAddress(h.regs_info.pc);
            item["sp"] = toHexAddress(h.regs_info.sp);
            item["pstate"] = h.regs_info.pstate;
            item["regs"] = std::move(regs);
            hits.push_back(std::move(item));
        }
        json result;
        result["address"] = toHexAddress(address);
        result["hits"] = std::move(hits);
        return makeOk(result);
    } catch (const std::exception& e) {
        return makeError(std::string("read_breakpoint_info: ") + e.what());
    }
}

std::string execSymbolResolve(
    const std::string& argsJson,
    const Mem::OperationContext& context) {
    return getAgentMemTools().symbolResolve(argsJson, context);
}

std::string execSymbolList(
    const std::string& argsJson,
    const Mem::OperationContext& context) {
    return getAgentMemTools().symbolList(argsJson, context);
}

// Hidden compatibility implementation for resolve_symbol.
std::string execResolveSymbolLegacy(const std::string& argsJson) {
    try {
        const json args = json::parse(argsJson.empty() ? std::string("{}") : argsJson);
        const std::string moduleName = requiredStringArg(
            args, {"module_name", "module"}, "module_name", kMaxToolStringParamBytes);
        const std::string symbolName = requiredStringArg(
            args, {"symbol_name", "name"}, "symbol_name", kMaxToolStringParamBytes);

        uint64_t base = 0;
        if (!GetModuleBaseByName(moduleName, base, PORT_MAIN)) {
            return makeError("socket communication error: could not resolve module '" + moduleName + "'");
        }

        uint64_t addr = 0;
        if (!SymbolFind(base, symbolName, addr, PORT_MAIN)) {
            return makeError("socket communication error: could not resolve symbol '" +
                             symbolName + "' in module '" + moduleName + "'");
        }

        json result;
        result["module"] = moduleName;
        result["module_base"] = toHexAddress(base);
        result["symbol"] = symbolName;
        result["address"] = toHexAddress(addr);
        return makeOk(result);
    } catch (const std::exception& e) {
        return makeError(std::string("resolve_symbol: ") + e.what());
    }
}

// symbol_init
std::string execSymbolInit(const std::string& argsJson) {
    try {
        const json args = json::parse(argsJson.empty() ? std::string("{}") : argsJson);
        const uint64_t moduleBase = parseAddressJson(args.at("module_base"));
        int totalCount = 0;
        if (!SymbolInit(moduleBase, totalCount, PORT_MAIN)) {
            return makeError("socket communication error: symbol_init");
        }
        json result;
        result["module_base"] = toHexAddress(moduleBase);
        result["total_count"] = totalCount;
        return makeOk(result);
    } catch (const std::exception& e) {
        return makeError(std::string("symbol_init: ") + e.what());
    }
}

// symbol_find
std::string execSymbolFind(const std::string& argsJson) {
    try {
        const json args = json::parse(argsJson.empty() ? std::string("{}") : argsJson);
        const uint64_t moduleBase = parseAddressJson(args.at("module_base"));
        const std::string name = requiredStringArg(
            args, {"symbol_name", "name"}, "symbol_name", kMaxToolStringParamBytes);
        uint64_t address = 0;
        if (!SymbolFind(moduleBase, name, address, PORT_MAIN) || address == 0) {
            return makeError("socket communication error: symbol_find");
        }
        json result;
        result["module_base"] = toHexAddress(moduleBase);
        result["symbol"] = name;
        result["address"] = toHexAddress(address);
        return makeOk(result);
    } catch (const std::exception& e) {
        return makeError(std::string("symbol_find: ") + e.what());
    }
}

// execute_lua
std::string execExecuteLua(const std::string& argsJson) {
#ifdef HAVE_LUAJIT
    try {
        const json args = json::parse(argsJson.empty() ? std::string("{}") : argsJson);
        const std::string code = requiredStringArg(
            args, {"code"}, "code", kMaxToolLuaCodeBytes);
        const int timeoutSeconds = optionalIntArg(
            args, "timeout_seconds", kDefaultToolLuaTimeoutSeconds,
            1, kMaxToolLuaTimeoutSeconds);
        SocketIoTimeout::ScopedTimeout luaTimeout(timeoutSeconds);
        auto& engine = LuaEngine::GetInstance();
        if (!engine.IsInitialized() && !engine.Initialize()) {
            return makeError("Lua engine initialization failed: " + engine.GetLastError());
        }
        std::string output;
        if (!engine.ExecuteStringCapture(
                code,
                "ai_tool",
                output,
                static_cast<int>(SocketIoTimeout::GetRemainingTimeoutMs()))) {
            json err;
            err["error"] = engine.GetLastError();
            err["output"] = output;
            return err.dump();
        }
        json result;
        result["output"] = output;
        return makeOk(result);
    } catch (const std::exception& e) {
        return makeError(std::string("execute_lua: ") + e.what());
    }
#else
    (void)argsJson;
    return makeError("execute_lua unavailable: AMem was built without LuaJIT");
#endif
}

// ---------------------------------------------------------------------------
// Schema literals
// ---------------------------------------------------------------------------
//
// JSON Schemas are written as raw-string literals so they can be read
// verbatim alongside the executor. ToolExecutor parses and caches them at
// registration time; no runtime build cost here.

// A non-empty delimiter is required because some descriptions contain the
// literal ')"' sequence (e.g. "... (1-4096)") which would otherwise
// terminate the raw string early.
constexpr const char* kSchemaMemoryRead = R"JSON({
  "type": "object",
  "required": ["address"],
  "properties": {
    "address": {
      "type": "string",
      "description": "Memory address as an explicit 0x-prefixed hexadecimal string"
    },
    "size": {
      "type": "integer",
      "description": "Number of bytes to read (default 256, max 65536)",
      "minimum": 1,
      "maximum": 65536
    }
  }
})JSON";

constexpr const char* kSchemaMemoryWrite = R"JSON({
  "type": "object",
  "required": ["address", "data_hex"],
  "properties": {
    "address": {
      "type": "string",
      "description": "Memory address as an explicit 0x-prefixed hexadecimal string"
    },
    "data_hex": {
      "type": "string",
      "description": "Hex-encoded bytes to write; whitespace and 0x prefixes are ignored (e.g. '48 65 6C 6C')",
      "minLength": 2,
      "maxLength": 16384
    }
  }
})JSON";

constexpr const char* kSchemaWriteBytes = R"JSON({
  "type": "object",
  "required": ["address"],
  "anyOf": [
    { "required": ["hex_string"] },
    { "required": ["data_hex"] },
    { "required": ["hex"] }
  ],
  "properties": {
    "address": {
      "description": "Memory address as hex string or integer"
    },
    "hex_string": {
      "type": "string",
      "description": "Hex-encoded bytes to write; whitespace and 0x prefixes are ignored (e.g. '90 90 90')",
      "minLength": 2,
      "maxLength": 16384
    },
    "data_hex": {
      "type": "string",
      "description": "Alias for hex_string",
      "minLength": 2,
      "maxLength": 16384
    },
    "hex": {
      "type": "string",
      "description": "Alias for hex_string",
      "minLength": 2,
      "maxLength": 16384
    }
  }
})JSON";

constexpr const char* kSchemaScanStart = R"JSON({
  "type": "object",
  "required": ["mode"],
  "properties": {
    "mode": {
      "type": "string",
      "description": "exact, greater, less, between, or unknown"
    },
    "data_type": {
      "type": "string",
      "description": "byte, word, dword, qword, xor, float, or double"
    },
    "value": {
      "description": "Comparison value; required except for unknown or pattern scans"
    },
    "upper_value": {
      "description": "Upper comparison value required for between mode"
    },
    "pattern_hex": {
      "type": "string",
      "description": "Byte pattern alternative to scalar data_type/value",
      "minLength": 2,
      "maxLength": 16384
    },
    "memory_type": {
      "type": "string",
      "description": "all, anonymous, c_alloc, c_heap, c_data, c_bss, java_heap, java, stack, code_app, code_system, video, ashmem, bad, or other"
    },
    "start": {
      "type": "string",
      "description": "Optional explicit 0x-prefixed start address"
    },
    "end": {
      "type": "string",
      "description": "Optional explicit 0x-prefixed end address"
    }
  }
})JSON";

constexpr const char* kSchemaScanRefine = R"JSON({
  "type": "object",
  "required": ["scan_epoch", "mode", "data_type"],
  "properties": {
    "scan_epoch": {
      "type": "integer",
      "minimum": 0
    },
    "mode": {
      "type": "string",
      "description": "exact, greater, less, between, increased, increased_by, decreased, decreased_by, changed, or unchanged"
    },
    "data_type": {
      "type": "string",
      "description": "Must match the active scan session"
    },
    "value": {
      "description": "Comparison value for value-bearing modes"
    },
    "upper_value": {
      "description": "Upper comparison value required for between mode"
    }
  }
})JSON";

constexpr const char* kSchemaScanResults = R"JSON({
  "type": "object",
  "required": ["scan_epoch"],
  "properties": {
    "scan_epoch": {
      "type": "integer",
      "minimum": 0
    },
    "offset": {
      "type": "integer",
      "minimum": 0,
      "maximum": 5000000
    },
    "count": {
      "type": "integer",
      "minimum": 1,
      "maximum": 1000
    }
  }
})JSON";

constexpr const char* kSchemaScanClear = R"JSON({
  "type": "object",
  "required": ["scan_epoch"],
  "properties": {
    "scan_epoch": {
      "type": "integer",
      "minimum": 0
    }
  }
})JSON";

constexpr const char* kSchemaScanValue = R"JSON({
  "type": "object",
  "anyOf": [
    { "required": ["value"] },
    { "required": ["value_hex"] },
    { "required": ["hex"] }
  ],
  "properties": {
    "value": {
      "description": "Value to scan for; interpreted according to value_type/data_type. Required unless value_hex or hex is supplied"
    },
    "value2": {
      "description": "Upper bound for scan_type=between; interpreted according to value_type/data_type"
    },
    "max_value": {
      "description": "Alias for value2 when scan_type=between"
    },
    "upper_value": {
      "description": "Alias for value2 when scan_type=between"
    },
    "value_hex": {
      "type": "string",
      "description": "Little-endian encoded value bytes. Alternative to value",
      "minLength": 2,
      "maxLength": 16384
    },
    "value2_hex": {
      "type": "string",
      "description": "Upper bound for scan_type=between as little-endian encoded bytes",
      "minLength": 2,
      "maxLength": 16384
    },
    "hex": {
      "type": "string",
      "description": "Alias for value_hex. Alternative to value",
      "minLength": 2,
      "maxLength": 16384
    },
    "value_type": {
      "type": "string",
      "description": "One of: byte, word, dword, qword, xor, int32, int64, float, double, bytes, string"
    },
    "data_type": {
      "type": "string",
      "description": "MCP-style alias for value_type: byte, word, dword, qword, xor, float, double"
    },
    "scan_type": {
      "type": "string",
      "description": "MCP-style scan type: exact, greater, less, between"
    },
    "flags": {
      "type": "integer",
      "description": "Scan flags (provider-defined, default 0)",
      "minimum": 0
    },
    "scan_flag": {
      "type": "integer",
      "description": "IPC/MCP raw scan flag alias",
      "minimum": 0
    },
    "start": {
      "description": "Optional start address as hex string or integer"
    },
    "end": {
      "description": "Optional end address as hex string or integer"
    },
    "memory_type": {
      "type": "string",
      "description": "all, anonymous, c_alloc, c_heap, c_data, c_bss, java_heap, java, stack, code_app, code_system, video, ashmem, bad, other"
    },
    "memory_type_raw": {
      "type": "integer",
      "description": "Raw AMem MemoryType integer"
    },
    "type": {
      "type": "integer",
      "description": "Raw AMem MemoryType integer alias"
    }
  }
})JSON";

constexpr const char* kSchemaScanNext = R"JSON({
  "type": "object",
  "properties": {
    "value": {
      "description": "Value to filter by. Required for exact/greater/less/between/increased_by/decreased_by; optional for increased/decreased/changed/unchanged"
    },
    "value2": {
      "description": "Upper bound for scan_type=between; interpreted according to value_type/data_type"
    },
    "max_value": {
      "description": "Alias for value2 when scan_type=between"
    },
    "upper_value": {
      "description": "Alias for value2 when scan_type=between"
    },
    "value_hex": {
      "type": "string",
      "description": "Little-endian encoded value bytes. Alternative to value",
      "minLength": 2,
      "maxLength": 16384
    },
    "value2_hex": {
      "type": "string",
      "description": "Upper bound for scan_type=between as little-endian encoded bytes",
      "minLength": 2,
      "maxLength": 16384
    },
    "hex": {
      "type": "string",
      "description": "Alias for value_hex. Alternative to value",
      "minLength": 2,
      "maxLength": 16384
    },
    "value_type": {
      "type": "string",
      "description": "One of: byte, word, dword, qword, xor, int32, int64, float, double, bytes, string"
    },
    "data_type": {
      "type": "string",
      "description": "MCP-style alias for value_type: byte, word, dword, qword, xor, float, double"
    },
    "scan_type": {
      "type": "string",
      "description": "MCP-style scan type: exact, greater, less, between, increased, decreased, changed, unchanged, increased_by, decreased_by"
    },
    "flags": {
      "type": "integer",
      "description": "Scan flags (provider-defined, default 0)",
      "minimum": 0
    },
    "scan_flag": {
      "type": "integer",
      "description": "IPC/MCP raw scan flag alias",
      "minimum": 0
    },
    "start": {
      "description": "Optional start address as hex string or integer"
    },
    "end": {
      "description": "Optional end address as hex string or integer"
    }
  }
})JSON";

constexpr const char* kSchemaScanFuzzy = R"JSON({
  "type": "object",
  "properties": {
    "value_type": {
      "type": "string",
      "description": "One of: byte, word, dword, qword, xor, float, double"
    },
    "data_type": {
      "type": "string",
      "description": "MCP-style alias for value_type: byte, word, dword, qword, xor, float, double"
    },
    "scan_type": {
      "type": "string",
      "description": "MCP-style fuzzy scan type: unknown, increased, decreased, changed, unchanged. Default: unknown"
    },
    "flags": {
      "type": "integer",
      "description": "Raw AMem scan flags",
      "minimum": 0
    },
    "scan_flag": {
      "type": "integer",
      "description": "IPC/MCP raw scan flag alias",
      "minimum": 0
    },
    "start": {
      "description": "Optional start address as hex string or integer"
    },
    "end": {
      "description": "Optional end address as hex string or integer"
    },
    "memory_type": {
      "type": "string",
      "description": "all, anonymous, c_alloc, c_heap, c_data, c_bss, java_heap, java, stack, code_app, code_system, video, ashmem, bad, other"
    },
    "memory_type_raw": {
      "type": "integer",
      "description": "Raw AMem MemoryType integer"
    },
    "type": {
      "type": "integer",
      "description": "Raw AMem MemoryType integer alias"
    }
  }
})JSON";

constexpr const char* kSchemaGetScanResults = R"JSON({
  "type": "object",
  "properties": {
    "offset": {
      "type": "integer",
      "description": "Index of first result to return (default 0)",
      "minimum": 0
    },
    "count": {
      "type": "integer",
      "description": "Maximum number of results to return (default 100, max 1000)",
      "minimum": 1,
      "maximum": 1000
    }
  }
})JSON";

constexpr const char* kSchemaEmptyObject = R"JSON({
  "type": "object",
  "properties": {}
})JSON";

constexpr const char* kSchemaStatusInitDriver = R"JSON({
  "type": "object",
  "anyOf": [
    { "required": ["card_name"] },
    { "required": ["card"] }
  ],
  "properties": {
    "card_name": {
      "type": "string",
      "description": "Driver authorization card/key string",
      "minLength": 1,
      "maxLength": 4096
    },
    "card": {
      "type": "string",
      "description": "IPC/MCP alias for card_name",
      "minLength": 1,
      "maxLength": 4096
    }
  }
})JSON";

constexpr const char* kSchemaReadMemory = R"JSON({
  "type": "object",
  "required": ["address"],
  "properties": {
    "address": {
      "description": "Memory address as hex string or integer"
    },
    "size": {
      "type": "integer",
      "description": "Number of bytes to read (default 256, max 65536)",
      "minimum": 1,
      "maximum": 65536
    }
  }
})JSON";

constexpr const char* kSchemaMemoryReadValue = R"JSON({
  "type": "object",
  "required": ["address"],
  "properties": {
    "address": {
      "type": "string",
      "description": "Memory address as an explicit 0x-prefixed hexadecimal string"
    },
    "data_type": {
      "type": "string",
      "description": "byte, word, dword, qword, xor, float, or double"
    }
  }
})JSON";

constexpr const char* kSchemaReadValue = R"JSON({
  "type": "object",
  "required": ["address"],
  "properties": {
    "address": {
      "description": "Memory address as hex string or integer"
    },
    "data_type": {
      "type": "string",
      "description": "byte, word, dword, qword, xor, float, or double"
    }
  }
})JSON";

constexpr const char* kSchemaMemoryWriteValue = R"JSON({
  "type": "object",
  "required": ["address", "value"],
  "properties": {
    "address": {
      "type": "string",
      "description": "Memory address as an explicit 0x-prefixed hexadecimal string"
    },
    "value": {
      "description": "Scalar value to write; use a string for exact qword values"
    },
    "data_type": {
      "type": "string",
      "description": "byte, word, dword, qword, xor, float, or double"
    }
  }
})JSON";

constexpr const char* kSchemaWriteValue = R"JSON({
  "type": "object",
  "required": ["address", "value"],
  "properties": {
    "address": {
      "description": "Memory address as hex string or integer"
    },
    "value": {
      "description": "Scalar value to write"
    },
    "data_type": {
      "type": "string",
      "description": "byte, word, dword, qword, xor, float, or double"
    }
  }
})JSON";

constexpr const char* kSchemaScanSetRange = R"JSON({
  "type": "object",
  "properties": {
    "memory_type": {
      "type": "string",
      "description": "all, anonymous, c_alloc, c_heap, c_data, c_bss, java_heap, java, stack, code_app, code_system, video, ashmem, bad, other"
    },
    "type": {
      "type": "integer",
      "description": "Raw AMem MemoryType integer"
    },
    "memory_type_raw": {
      "type": "integer",
      "description": "Raw AMem MemoryType integer alias"
    }
  }
})JSON";

constexpr const char* kSchemaScanHex = R"JSON({
  "type": "object",
  "anyOf": [
    { "required": ["hex_pattern"] },
    { "required": ["pattern_hex"] },
    { "required": ["value"] }
  ],
  "properties": {
    "hex_pattern": {
      "type": "string",
      "description": "Hex byte pattern, e.g. '48 65 6C 6C 6F'",
      "minLength": 2,
      "maxLength": 16384
    },
    "pattern_hex": {
      "type": "string",
      "description": "IPC/MCP alias for hex_pattern",
      "minLength": 2,
      "maxLength": 16384
    },
    "value": {
      "type": "string",
      "description": "Legacy alias for hex_pattern",
      "minLength": 2,
      "maxLength": 16384
    },
    "start": {
      "description": "Optional start address as hex string or integer"
    },
    "end": {
      "description": "Optional end address as hex string or integer"
    },
    "memory_type": {
      "type": "string",
      "description": "all, anonymous, c_alloc, c_heap, c_data, c_bss, java_heap, java, stack, code_app, code_system, video, ashmem, bad, other"
    },
    "memory_type_raw": {
      "type": "integer",
      "description": "Raw AMem MemoryType integer"
    },
    "type": {
      "type": "integer",
      "description": "Raw AMem MemoryType integer alias"
    }
  }
})JSON";

constexpr const char* kSchemaListModules = R"JSON({
  "type": "object",
  "properties": {
    "filter": {
      "type": "string",
      "description": "Optional case-insensitive module-name substring",
      "maxLength": 4096
    },
    "offset": {
      "type": "integer",
      "minimum": 0
    },
    "count": {
      "type": "integer",
      "minimum": 1,
      "maximum": 1000
    }
  }
})JSON";

constexpr const char* kSchemaModuleResolve = R"JSON({
  "type": "object",
  "required": ["module_name"],
  "properties": {
    "module_name": {
      "type": "string",
      "description": "Exact module name, basename, or unique substring",
      "minLength": 1,
      "maxLength": 4096
    }
  }
})JSON";

constexpr const char* kSchemaGetModuleBase = R"JSON({
  "type": "object",
  "anyOf": [
    { "required": ["module_name"] },
    { "required": ["name"] }
  ],
  "properties": {
    "module_name": {
      "type": "string",
      "description": "Module name or unique substring",
      "minLength": 1,
      "maxLength": 4096
    },
    "name": {
      "type": "string",
      "description": "IPC/MCP alias for module_name",
      "minLength": 1,
      "maxLength": 4096
    }
  }
})JSON";

constexpr const char* kSchemaPointerResolve = R"JSON({
  "type": "object",
  "required": ["module_name", "base_offset"],
  "properties": {
    "module_name": {
      "type": "string",
      "description": "Exact module name, basename, or unique substring",
      "minLength": 1,
      "maxLength": 4096
    },
    "base_offset": {
      "type": "string",
      "description": "Explicit 0x-prefixed offset from the module base"
    },
    "offsets": {
      "type": "array",
      "description": "Pointer offsets as explicit 0x-prefixed strings",
      "items": { "type": "string" },
      "maxItems": 1024
    },
    "deref_final": {
      "type": "boolean",
      "description": "Dereference the address after applying all offsets (default true)"
    }
  }
})JSON";

constexpr const char* kSchemaResolveOffsetChain = R"JSON({
  "type": "object",
  "required": ["base_offset"],
  "anyOf": [
    { "required": ["module"] },
    { "required": ["module_name"] }
  ],
  "properties": {
    "module": {
      "type": "string",
      "description": "Module name or unique substring",
      "minLength": 1,
      "maxLength": 4096
    },
    "module_name": {
      "type": "string",
      "description": "Alias for module",
      "minLength": 1,
      "maxLength": 4096
    },
    "base_offset": {
      "description": "Base offset from module base as hex string or integer"
    },
    "offsets": {
      "type": "array",
      "description": "Pointer-chain offsets as integers or hex strings",
      "items": {
        "anyOf": [
          { "type": "integer" },
          { "type": "string" }
        ]
      },
      "maxItems": 1024
    },
    "deref_final": {
      "type": "boolean",
      "description": "Whether to dereference the final address (default true)"
    }
  }
})JSON";

constexpr const char* kSchemaDisassemble = R"JSON({
  "type": "object",
  "required": ["address"],
  "properties": {
    "address": {
      "type": "string",
      "pattern": "^0[xX][0-9A-Fa-f]+$",
      "description": "Explicit 0x-prefixed ARM64 instruction address"
    },
    "count": {
      "type": "integer",
      "description": "Number of fixed-width ARM64 instructions (default 16)",
      "minimum": 1,
      "maximum": 512
    }
  }
})JSON";

constexpr const char* kSchemaReadDisassembly = R"JSON({
  "type": "object",
  "required": ["address", "count"],
  "properties": {
    "address": {
      "description": "Starting address as hex string or integer"
    },
    "count": {
      "type": "integer",
      "description": "Number of ARM64 instructions to read (1-512)",
      "minimum": 1,
      "maximum": 512
    }
  }
})JSON";

constexpr const char* kSchemaBreakpointSet = R"JSON({
  "type": "object",
  "required": ["address"],
  "properties": {
    "address": {
      "type": "string",
      "pattern": "^0[xX][0-9A-Fa-f]+$",
      "description": "Explicit 0x-prefixed target address"
    },
    "access": {
      "type": "string",
      "enum": ["read", "write", "read_write", "execute"],
      "description": "Hardware breakpoint access type (default write)"
    },
    "size": {
      "type": "integer",
      "enum": [1, 2, 4, 8],
      "description": "Breakpoint width in bytes (default 4; execute requires 4)"
    }
  }
})JSON";

constexpr const char* kSchemaBreakpointAddress = R"JSON({
  "type": "object",
  "required": ["address"],
  "properties": {
    "address": {
      "type": "string",
      "pattern": "^0[xX][0-9A-Fa-f]+$",
      "description": "Explicit 0x-prefixed breakpoint address"
    }
  }
})JSON";

constexpr const char* kSchemaBreakpointHits = R"JSON({
  "type": "object",
  "required": ["address"],
  "properties": {
    "address": {
      "type": "string",
      "pattern": "^0[xX][0-9A-Fa-f]+$",
      "description": "Explicit 0x-prefixed breakpoint address"
    },
    "offset": {
      "type": "integer",
      "minimum": 0,
      "maximum": 100000
    },
    "count": {
      "type": "integer",
      "minimum": 1,
      "maximum": 100
    }
  }
})JSON";

constexpr const char* kSchemaSetBreakpoint = R"JSON({
  "type": "object",
  "required": ["address"],
  "properties": {
    "address": {
      "description": "Target address as hex string or integer"
    },
    "bp_type": {
      "type": "integer",
      "description": "Breakpoint type: 1=read, 2=write, 3=readwrite, 4=execute",
      "minimum": 1,
      "maximum": 4
    },
    "bp_size": {
      "type": "integer",
      "description": "Breakpoint width in bytes: 1, 2, 4, or 8. Execute breakpoints use 4",
      "minimum": 1,
      "maximum": 8
    }
  }
})JSON";

constexpr const char* kSchemaReadBreakpointInfo = R"JSON({
  "type": "object",
  "required": ["address"],
  "properties": {
    "address": {
      "description": "Breakpoint address as hex string or integer"
    }
  }
})JSON";

constexpr const char* kSchemaRemoveBreakpoint = R"JSON({
  "type": "object",
  "required": ["address"],
  "properties": {
    "address": {
      "description": "Breakpoint address as hex string or integer"
    }
  }
})JSON";

constexpr const char* kSchemaSymbolResolve = R"JSON({
  "type": "object",
  "required": ["module_name", "symbol_name"],
  "properties": {
    "module_name": {
      "type": "string",
      "description": "Exact module name, basename, or unique substring",
      "minLength": 1,
      "maxLength": 4096
    },
    "symbol_name": {
      "type": "string",
      "description": "Symbol name to resolve inside the module",
      "minLength": 1,
      "maxLength": 4096
    }
  }
})JSON";

constexpr const char* kSchemaResolveSymbol = R"JSON({
  "type": "object",
  "anyOf": [
    { "required": ["module_name", "symbol_name"] },
    { "required": ["module_name", "name"] },
    { "required": ["module", "symbol_name"] },
    { "required": ["module", "name"] }
  ],
  "properties": {
    "module_name": {
      "type": "string",
      "description": "Module name, e.g. 'libfoo.so'",
      "minLength": 1,
      "maxLength": 4096
    },
    "module": {
      "type": "string",
      "description": "Alias for module_name",
      "minLength": 1,
      "maxLength": 4096
    },
    "symbol_name": {
      "type": "string",
      "description": "Symbol (function / global) to resolve inside the module",
      "minLength": 1,
      "maxLength": 4096
    },
    "name": {
      "type": "string",
      "description": "Alias for symbol_name",
      "minLength": 1,
      "maxLength": 4096
    }
  }
})JSON";

constexpr const char* kSchemaSymbolInit = R"JSON({
  "type": "object",
  "required": ["module_base"],
  "properties": {
    "module_base": {
      "description": "Module base address as hex string or integer"
    }
  }
})JSON";

constexpr const char* kSchemaSymbolList = R"JSON({
  "type": "object",
  "required": ["module_name"],
  "properties": {
    "module_name": {
      "type": "string",
      "description": "Exact module name, basename, or unique substring",
      "minLength": 1,
      "maxLength": 4096
    },
    "symbol_epoch": {
      "type": "integer",
      "minimum": 0,
      "description": "Required with offset > 0; use the previous page's symbol_epoch"
    },
    "offset": {
      "type": "integer",
      "minimum": 0,
      "maximum": 2147483647
    },
    "count": {
      "type": "integer",
      "minimum": 1,
      "maximum": 1000
    }
  }
})JSON";

constexpr const char* kSchemaSymbolFind = R"JSON({
  "type": "object",
  "required": ["module_base"],
  "anyOf": [
    { "required": ["symbol_name"] },
    { "required": ["name"] }
  ],
  "properties": {
    "module_base": {
      "description": "Module base address as hex string or integer"
    },
    "symbol_name": {
      "type": "string",
      "minLength": 1,
      "maxLength": 4096
    },
    "name": {
      "type": "string",
      "description": "IPC/MCP alias for symbol_name",
      "minLength": 1,
      "maxLength": 4096
    }
  }
})JSON";

constexpr const char* kSchemaExecuteLua = R"JSON({
  "type": "object",
  "required": ["code"],
  "properties": {
    "code": {
      "type": "string",
      "description": "Lua code to execute inside AMem",
      "minLength": 1,
      "maxLength": 262144
    },
    "timeout_seconds": {
      "type": "integer",
      "description": "Execution timeout in seconds (default 30, max 300)",
      "minimum": 1,
      "maximum": 300
    }
  }
})JSON";

constexpr const char* kSchemaOpenProcess = R"JSON({
  "type": "object",
  "required": ["pid"],
  "properties": {
    "pid": {
      "type": "integer",
      "description": "Target process pid (positive integer) obtained from process_list",
      "minimum": 1
    },
    "name": {
      "type": "string",
      "description": "Optional process name; resolved automatically from the process list when omitted",
      "maxLength": 4096
    }
  }
})JSON";

constexpr const char* kSchemaProcessList = R"JSON({
  "type": "object",
  "properties": {
    "filter": {
      "type": "string",
      "description": "Optional case-insensitive process-name filter",
      "maxLength": 4096
    },
    "offset": {
      "type": "integer",
      "description": "Zero-based result offset",
      "minimum": 0
    },
    "count": {
      "type": "integer",
      "description": "Maximum results to return (default 200, max 1000)",
      "minimum": 1,
      "maximum": 1000
    }
  }
})JSON";

} // namespace

// ---------------------------------------------------------------------------
// ToolExecutor::initBuiltinTools
// ---------------------------------------------------------------------------
//
// This is the out-of-line definition for the declaration in ToolExecutor.h.
// Living here keeps ToolExecutor.cpp free of socket / tool-implementation
// dependencies (see note in ToolExecutor.cpp).

void ToolExecutor::initBuiltinTools() {
    registerTool(
        "status",
        "Get connection, server, architecture, feature, and attached-target status.",
        kSchemaEmptyObject,
        ToolSafety::ReadOnly,
        &execGetStatus,
        ToolTargetPolicy::None);

    registerTool(
        "get_status",
        "Compatibility alias for status.",
        kSchemaEmptyObject,
        ToolSafety::ReadOnly,
        &execGetStatus,
        ToolTargetPolicy::None,
        false);

    registerTool(
        "get_server_version",
        "Get the connected Android server version information.",
        kSchemaEmptyObject,
        ToolSafety::ReadOnly,
        &execGetServerVersion,
        ToolTargetPolicy::None,
        false);

    registerTool(
        "get_architecture",
        "Get the connected Android memory-driver architecture/type.",
        kSchemaEmptyObject,
        ToolSafety::ReadOnly,
        &execGetArchitecture,
        ToolTargetPolicy::None,
        false);

    registerTool(
        "init_driver",
        "Initialize the Android memory driver with an authorization card/key. Requires user confirmation.",
        kSchemaStatusInitDriver,
        ToolSafety::Write,
        &execInitDriver,
        true,
        ToolTargetPolicy::None);

    registerTool(
        "memory_read",
        "Read up to 65536 bytes from an explicit 0x-prefixed target address.",
        kSchemaMemoryRead,
        ToolSafety::ReadOnly,
        &execMemoryRead,
        ToolTargetPolicy::Bound);

    registerTool(
        "read_memory",
        "Read process memory and return compact hex plus spaced hex data.",
        kSchemaReadMemory,
        ToolSafety::ReadOnly,
        &execReadMemory,
        ToolTargetPolicy::Bound,
        false);

    registerTool(
        "memory_read_value",
        "Read one typed scalar from an explicit 0x-prefixed target address.",
        kSchemaMemoryReadValue,
        ToolSafety::ReadOnly,
        &execMemoryReadValue,
        ToolTargetPolicy::Bound);

    registerTool(
        "read_value",
        "Compatibility alias for memory_read_value.",
        kSchemaReadValue,
        ToolSafety::ReadOnly,
        &execReadValue,
        ToolTargetPolicy::Bound,
        false);

    registerTool(
        "memory_write",
        "Write up to 4096 bytes to an explicit 0x-prefixed target address. Requires user confirmation.",
        kSchemaMemoryWrite,
        ToolSafety::Write,
        &execMemoryWrite,
        ToolTargetPolicy::Bound);

    registerTool(
        "write_bytes",
        "Write raw bytes to target process memory. Requires user confirmation.",
        kSchemaWriteBytes,
        ToolSafety::Write,
        &execWriteBytes,
        ToolTargetPolicy::Bound,
        false);

    registerTool(
        "memory_write_value",
        "Write one typed scalar to an explicit 0x-prefixed target address. Requires user confirmation.",
        kSchemaMemoryWriteValue,
        ToolSafety::Write,
        &execMemoryWriteValue,
        ToolTargetPolicy::Bound);

    registerTool(
        "write_value",
        "Compatibility alias for memory_write_value.",
        kSchemaWriteValue,
        ToolSafety::Write,
        &execWriteValue,
        ToolTargetPolicy::Bound,
        false);

    registerTool(
        "scan_start",
        "Start a complete target-bound scan session. Requires user confirmation.",
        kSchemaScanStart,
        ToolSafety::Write,
        &execScanStart,
        ToolTargetPolicy::Bound);

    registerTool(
        "scan_refine",
        "Refine the expected scan session. Requires user confirmation.",
        kSchemaScanRefine,
        ToolSafety::Write,
        &execScanRefine,
        ToolTargetPolicy::Bound);

    registerTool(
        "scan_results",
        "Retrieve a page from the expected scan session.",
        kSchemaScanResults,
        ToolSafety::ReadOnly,
        &execScanResults,
        ToolTargetPolicy::Bound);

    registerTool(
        "scan_clear",
        "Clear the expected scan session. Requires user confirmation.",
        kSchemaScanClear,
        ToolSafety::Write,
        &execScanClear,
        ToolTargetPolicy::Bound);

    registerTool(
        "scan_set_range",
        "Compatibility alias retained for saved sessions.",
        kSchemaScanSetRange,
        ToolSafety::Write,
        &execScanSetRange,
        false,
        ToolTargetPolicy::Bound);

    registerTool(
        "scan_value",
        "Compatibility alias for scan_start.",
        kSchemaScanValue,
        ToolSafety::Write,
        &execScanValue,
        false,
        ToolTargetPolicy::Bound);

    registerTool(
        "scan_next",
        "Compatibility alias for scan_refine.",
        kSchemaScanNext,
        ToolSafety::Write,
        &execScanNext,
        false,
        ToolTargetPolicy::Bound);

    registerTool(
        "scan_fuzzy",
        "Compatibility alias for scan_start unknown mode.",
        kSchemaScanFuzzy,
        ToolSafety::Write,
        &execScanFuzzy,
        false,
        ToolTargetPolicy::Bound);

    registerTool(
        "scan_hex",
        "Compatibility alias for scan_start pattern_hex.",
        kSchemaScanHex,
        ToolSafety::Write,
        &execScanHex,
        false,
        ToolTargetPolicy::Bound);

    registerTool(
        "get_scan_count",
        "Compatibility alias for scan_results.",
        kSchemaEmptyObject,
        ToolSafety::ReadOnly,
        &execGetScanCount,
        false,
        ToolTargetPolicy::Bound);

    registerTool(
        "get_scan_results",
        "Compatibility alias for scan_results.",
        kSchemaGetScanResults,
        ToolSafety::ReadOnly,
        &execGetScanResults,
        false,
        ToolTargetPolicy::Bound);

    registerTool(
        "clear_scan",
        "Compatibility alias for scan_clear.",
        kSchemaEmptyObject,
        ToolSafety::Write,
        &execClearScan,
        false,
        ToolTargetPolicy::Bound);

    registerTool(
        "module_list",
        "List and page modules loaded in the attached target process.",
        kSchemaListModules,
        ToolSafety::ReadOnly,
        &execModuleList,
        ToolTargetPolicy::Bound);

    registerTool(
        "get_module_list",
        "Compatibility alias for module_list.",
        kSchemaListModules,
        ToolSafety::ReadOnly,
        &execGetModuleList,
        ToolTargetPolicy::Bound,
        false);

    registerTool(
        "list_modules",
        "Compatibility alias for module_list.",
        kSchemaListModules,
        ToolSafety::ReadOnly,
        &execListModules,
        ToolTargetPolicy::Bound,
        false);

    registerTool(
        "module_resolve",
        "Resolve an exact module name, basename, or unique substring.",
        kSchemaModuleResolve,
        ToolSafety::ReadOnly,
        &execModuleResolve,
        ToolTargetPolicy::Bound);

    registerTool(
        "get_module_base",
        "Compatibility alias for module_resolve.",
        kSchemaGetModuleBase,
        ToolSafety::ReadOnly,
        &execGetModuleBase,
        ToolTargetPolicy::Bound,
        false);

    registerTool(
        "process_list",
        "List and page processes available on the connected Android device.",
        kSchemaProcessList,
        ToolSafety::ReadOnly,
        &execListProcesses,
        ToolTargetPolicy::None);

    registerTool(
        "get_process_list",
        "Compatibility alias for process_list.",
        kSchemaEmptyObject,
        ToolSafety::ReadOnly,
        &execGetProcessList,
        ToolTargetPolicy::None,
        false);

    registerTool(
        "list_processes",
        "List processes available on the connected device.",
        kSchemaEmptyObject,
        ToolSafety::ReadOnly,
        &execListProcesses,
        ToolTargetPolicy::None,
        false);

    registerTool(
        "process_open",
        "Attach AMem to an observed process id. Requires user confirmation.",
        kSchemaOpenProcess,
        ToolSafety::Write,
        &execOpenProcess,
        ToolTargetPolicy::Selection);

    registerTool(
        "open_process",
        "Compatibility alias for process_open.",
        kSchemaOpenProcess,
        ToolSafety::Write,
        &execOpenProcess,
        ToolTargetPolicy::Selection,
        false);

    registerTool(
        "pointer_resolve",
        "Resolve a module-relative pointer chain in one target-bound read transaction.",
        kSchemaPointerResolve,
        ToolSafety::ReadOnly,
        &execPointerResolve,
        ToolTargetPolicy::Bound);

    registerTool(
        "resolve_offset_chain",
        "Compatibility alias for pointer_resolve.",
        kSchemaResolveOffsetChain,
        ToolSafety::ReadOnly,
        &execResolveOffsetChain,
        ToolTargetPolicy::Bound,
        false);

    registerTool(
        "disassemble",
        "Read bounded ARM64 instruction encodings from an explicit target address.",
        kSchemaDisassemble,
        ToolSafety::ReadOnly,
        &execDisassemble,
        ToolTargetPolicy::Bound);

    registerTool(
        "read_disassembly",
        "Compatibility alias for disassemble.",
        kSchemaReadDisassembly,
        ToolSafety::ReadOnly,
        &execReadDisassemblyLegacy,
        false,
        ToolTargetPolicy::Bound);

    registerTool(
        "breakpoint_set",
        "Set a target-bound hardware breakpoint with a confirmed mutation receipt.",
        kSchemaBreakpointSet,
        ToolSafety::Write,
        &execBreakpointSet,
        ToolTargetPolicy::Bound);

    registerTool(
        "breakpoint_remove",
        "Remove a target-bound hardware breakpoint with a confirmed mutation receipt.",
        kSchemaBreakpointAddress,
        ToolSafety::Write,
        &execBreakpointRemove,
        ToolTargetPolicy::Bound);

    registerTool(
        "breakpoint_hits",
        "Read a bounded page of hit and register data for a hardware breakpoint.",
        kSchemaBreakpointHits,
        ToolSafety::ReadOnly,
        &execBreakpointHits,
        ToolTargetPolicy::Bound);

    registerTool(
        "breakpoint_suspend",
        "Suspend a target-bound hardware breakpoint with a confirmed mutation receipt.",
        kSchemaBreakpointAddress,
        ToolSafety::Write,
        &execBreakpointSuspend,
        ToolTargetPolicy::Bound);

    registerTool(
        "breakpoint_resume",
        "Resume a target-bound hardware breakpoint with a confirmed mutation receipt.",
        kSchemaBreakpointAddress,
        ToolSafety::Write,
        &execBreakpointResume,
        ToolTargetPolicy::Bound);

    registerTool(
        "set_breakpoint",
        "Compatibility alias for breakpoint_set.",
        kSchemaSetBreakpoint,
        ToolSafety::Write,
        &execSetBreakpointLegacy,
        false,
        ToolTargetPolicy::Bound);

    registerTool(
        "remove_breakpoint",
        "Compatibility alias for breakpoint_remove.",
        kSchemaRemoveBreakpoint,
        ToolSafety::Write,
        &execRemoveBreakpointLegacy,
        false,
        ToolTargetPolicy::Bound);

    registerTool(
        "read_breakpoint_info",
        "Compatibility alias for breakpoint_hits.",
        kSchemaReadBreakpointInfo,
        ToolSafety::ReadOnly,
        &execReadBreakpointInfoLegacy,
        false,
        ToolTargetPolicy::Bound);

    registerTool(
        "suspend_breakpoint",
        "Compatibility alias for breakpoint_suspend.",
        kSchemaReadBreakpointInfo,
        ToolSafety::Write,
        &execSuspendBreakpointLegacy,
        false,
        ToolTargetPolicy::Bound);

    registerTool(
        "resume_breakpoint",
        "Compatibility alias for breakpoint_resume.",
        kSchemaReadBreakpointInfo,
        ToolSafety::Write,
        &execResumeBreakpointLegacy,
        false,
        ToolTargetPolicy::Bound);

    registerTool(
        "symbol_resolve",
        "Resolve a symbol by module name in one target-bound symbol transaction.",
        kSchemaSymbolResolve,
        ToolSafety::ReadOnly,
        &execSymbolResolve,
        ToolTargetPolicy::Bound);

    registerTool(
        "resolve_symbol",
        "Compatibility alias for symbol_resolve.",
        kSchemaResolveSymbol,
        ToolSafety::ReadOnly,
        &execResolveSymbolLegacy,
        false,
        ToolTargetPolicy::Bound);

    registerTool(
        "symbol_init",
        "Initialize the active symbol table for a module base address.",
        kSchemaSymbolInit,
        ToolSafety::ReadOnly,
        &execSymbolInit,
        false,
        ToolTargetPolicy::Bound);

    registerTool(
        "symbol_list",
        "List one page of symbols by module name in a target-bound symbol session.",
        kSchemaSymbolList,
        ToolSafety::ReadOnly,
        &execSymbolList,
        ToolTargetPolicy::Bound);

    registerTool(
        "symbol_find",
        "Find a symbol by name in a module base address.",
        kSchemaSymbolFind,
        ToolSafety::ReadOnly,
        &execSymbolFind,
        false,
        ToolTargetPolicy::Bound);

    registerTool(
        "execute_lua",
        "Execute Lua code inside AMem. Requires user confirmation.",
        kSchemaExecuteLua,
        ToolSafety::Write,
        &execExecuteLua,
        true,
        ToolTargetPolicy::Bound);
}

} // namespace AI

#endif // HAVE_AI_CHAT
