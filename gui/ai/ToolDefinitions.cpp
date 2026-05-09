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
#include "../../socket/client_singleton.h"
#include "../../third_party/nlohmann/json.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
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

// Encode a value (int/float/etc.) as a byte vector for scan_value. Supports
// the subset required by AC 5.3 and task 5.2: int32, int64, float, double,
// bytes, string. Throws std::runtime_error with a clear description on
// unsupported types or malformed values.
std::vector<unsigned char> encodeScanValue(const std::string& valueType, const json& value) {
    auto toLower = [](std::string s) {
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    const std::string t = toLower(valueType);

    auto asString = [&]() -> std::string {
        if (value.is_string()) return value.get<std::string>();
        if (value.is_number_integer()) return std::to_string(value.get<long long>());
        if (value.is_number_unsigned()) return std::to_string(value.get<unsigned long long>());
        if (value.is_number_float()) return std::to_string(value.get<double>());
        throw std::runtime_error("value must be string or number for type '" + t + "'");
    };

    std::vector<unsigned char> out;

    if (t == "int32") {
        const long long v = std::stoll(asString());
        if (v < INT32_MIN || v > INT32_MAX) throw std::runtime_error("int32 value out of range");
        int32_t iv = static_cast<int32_t>(v);
        out.resize(sizeof(iv));
        std::memcpy(out.data(), &iv, sizeof(iv));
    } else if (t == "int64") {
        const long long v = std::stoll(asString());
        int64_t iv = static_cast<int64_t>(v);
        out.resize(sizeof(iv));
        std::memcpy(out.data(), &iv, sizeof(iv));
    } else if (t == "float") {
        const float fv = std::stof(asString());
        out.resize(sizeof(fv));
        std::memcpy(out.data(), &fv, sizeof(fv));
    } else if (t == "double") {
        const double dv = std::stod(asString());
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
                                 "' (supported: int32, int64, float, double, bytes, string)");
    }
    return out;
}

// ---------------------------------------------------------------------------
// Individual tool executors
// ---------------------------------------------------------------------------

