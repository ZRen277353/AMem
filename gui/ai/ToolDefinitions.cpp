#ifdef HAVE_AI_CHAT

// ToolDefinitions.cpp
//
// Concrete implementations of the 10 built-in tools exposed to the AI
// (Requirement 5.3). Each tool is registered with ToolExecutor during
// ToolExecutor::initBuiltinTools(), providing a JSON Schema for argument
// validation and an executor lambda that wraps the corresponding socket
// command from `socket/client_singleton.h`.
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

#include "../AppContext.h"
#include "../MemoryTypes.h"
#include "../../socket/client_singleton.h"
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
#include <string>
#include <vector>

namespace AI {

namespace {

using nlohmann::json;

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
    std::string trimmed = s;
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

std::string valueTypeFromArgs(const json& args, const std::string& fallback = "dword") {
    if (args.contains("value_type") && args["value_type"].is_string()) {
        return args["value_type"].get<std::string>();
    }
    return args.value("data_type", fallback);
}

int dataTypeToSize(const std::string& valueType) {
    const std::string t = lowerCopy(valueType);
    if (t == "byte" || t == "u8" || t == "uint8" || t == "int8") return 1;
    if (t == "word" || t == "u16" || t == "uint16" || t == "int16") return 2;
    if (t == "dword" || t == "int32" || t == "uint32" || t == "float") return 4;
    if (t == "qword" || t == "int64" || t == "uint64" || t == "double") return 8;
    throw std::runtime_error("unsupported data_type '" + valueType +
                             "' (supported: byte, word, dword, qword, float, double)");
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

uint32_t scanFlagsFromArgs(const json& args, const std::string& valueType) {
    uint32_t rawFlag = 0;
    if (readRawFlagArg(args, "flags", rawFlag) ||
        readRawFlagArg(args, "scan_flag", rawFlag)) {
        return rawFlag;
    }
    const std::string scanType =
        args.contains("scan_type") && args["scan_type"].is_string()
            ? args["scan_type"].get<std::string>()
            : std::string("exact");
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
    if (readRawFlagArg(args, "flags", rawFlag) ||
        readRawFlagArg(args, "scan_flag", rawFlag)) {
        return rawFlag;
    }
    const std::string scanType =
        args.contains("scan_type") && args["scan_type"].is_string()
            ? args["scan_type"].get<std::string>()
            : std::string("unknown");
    return static_cast<uint32_t>(scanTypeToFlag(scanType) | dataTypeToFlag(valueType));
}

// Encode a value (int/float/etc.) as a byte vector for scan/write tools.
// Supports both legacy value_type names (int32/int64/bytes/string) and
// MCP-style data_type names (byte/word/dword/qword/float/double).
std::vector<unsigned char> encodeScanValue(const std::string& valueType, const json& value) {
    const std::string t = lowerCopy(valueType);

    auto asString = [&]() -> std::string {
        if (value.is_string()) return value.get<std::string>();
        if (value.is_number_integer()) return std::to_string(value.get<long long>());
        if (value.is_number_unsigned()) return std::to_string(value.get<unsigned long long>());
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
    } else if (t == "dword" || t == "uint32") {
        out = integerToLittleEndian(parseIntegerBits(asString(), 32, valueType), 4);
    } else if (t == "int32") {
        out = integerToLittleEndian(parseIntegerBits(asString(), 32, valueType), 4);
    } else if (t == "qword" || t == "uint64") {
        out = integerToLittleEndian(parseIntegerBits(asString(), 64, valueType), 8);
    } else if (t == "int64") {
        out = integerToLittleEndian(parseIntegerBits(asString(), 64, valueType), 8);
    } else if (t == "float") {
        const float fv = parseFloatValueStrict(asString(), valueType);
        out.resize(sizeof(fv));
        std::memcpy(out.data(), &fv, sizeof(fv));
    } else if (t == "double") {
        const double dv = parseDoubleValueStrict(asString(), valueType);
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
                                 "' (supported: byte, word, dword, qword, int32, int64, float, double, bytes, string)");
    }
    return out;
}

std::vector<unsigned char> scanBytesFromArgs(const json& args,
                                             const std::string& valueType,
                                             const char* valueFieldName) {
    if (args.contains("value_hex") && args["value_hex"].is_string()) {
        std::vector<unsigned char> bytes = parseHexBytes(args["value_hex"].get<std::string>());
        if (bytes.empty()) throw std::runtime_error("value_hex is empty");
        return bytes;
    }
    if (args.contains("hex") && args["hex"].is_string()) {
        std::vector<unsigned char> bytes = parseHexBytes(args["hex"].get<std::string>());
        if (bytes.empty()) throw std::runtime_error("hex is empty");
        return bytes;
    }
    if (!args.contains(valueFieldName)) {
        throw std::runtime_error(std::string("missing required property '") +
                                 valueFieldName +
                                 "' (or provide value_hex/hex)");
    }
    return encodeScanValue(valueType, args.at(valueFieldName));
}

// ---------------------------------------------------------------------------
// Individual tool executors
// ---------------------------------------------------------------------------

// get_status
std::string execGetStatus(const std::string& /*argsJson*/) {
    try {
        auto& ctx = AppContext::Get();
        json result;
        result["connected"] = IsMultiPortConnected();
        result["pid"] = ctx.selectedPid.load();
        result["process_name"] = ctx.getSelectedName();
        result["handle"] = ctx.processHandle.load();
        return makeOk(result);
    } catch (const std::exception& e) {
        return makeError(std::string("get_status: ") + e.what());
    }
}

// get_server_version
std::string execGetServerVersion(const std::string& /*argsJson*/) {
    try {
        ServerVersionInfo info;
        if (!FetchServerVersion(info, PORT_MAIN)) {
            return makeError("socket communication error: get_server_version");
        }
        json result;
        result["version"] = info.version;
        result["version_string"] = info.versionString;
        return makeOk(result);
    } catch (const std::exception& e) {
        return makeError(std::string("get_server_version: ") + e.what());
    }
}

// get_architecture
std::string execGetArchitecture(const std::string& /*argsJson*/) {
    try {
        int type = 0;
        if (!GetMemType(type, PORT_MAIN)) {
            return makeError("socket communication error: get_architecture");
        }
        const char* names[] = {"Null", "IO", "Syscall", "Kernel", "SysHook"};
        json result;
        result["type"] = type;
        result["name"] = (type >= 0 && type <= 4) ? names[type] : "Unknown";
        return makeOk(result);
    } catch (const std::exception& e) {
        return makeError(std::string("get_architecture: ") + e.what());
    }
}

// init_driver
std::string execInitDriver(const std::string& argsJson) {
    try {
        const json args = json::parse(argsJson.empty() ? std::string("{}") : argsJson);
        std::string card = firstStringArg(args, {"card_name", "card"}, "card_name");
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

// memory_read
std::string execMemoryRead(const std::string& argsJson) {
    try {
        const json args = json::parse(argsJson.empty() ? std::string("{}") : argsJson);
        const uint64_t address = parseAddressJson(args.at("address"));
        const int sizeRaw = args.at("size").get<int>();
        if (sizeRaw <= 0 || sizeRaw > 4096) {
            return makeError("size must be between 1 and 4096");
        }
        const uint32_t size = static_cast<uint32_t>(sizeRaw);

        std::vector<unsigned char> bytes;
        if (!ReadProcessMemoryBytes(address, size, bytes, PORT_MAIN)) {
            return makeError("socket communication error: memory_read");
        }

        json result;
        result["address"] = toHexAddress(address);
        result["size"] = static_cast<int>(bytes.size());
        result["data"] = bytesToHex(bytes);
        return makeOk(result);
    } catch (const std::exception& e) {
        return makeError(std::string("memory_read: ") + e.what());
    }
}

// read_memory (MCP-compatible alias with larger read cap and compact hex)
std::string execReadMemory(const std::string& argsJson) {
    try {
        const json args = json::parse(argsJson.empty() ? std::string("{}") : argsJson);
        const uint64_t address = parseAddressJson(args.at("address"));
        int sizeRaw = args.value("size", 256);
        if (sizeRaw <= 0) {
            return makeError("size must be positive");
        }
        if (sizeRaw > 65536) {
            sizeRaw = 65536;
        }

        std::vector<unsigned char> bytes;
        if (!ReadProcessMemoryBytes(address, static_cast<uint32_t>(sizeRaw), bytes, PORT_MAIN)) {
            return makeError("socket communication error: read_memory");
        }

        json result;
        result["address"] = toHexAddress(address);
        result["size"] = static_cast<int>(bytes.size());
        result["hex"] = bytesToCompactHex(bytes);
        result["data"] = bytesToHex(bytes);
        return makeOk(result);
    } catch (const std::exception& e) {
        return makeError(std::string("read_memory: ") + e.what());
    }
}

// read_value
std::string execReadValue(const std::string& argsJson) {
    try {
        const json args = json::parse(argsJson.empty() ? std::string("{}") : argsJson);
        const uint64_t address = parseAddressJson(args.at("address"));
        const std::string dataType = args.value("data_type", std::string("dword"));
        const int size = dataTypeToSize(dataType);

        std::vector<unsigned char> bytes;
        if (!ReadProcessMemoryBytes(address, static_cast<uint32_t>(size), bytes, PORT_MAIN)) {
            return makeError("socket communication error: read_value");
        }
        if (static_cast<int>(bytes.size()) < size) {
            return makeError("read_value returned fewer bytes than requested");
        }

        json result;
        result["address"] = toHexAddress(address);
        result["data_type"] = dataType;
        result["hex"] = bytesToCompactHex(bytes);
        const std::string t = lowerCopy(dataType);
        if (t == "float") {
            float v = 0.0f;
            std::memcpy(&v, bytes.data(), sizeof(v));
            result["value"] = v;
        } else if (t == "double") {
            double v = 0.0;
            std::memcpy(&v, bytes.data(), sizeof(v));
            result["value"] = v;
        } else {
            uint64_t value = 0;
            for (int i = 0; i < size; ++i) {
                value |= static_cast<uint64_t>(bytes[i]) << (8 * i);
            }
            result["value"] = value;
            result["value_hex"] = toHexAddress(value);
        }
        return makeOk(result);
    } catch (const std::exception& e) {
        return makeError(std::string("read_value: ") + e.what());
    }
}

// memory_write
std::string execMemoryWrite(const std::string& argsJson) {
    try {
        const json args = json::parse(argsJson.empty() ? std::string("{}") : argsJson);
        const uint64_t address = parseAddressJson(args.at("address"));
        std::string hexBytes = firstStringArg(args, {"data_hex", "hex", "hex_string"}, "data_hex");
        std::vector<unsigned char> bytes = parseHexBytes(hexBytes);
        if (bytes.empty()) {
            return makeError("data_hex must contain at least one byte");
        }
        if (bytes.size() > 4096) {
            return makeError("data_hex exceeds maximum write size of 4096 bytes");
        }

        const uint32_t size = static_cast<uint32_t>(bytes.size());
        if (!WriteProcessMemoryBytes(address, size, bytes, PORT_MAIN)) {
            return makeError("socket communication error: memory_write");
        }

        json result;
        result["address"] = toHexAddress(address);
        result["written_bytes"] = static_cast<int>(size);
        return makeOk(result);
    } catch (const std::exception& e) {
        return makeError(std::string("memory_write: ") + e.what());
    }
}

// write_bytes
std::string execWriteBytes(const std::string& argsJson) {
    return execMemoryWrite(argsJson);
}

// write_value
std::string execWriteValue(const std::string& argsJson) {
    try {
        const json args = json::parse(argsJson.empty() ? std::string("{}") : argsJson);
        const uint64_t address = parseAddressJson(args.at("address"));
        const std::string dataType = valueTypeFromArgs(args);
        std::vector<unsigned char> bytes = encodeScanValue(dataType, args.at("value"));
        if (bytes.empty() || bytes.size() > 8) {
            return makeError("write_value supports scalar values up to 8 bytes");
        }

        const uint32_t size = static_cast<uint32_t>(bytes.size());
        if (!WriteProcessMemoryBytes(address, size, bytes, PORT_MAIN)) {
            return makeError("socket communication error: write_value");
        }

        json result;
        result["address"] = toHexAddress(address);
        result["written_bytes"] = static_cast<int>(size);
        result["hex"] = bytesToCompactHex(bytes);
        return makeOk(result);
    } catch (const std::exception& e) {
        return makeError(std::string("write_value: ") + e.what());
    }
}

// scan_value
std::string execScanValue(const std::string& argsJson) {
    try {
        const json args = json::parse(argsJson.empty() ? std::string("{}") : argsJson);
        const std::string valueType = valueTypeFromArgs(args);
        std::vector<unsigned char> bytes = scanBytesFromArgs(args, valueType, "value");

        const uint32_t flags = scanFlagsFromArgs(args, valueType);
        uint64_t start = 0;
        uint64_t end = UINT64_MAX;
        parseScanRange(args, start, end);
        const int memoryType = args.contains("memory_type_raw") && args["memory_type_raw"].is_number_integer()
                                   ? args["memory_type_raw"].get<int>()
                                   : memoryTypeToFlag(args.value("memory_type", std::string("all")));

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
        std::vector<unsigned char> bytes = scanBytesFromArgs(args, valueType, "value");
        uint32_t rawFlag = 0;
        const uint32_t flags = readRawFlagArg(args, "scan_flag", rawFlag)
                                   ? rawFlag
                                   : scanFlagsFromArgs(args, valueType);
        if (flags > static_cast<uint32_t>(INT32_MAX)) {
            return makeError("scan_flag out of range");
        }
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
        const int memoryType = args.contains("memory_type_raw") && args["memory_type_raw"].is_number_integer()
                                   ? args["memory_type_raw"].get<int>()
                                   : memoryTypeToFlag(args.value("memory_type", std::string("all")));

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
        const std::string pattern =
            firstStringArg(args, {"hex_pattern", "pattern_hex", "value"}, "hex_pattern");
        std::vector<unsigned char> bytes = parseHexBytes(pattern);
        if (bytes.empty()) {
            return makeError("hex_pattern must contain at least one byte");
        }
        uint64_t start = 0;
        uint64_t end = UINT64_MAX;
        parseScanRange(args, start, end);
        const int memoryType = args.contains("memory_type_raw") && args["memory_type_raw"].is_number_integer()
                                   ? args["memory_type_raw"].get<int>()
                                   : memoryTypeToFlag(args.value("memory_type", std::string("all")));

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
        int type = All;
        if (args.contains("type") && args["type"].is_number_integer()) {
            type = args["type"].get<int>();
        } else {
            type = memoryTypeToFlag(args.value("memory_type", std::string("all")));
        }
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
        int offset = 0;
        int count = 100;
        if (args.contains("offset") && args["offset"].is_number_integer()) {
            offset = args["offset"].get<int>();
            if (offset < 0) return makeError("offset must be >= 0");
        }
        if (args.contains("count") && args["count"].is_number_integer()) {
            count = args["count"].get<int>();
            if (count < 1 || count > 1000) return makeError("count must be between 1 and 1000");
        }

        std::vector<std::pair<uint64_t, uint64_t>> raw;
        if (!GetScanResult(offset, count, raw, PORT_MAIN)) {
            return makeError("socket communication error: get_scan_results");
        }
        const int total = GetScanResultCount(PORT_MAIN);
        if (total < 0) {
            return makeError("socket communication error: get_scan_count");
        }

        json entries = json::array();
        for (const auto& kv : raw) {
            json entry;
            entry["address"] = toHexAddress(kv.first);
            entry["value"] = static_cast<uint64_t>(kv.second);
            entries.push_back(std::move(entry));
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

// get_module_list
std::string execGetModuleList(const std::string& argsJson) {
    try {
        const json args = argsJson.empty() ? json::object() : json::parse(argsJson);
        std::vector<ModuleInfoItem> modules;
        if (!FetchModuleList(modules, PORT_MAIN)) {
            return makeError("socket communication error: get_module_list");
        }

        const std::string filter = lowerCopy(args.value("filter", std::string{}));
        std::vector<ModuleInfoItem> filtered;
        filtered.reserve(modules.size());
        for (const auto& m : modules) {
            if (!filter.empty()) {
                const std::string name = lowerCopy(m.name);
                if (name.find(filter) == std::string::npos) continue;
            }
            filtered.push_back(m);
        }

        int offset = args.value("offset", 0);
        int count = args.value("count", 1000);
        if (offset < 0) offset = 0;
        if (count < 1) count = 1;
        if (count > 1000) count = 1000;
        const int total = static_cast<int>(filtered.size());
        if (offset > total) offset = total;
        const int end = (std::min)(offset + count, total);

        json arr = json::array();
        for (int i = offset; i < end; ++i) {
            const auto& m = filtered[static_cast<size_t>(i)];
            json item;
            item["name"] = m.name;
            item["base"] = toHexAddress(m.base);
            item["size"] = m.size;
            item["type"] = m.type;
            item["flag"] = m.flag;
            arr.push_back(std::move(item));
        }
        json result;
        result["total"] = total;
        result["offset"] = offset;
        result["modules"] = std::move(arr);
        return makeOk(result);
    } catch (const std::exception& e) {
        return makeError(std::string("get_module_list: ") + e.what());
    }
}

// list_modules
std::string execListModules(const std::string& argsJson) {
    return execGetModuleList(argsJson);
}

// get_process_list
std::string execGetProcessList(const std::string& /*argsJson*/) {
    try {
        std::vector<ProcessInfoItem> procs;
        if (!FetchProcessList(procs, PORT_MAIN)) {
            return makeError("socket communication error: get_process_list");
        }

        json arr = json::array();
        for (const auto& p : procs) {
            json item;
            item["pid"] = p.pid;
            item["name"] = p.name;
            arr.push_back(std::move(item));
        }
        json result;
        result["processes"] = std::move(arr);
        return makeOk(result);
    } catch (const std::exception& e) {
        return makeError(std::string("get_process_list: ") + e.what());
    }
}

// list_processes
std::string execListProcesses(const std::string& argsJson) {
    return execGetProcessList(argsJson);
}

// open_process
//
// Attaches AMem to the given pid so that subsequent memory / scan / module
// tools operate against the new target. Matches the GUI's process-picker
// flow and the MCP `open_process` tool by going through
// AppContext::selectProcess(), which:
//   * stores the pid in the atomic,
//   * calls OpenProcessHandle (via SetCurrentPid + socket),
//   * invalidates the module cache so the next get_module_list is fresh.
// Classified as a Write tool so the user gets a confirmation prompt
// before we flip the global target — changing the attached process
// mid-conversation has wide blast radius for any other window relying on
// the previous pid.
std::string execOpenProcess(const std::string& argsJson) {
    try {
        const json args = json::parse(argsJson.empty() ? std::string("{}") : argsJson);
        const int pid = args.at("pid").get<int>();
        if (pid <= 0) {
            return makeError("pid must be a positive integer");
        }
        const std::string name = args.contains("name") && args["name"].is_string()
                                 ? args["name"].get<std::string>()
                                 : std::string{};

        // Resolve name if the caller didn't supply one so the process-bar
        // label shows something meaningful after attach. Best-effort only
        // — a lookup failure should not block the attach itself.
        std::string resolvedName = name;
        if (resolvedName.empty()) {
            std::vector<ProcessInfoItem> procs;
            if (FetchProcessList(procs, PORT_MAIN)) {
                for (const auto& p : procs) {
                    if (p.pid == pid) {
                        resolvedName = p.name;
                        break;
                    }
                }
            }
        }

        // selectProcess() handles SetCurrentPid + OpenProcessHandle and
        // updates AppContext for other windows. It does not return a
        // status, so we verify success by reading back the handle.
        AppContext::Get().selectProcess(pid, resolvedName);
        const int handle = AppContext::Get().processHandle.load();
        if (handle == 0) {
            return makeError("socket communication error: open_process (handle=0)");
        }

        json result;
        result["pid"] = pid;
        result["name"] = resolvedName;
        result["handle"] = handle;
        return makeOk(result);
    } catch (const std::exception& e) {
        return makeError(std::string("open_process: ") + e.what());
    }
}

// get_module_base
std::string execGetModuleBase(const std::string& argsJson) {
    try {
        const json args = json::parse(argsJson.empty() ? std::string("{}") : argsJson);
        const std::string moduleName =
            firstStringArg(args, {"module_name", "name"}, "module_name");
        uint64_t base = 0;
        if (!GetModuleBaseByName(moduleName, base, PORT_MAIN)) {
            return makeError("socket communication error: get_module_base");
        }
        json result;
        result["module"] = moduleName;
        result["base"] = toHexAddress(base);
        return makeOk(result);
    } catch (const std::exception& e) {
        return makeError(std::string("get_module_base: ") + e.what());
    }
}

// resolve_offset_chain
std::string execResolveOffsetChain(const std::string& argsJson) {
    try {
        const json args = json::parse(argsJson.empty() ? std::string("{}") : argsJson);
        const std::string moduleName =
            firstStringArg(args, {"module", "module_name"}, "module");
        const uint64_t baseOffset = parseAddressJson(args.at("base_offset"));
        std::vector<uint64_t> offsets;
        if (args.contains("offsets") && args["offsets"].is_array()) {
            for (const auto& off : args["offsets"]) {
                offsets.push_back(parseAddressJson(off));
            }
        }
        const bool derefFinal = args.value("deref_final", true);

        uint64_t address = 0;
        if (!ResolveModuleOffsetChain(address,
                                      moduleName,
                                      baseOffset,
                                      offsets,
                                      derefFinal,
                                      PORT_MAIN)) {
            return makeError("socket communication error: resolve_offset_chain");
        }
        json result;
        result["module"] = moduleName;
        result["address"] = toHexAddress(address);
        return makeOk(result);
    } catch (const std::exception& e) {
        return makeError(std::string("resolve_offset_chain: ") + e.what());
    }
}

// read_disassembly
//
// The AI chat module is compiled independently of HAVE_CAPSTONE, so rather
// than pulling in DisassemblyHelper we return the raw instruction bytes and
// note that full disassembly rendering is not yet wired through the tool
// interface. The AI still receives enough to reason about instruction
// encodings or to display them to the user.
std::string execReadDisassembly(const std::string& argsJson) {
    try {
        const json args = json::parse(argsJson.empty() ? std::string("{}") : argsJson);
        const uint64_t address = parseAddressJson(args.at("address"));
        const int countRaw = args.at("count").get<int>();
        if (countRaw < 1 || countRaw > 512) {
            return makeError("count must be between 1 and 512");
        }

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

// set_breakpoint
std::string execSetBreakpoint(const std::string& argsJson) {
    try {
        const json args = json::parse(argsJson.empty() ? std::string("{}") : argsJson);
        const uint64_t address = parseAddressJson(args.at("address"));
        const int bpType = args.value("bp_type", 2);
        const int bpSize = args.value("bp_size", 4);
        if (bpType < 1 || bpType > 4) {
            return makeError("bp_type must be 1 (read), 2 (write), 3 (readwrite), or 4 (execute)");
        }
        if (bpSize < 1 || bpSize > 8) {
            return makeError("bp_size must be between 1 and 8");
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

// remove_breakpoint
std::string execRemoveBreakpoint(const std::string& argsJson) {
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

// suspend_breakpoint
std::string execSuspendBreakpoint(const std::string& argsJson) {
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

// resume_breakpoint
std::string execResumeBreakpoint(const std::string& argsJson) {
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

// read_breakpoint_info
std::string execReadBreakpointInfo(const std::string& argsJson) {
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

// resolve_symbol
std::string execResolveSymbol(const std::string& argsJson) {
    try {
        const json args = json::parse(argsJson.empty() ? std::string("{}") : argsJson);
        const std::string moduleName =
            firstStringArg(args, {"module_name", "module"}, "module_name");
        const std::string symbolName =
            firstStringArg(args, {"symbol_name", "name"}, "symbol_name");
        if (moduleName.empty()) return makeError("module_name is empty");
        if (symbolName.empty()) return makeError("symbol_name is empty");

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

// symbol_list
std::string execSymbolList(const std::string& argsJson) {
    try {
        const json args = argsJson.empty() ? json::object() : json::parse(argsJson);
        if (args.contains("module_base") && !args["module_base"].is_null() &&
            !(args["module_base"].is_string() && args["module_base"].get<std::string>().empty())) {
            int totalCount = 0;
            const uint64_t moduleBase = parseAddressJson(args["module_base"]);
            if (!SymbolInit(moduleBase, totalCount, PORT_MAIN)) {
                return makeError("socket communication error: symbol_list/symbol_init");
            }
        }

        int offset = args.value("offset", 0);
        int count = args.value("count", 100);
        if (offset < 0) offset = 0;
        if (count < 1) count = 1;
        if (count > 1000) count = 1000;

        int totalCount = 0;
        std::vector<std::pair<uint64_t, std::string>> symbols;
        if (!SymbolGetList(offset, count, symbols, &totalCount, PORT_MAIN)) {
            return makeError("socket communication error: symbol_list");
        }
        json arr = json::array();
        for (const auto& [address, name] : symbols) {
            arr.push_back({{"address", toHexAddress(address)}, {"name", name}});
        }
        json result;
        result["total"] = totalCount;
        result["offset"] = offset;
        result["symbols"] = std::move(arr);
        return makeOk(result);
    } catch (const std::exception& e) {
        return makeError(std::string("symbol_list: ") + e.what());
    }
}

// symbol_find
std::string execSymbolFind(const std::string& argsJson) {
    try {
        const json args = json::parse(argsJson.empty() ? std::string("{}") : argsJson);
        const uint64_t moduleBase = parseAddressJson(args.at("module_base"));
        const std::string name = firstStringArg(args, {"symbol_name", "name"}, "symbol_name");
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
        const std::string code = args.at("code").get<std::string>();
        auto& engine = LuaEngine::GetInstance();
        if (!engine.IsInitialized() && !engine.Initialize()) {
            return makeError("Lua engine initialization failed: " + engine.GetLastError());
        }
        std::string output;
        if (!engine.ExecuteStringCapture(code, "ai_tool", output)) {
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
  "required": ["address", "size"],
  "properties": {
    "address": {
      "type": "string",
      "description": "Memory address in hex format, e.g. '0x7FF00000'"
    },
    "size": {
      "type": "integer",
      "description": "Number of bytes to read (1-4096)",
      "minimum": 1,
      "maximum": 4096
    }
  }
})JSON";

constexpr const char* kSchemaMemoryWrite = R"JSON({
  "type": "object",
  "required": ["address"],
  "anyOf": [
    { "required": ["data_hex"] },
    { "required": ["hex_string"] },
    { "required": ["hex"] }
  ],
  "properties": {
    "address": {
      "description": "Memory address as hex string or integer"
    },
    "data_hex": {
      "type": "string",
      "description": "Hex-encoded bytes to write; whitespace and 0x prefixes are ignored (e.g. '48 65 6C 6C')",
      "minLength": 2,
      "maxLength": 16384
    },
    "hex_string": {
      "type": "string",
      "description": "Alias for data_hex",
      "minLength": 2,
      "maxLength": 16384
    },
    "hex": {
      "type": "string",
      "description": "Alias for data_hex",
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
    "value_hex": {
      "type": "string",
      "description": "Little-endian encoded value bytes. Alternative to value"
    },
    "hex": {
      "type": "string",
      "description": "Alias for value_hex. Alternative to value"
    },
    "value_type": {
      "type": "string",
      "description": "One of: byte, word, dword, qword, int32, int64, float, double, bytes, string"
    },
    "data_type": {
      "type": "string",
      "description": "MCP-style alias for value_type: byte, word, dword, qword, float, double"
    },
    "scan_type": {
      "type": "string",
      "description": "MCP-style scan type: exact, unknown, greater, less, between, increased, decreased, changed, unchanged"
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
      "description": "One of: int32, int64, float, double, bytes, string"
    },
    "data_type": {
      "type": "string",
      "description": "MCP-style alias for value_type: byte, word, dword, qword, float, double"
    },
    "scan_type": {
      "type": "string",
      "description": "MCP-style fuzzy scan type: unknown, increased, increased_by, decreased, decreased_by, changed, unchanged. Default: unknown"
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
      "minLength": 1
    },
    "card": {
      "type": "string",
      "description": "IPC/MCP alias for card_name",
      "minLength": 1
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

constexpr const char* kSchemaReadValue = R"JSON({
  "type": "object",
  "required": ["address"],
  "properties": {
    "address": {
      "description": "Memory address as hex string or integer"
    },
    "data_type": {
      "type": "string",
      "description": "byte, word, dword, qword, float, or double"
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
      "description": "byte, word, dword, qword, float, or double"
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
      "minLength": 2
    },
    "pattern_hex": {
      "type": "string",
      "description": "IPC/MCP alias for hex_pattern",
      "minLength": 2
    },
    "value": {
      "type": "string",
      "description": "Legacy alias for hex_pattern",
      "minLength": 2
    },
    "start": {
      "description": "Optional start address as hex string or integer"
    },
    "end": {
      "description": "Optional end address as hex string or integer"
    }
  }
})JSON";

constexpr const char* kSchemaListModules = R"JSON({
  "type": "object",
  "properties": {
    "filter": {
      "type": "string",
      "description": "Optional case-insensitive module-name substring"
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
      "minLength": 1
    },
    "name": {
      "type": "string",
      "description": "IPC/MCP alias for module_name",
      "minLength": 1
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
      "minLength": 1
    },
    "module_name": {
      "type": "string",
      "description": "Alias for module",
      "minLength": 1
    },
    "base_offset": {
      "description": "Base offset from module base as hex string or integer"
    },
    "offsets": {
      "type": "array",
      "description": "Pointer-chain offsets as integers or hex strings"
    },
    "deref_final": {
      "type": "boolean",
      "description": "Whether to dereference the final address (default true)"
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
      "description": "Breakpoint width in bytes (1-8)",
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
      "minLength": 1
    },
    "module": {
      "type": "string",
      "description": "Alias for module_name",
      "minLength": 1
    },
    "symbol_name": {
      "type": "string",
      "description": "Symbol (function / global) to resolve inside the module",
      "minLength": 1
    },
    "name": {
      "type": "string",
      "description": "Alias for symbol_name",
      "minLength": 1
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
  "properties": {
    "module_base": {
      "description": "Optional module base address; initializes symbols for that module first"
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
      "minLength": 1
    },
    "name": {
      "type": "string",
      "description": "IPC/MCP alias for symbol_name",
      "minLength": 1
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
      "minLength": 1
    }
  }
})JSON";

constexpr const char* kSchemaOpenProcess = R"JSON({
  "type": "object",
  "required": ["pid"],
  "properties": {
    "pid": {
      "type": "integer",
      "description": "Target process pid (positive integer) obtained from get_process_list",
      "minimum": 1
    },
    "name": {
      "type": "string",
      "description": "Optional process name; resolved automatically from the process list when omitted"
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
        "get_status",
        "Get AMem connection and currently attached process status.",
        kSchemaEmptyObject,
        ToolSafety::ReadOnly,
        &execGetStatus);

    registerTool(
        "get_server_version",
        "Get the connected Android server version information.",
        kSchemaEmptyObject,
        ToolSafety::ReadOnly,
        &execGetServerVersion);

    registerTool(
        "get_architecture",
        "Get the connected Android memory-driver architecture/type.",
        kSchemaEmptyObject,
        ToolSafety::ReadOnly,
        &execGetArchitecture);

    registerTool(
        "init_driver",
        "Initialize the Android memory driver with an authorization card/key. Requires user confirmation.",
        kSchemaStatusInitDriver,
        ToolSafety::Write,
        &execInitDriver);

    registerTool(
        "memory_read",
        "Read bytes from target process memory at the specified address.",
        kSchemaMemoryRead,
        ToolSafety::ReadOnly,
        &execMemoryRead);

    registerTool(
        "read_memory",
        "Read process memory and return compact hex plus spaced hex data.",
        kSchemaReadMemory,
        ToolSafety::ReadOnly,
        &execReadMemory);

    registerTool(
        "read_value",
        "Read a single typed scalar value from process memory.",
        kSchemaReadValue,
        ToolSafety::ReadOnly,
        &execReadValue);

    registerTool(
        "memory_write",
        "Write bytes to target process memory. Requires user confirmation.",
        kSchemaMemoryWrite,
        ToolSafety::Write,
        &execMemoryWrite);

    registerTool(
        "write_bytes",
        "Write raw bytes to target process memory. Requires user confirmation.",
        kSchemaWriteBytes,
        ToolSafety::Write,
        &execWriteBytes);

    registerTool(
        "write_value",
        "Write a single typed scalar value to process memory. Requires user confirmation.",
        kSchemaWriteValue,
        ToolSafety::Write,
        &execWriteValue);

    registerTool(
        "scan_set_range",
        "Set the memory range used by subsequent scans. Requires user confirmation.",
        kSchemaScanSetRange,
        ToolSafety::Write,
        &execScanSetRange);

    registerTool(
        "scan_value",
        "Scan target process memory for a value. Supports AMem flags or MCP-style data_type/scan_type.",
        kSchemaScanValue,
        ToolSafety::ReadOnly,
        &execScanValue);

    registerTool(
        "scan_next",
        "Filter the previous scan results with a new value/condition.",
        kSchemaScanValue,
        ToolSafety::ReadOnly,
        &execScanNext);

    registerTool(
        "scan_fuzzy",
        "Run a fuzzy scan such as unknown/increased/decreased/changed/unchanged.",
        kSchemaScanFuzzy,
        ToolSafety::ReadOnly,
        &execScanFuzzy);

    registerTool(
        "scan_hex",
        "Scan memory for a hex byte pattern.",
        kSchemaScanHex,
        ToolSafety::ReadOnly,
        &execScanHex);

    registerTool(
        "get_scan_count",
        "Get the current scan result count.",
        kSchemaEmptyObject,
        ToolSafety::ReadOnly,
        &execGetScanCount);

    registerTool(
        "get_scan_results",
        "Retrieve a page of results from the most recent scan.",
        kSchemaGetScanResults,
        ToolSafety::ReadOnly,
        &execGetScanResults);

    registerTool(
        "clear_scan",
        "Clear all scan results. Requires user confirmation.",
        kSchemaEmptyObject,
        ToolSafety::Write,
        &execClearScan);

    registerTool(
        "get_module_list",
        "List modules loaded in the currently attached target process.",
        kSchemaListModules,
        ToolSafety::ReadOnly,
        &execGetModuleList);

    registerTool(
        "list_modules",
        "List modules loaded in the currently attached target process.",
        kSchemaListModules,
        ToolSafety::ReadOnly,
        &execListModules);

    registerTool(
        "get_module_base",
        "Resolve a module name/substr to its base address.",
        kSchemaGetModuleBase,
        ToolSafety::ReadOnly,
        &execGetModuleBase);

    registerTool(
        "get_process_list",
        "List processes available on the connected device.",
        kSchemaEmptyObject,
        ToolSafety::ReadOnly,
        &execGetProcessList);

    registerTool(
        "list_processes",
        "List processes available on the connected device.",
        kSchemaEmptyObject,
        ToolSafety::ReadOnly,
        &execListProcesses);

    registerTool(
        "open_process",
        "Attach AMem to a process by pid. Subsequent memory and scan tools "
        "operate against the attached target. Requires user confirmation.",
        kSchemaOpenProcess,
        ToolSafety::Write,
        &execOpenProcess);

    registerTool(
        "resolve_offset_chain",
        "Resolve a module-relative pointer chain to a final address.",
        kSchemaResolveOffsetChain,
        ToolSafety::ReadOnly,
        &execResolveOffsetChain);

    registerTool(
        "read_disassembly",
        "Read a range of ARM64 instructions as raw 32-bit encodings plus hex bytes.",
        kSchemaReadDisassembly,
        ToolSafety::ReadOnly,
        &execReadDisassembly);

    registerTool(
        "set_breakpoint",
        "Set a hardware breakpoint at the given address. Requires user confirmation.",
        kSchemaSetBreakpoint,
        ToolSafety::Write,
        &execSetBreakpoint);

    registerTool(
        "remove_breakpoint",
        "Remove a hardware breakpoint at the given address. Requires user confirmation.",
        kSchemaRemoveBreakpoint,
        ToolSafety::Write,
        &execRemoveBreakpoint);

    registerTool(
        "read_breakpoint_info",
        "Read hit/register information for a hardware breakpoint.",
        kSchemaReadBreakpointInfo,
        ToolSafety::ReadOnly,
        &execReadBreakpointInfo);

    registerTool(
        "suspend_breakpoint",
        "Suspend a hardware breakpoint without removing it. Requires user confirmation.",
        kSchemaReadBreakpointInfo,
        ToolSafety::Write,
        &execSuspendBreakpoint);

    registerTool(
        "resume_breakpoint",
        "Resume a suspended hardware breakpoint. Requires user confirmation.",
        kSchemaReadBreakpointInfo,
        ToolSafety::Write,
        &execResumeBreakpoint);

    registerTool(
        "resolve_symbol",
        "Resolve a symbol name to its absolute address inside a loaded module.",
        kSchemaResolveSymbol,
        ToolSafety::ReadOnly,
        &execResolveSymbol);

    registerTool(
        "symbol_init",
        "Initialize the symbol table for a module base address.",
        kSchemaSymbolInit,
        ToolSafety::ReadOnly,
        &execSymbolInit);

    registerTool(
        "symbol_list",
        "List symbols from the initialized symbol table, optionally initializing a module first.",
        kSchemaSymbolList,
        ToolSafety::ReadOnly,
        &execSymbolList);

    registerTool(
        "symbol_find",
        "Find a symbol by name in a module base address.",
        kSchemaSymbolFind,
        ToolSafety::ReadOnly,
        &execSymbolFind);

    registerTool(
        "execute_lua",
        "Execute Lua code inside AMem. Requires user confirmation.",
        kSchemaExecuteLua,
        ToolSafety::Write,
        &execExecuteLua);
}

} // namespace AI

#endif // HAVE_AI_CHAT