// memory_read
std::string execMemoryRead(const std::string& argsJson) {
    try {
        const json args = json::parse(argsJson.empty() ? std::string("{}") : argsJson);
        const uint64_t address = parseHexAddress(args.at("address").get<std::string>());
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

// memory_write
std::string execMemoryWrite(const std::string& argsJson) {
    try {
        const json args = json::parse(argsJson.empty() ? std::string("{}") : argsJson);
        const uint64_t address = parseHexAddress(args.at("address").get<std::string>());
        std::vector<unsigned char> bytes = parseHexBytes(args.at("data_hex").get<std::string>());
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

// scan_value
std::string execScanValue(const std::string& argsJson) {
    try {
        const json args = json::parse(argsJson.empty() ? std::string("{}") : argsJson);
        const std::string valueType = args.at("value_type").get<std::string>();
        std::vector<unsigned char> bytes = encodeScanValue(valueType, args.at("value"));

        uint32_t flags = 0;
        if (args.contains("flags") && args["flags"].is_number_integer()) {
            const long long f = args["flags"].get<long long>();
            if (f < 0 || f > UINT32_MAX) return makeError("flags out of range");
            flags = static_cast<uint32_t>(f);
        }

        const int count = ScanValue(flags, bytes, 0, UINT64_MAX, PORT_MAIN);
        if (count < 0) {
            return makeError("socket communication error: scan_value");
        }

        json result;
        result["result_count"] = count;
        return makeOk(result);
    } catch (const std::exception& e) {
        return makeError(std::string("scan_value: ") + e.what());
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

        json entries = json::array();
        for (const auto& kv : raw) {
            json entry;
            entry["address"] = toHexAddress(kv.first);
            entry["value"] = static_cast<uint64_t>(kv.second);
            entries.push_back(std::move(entry));
        }

        json result;
        result["total"] = total < 0 ? static_cast<int>(raw.size()) : total;
        result["offset"] = offset;
        result["results"] = std::move(entries);
        return makeOk(result);
    } catch (const std::exception& e) {
        return makeError(std::string("get_scan_results: ") + e.what());
    }
}

// get_module_list
std::string execGetModuleList(const std::string& /*argsJson*/) {
    try {
        std::vector<ModuleInfoItem> modules;
        if (!FetchModuleList(modules, PORT_MAIN)) {
            return makeError("socket communication error: get_module_list");
        }

        json arr = json::array();
        for (const auto& m : modules) {
            json item;
            item["name"] = m.name;
            item["base"] = toHexAddress(m.base);
            item["size"] = m.size;
            item["type"] = m.type;
            item["flag"] = m.flag;
            arr.push_back(std::move(item));
        }
        json result;
        result["modules"] = std::move(arr);
        return makeOk(result);
    } catch (const std::exception& e) {
        return makeError(std::string("get_module_list: ") + e.what());
    }
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
        const uint64_t address = parseHexAddress(args.at("address").get<std::string>());
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
            uint32_t insn = static_cast<uint32_t>(bytes[i]) |
                            (static_cast<uint32_t>(bytes[i + 1]) << 8) |
                            (static_cast<uint32_t>(bytes[i + 2]) << 16) |
                            (static_cast<uint32_t>(bytes[i + 3]) << 24);
            char hexBuf[16];
            std::snprintf(hexBuf, sizeof(hexBuf), "0x%08X", insn);
            json entry;
            entry["address"] = toHexAddress(address + static_cast<uint64_t>(i));
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
        const uint64_t address = parseHexAddress(args.at("address").get<std::string>());
        const int bpType = args.at("bp_type").get<int>();
        const int bpSize = args.at("bp_size").get<int>();
        if (bpType < 1 || bpType > 3) {
            return makeError("bp_type must be 1 (read), 2 (write), or 3 (execute)");
        }
        if (bpSize < 1 || bpSize > 8) {
            return makeError("bp_size must be between 1 and 8");
        }

        const bool ok = SetKernelBreakpoint(address,
                                            static_cast<uint32_t>(bpType),
                                            static_cast<uint32_t>(bpSize),
                                            PORT_MAIN);
        if (!ok) {
            return makeError("socket communication error: set_breakpoint");
        }

        json result;
        result["address"] = toHexAddress(address);
        result["bp_type"] = bpType;
        result["bp_size"] = bpSize;
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
        const uint64_t address = parseHexAddress(args.at("address").get<std::string>());

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

// resolve_symbol
std::string execResolveSymbol(const std::string& argsJson) {
    try {
        const json args = json::parse(argsJson.empty() ? std::string("{}") : argsJson);
        const std::string moduleName = args.at("module_name").get<std::string>();
        const std::string symbolName = args.at("symbol_name").get<std::string>();
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
  "required": ["address", "data_hex"],
  "properties": {
    "address": {
      "type": "string",
      "description": "Memory address in hex format, e.g. '0x7FF00000'"
    },
    "data_hex": {
      "type": "string",
      "description": "Hex-encoded bytes to write; whitespace and 0x prefixes are ignored (e.g. '48 65 6C 6C')",
      "minLength": 2,
      "maxLength": 16384
    }
  }
})JSON";

constexpr const char* kSchemaScanValue = R"JSON({
  "type": "object",
  "required": ["value", "value_type"],
  "properties": {
    "value": {
      "description": "Value to scan for; interpreted according to value_type"
    },
    "value_type": {
      "type": "string",
      "description": "One of: int32, int64, float, double, bytes, string"
    },
    "flags": {
      "type": "integer",
      "description": "Scan flags (provider-defined, default 0)",
      "minimum": 0
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

constexpr const char* kSchemaReadDisassembly = R"JSON({
  "type": "object",
  "required": ["address", "count"],
  "properties": {
    "address": {
      "type": "string",
      "description": "Starting address in hex format, e.g. '0x7FF00000'"
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
  "required": ["address", "bp_type", "bp_size"],
  "properties": {
    "address": {
      "type": "string",
      "description": "Target address in hex format"
    },
    "bp_type": {
      "type": "integer",
      "description": "Breakpoint type: 1=read, 2=write, 3=execute",
      "minimum": 1,
      "maximum": 3
    },
    "bp_size": {
      "type": "integer",
      "description": "Breakpoint width in bytes (1-8)",
      "minimum": 1,
      "maximum": 8
    }
  }
})JSON";

constexpr const char* kSchemaRemoveBreakpoint = R"JSON({
  "type": "object",
  "required": ["address"],
  "properties": {
    "address": {
      "type": "string",
      "description": "Address of the breakpoint to remove, in hex format"
    }
  }
})JSON";

constexpr const char* kSchemaResolveSymbol = R"JSON({
  "type": "object",
  "required": ["module_name", "symbol_name"],
  "properties": {
    "module_name": {
      "type": "string",
      "description": "Module name, e.g. 'libfoo.so'",
      "minLength": 1
    },
    "symbol_name": {
      "type": "string",
      "description": "Symbol (function / global) to resolve inside the module",
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
        "memory_read",
        "Read bytes from target process memory at the specified address.",
        kSchemaMemoryRead,
        ToolSafety::ReadOnly,
        &execMemoryRead);

    registerTool(
        "memory_write",
        "Write bytes to target process memory. Requires user confirmation.",
        kSchemaMemoryWrite,
        ToolSafety::Write,
        &execMemoryWrite);

    registerTool(
        "scan_value",
        "Scan the target process memory for a value. Returns the number of matches.",
        kSchemaScanValue,
        ToolSafety::ReadOnly,
        &execScanValue);

    registerTool(
        "get_scan_results",
        "Retrieve a page of results from the most recent scan.",
        kSchemaGetScanResults,
        ToolSafety::ReadOnly,
        &execGetScanResults);

    registerTool(
        "get_module_list",
        "List modules loaded in the currently attached target process.",
        kSchemaEmptyObject,
        ToolSafety::ReadOnly,
        &execGetModuleList);

    registerTool(
        "get_process_list",
        "List processes available on the connected device.",
        kSchemaEmptyObject,
        ToolSafety::ReadOnly,
        &execGetProcessList);

    registerTool(
        "open_process",
        "Attach AMem to a process by pid. Subsequent memory and scan tools "
        "operate against the attached target. Requires user confirmation.",
        kSchemaOpenProcess,
        ToolSafety::Write,
        &execOpenProcess);

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
        "resolve_symbol",
        "Resolve a symbol name to its absolute address inside a loaded module.",
        kSchemaResolveSymbol,
        ToolSafety::ReadOnly,
        &execResolveSymbol);
}

} // namespace AI

#endif // HAVE_AI_CHAT
