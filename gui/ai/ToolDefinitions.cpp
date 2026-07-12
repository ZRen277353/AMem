#ifdef HAVE_AI_CHAT

// ToolDefinitions.cpp
//
// Canonical tool registry. Memory-debugging tools delegate to
// AgentMemTools -> MemService; Lua remains behind its host feature boundary.
//
// Design notes:
//   * Schema validation is performed by ToolExecutor::execute() before the
//     lambda fires, but we still wrap all JSON access in try/catch and
//     defensively check bounds: the goal is a clear diagnostic for the AI
//     rather than a crash when inputs are unexpected.

#include "ToolExecutor.h"
#include "AgentMemTools.h"
#include "AgentRunContext.h"

#include "../../socket/socket_io_timeout.h"
#include "../../third_party/nlohmann/json.hpp"

#ifdef HAVE_LUAJIT
#include "../../lua/LuaEngine.h"
#include "../../mem/SystemMemService.h"
#endif

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <limits>
#include <string>

namespace AI {

namespace {

using nlohmann::json;

constexpr size_t kMaxToolLuaCodeBytes = 256 * 1024;
constexpr int kDefaultToolLuaTimeoutSeconds = 30;
constexpr int kMaxToolLuaTimeoutSeconds = 300;

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
// Lua argument helpers
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Individual tool executors
// ---------------------------------------------------------------------------

// status
std::string execGetStatus(const std::string& /*argsJson*/,
                          const Mem::OperationContext& context) {
    return getAgentMemTools().status("{}", context);
}

// driver_initialize
std::string execDriverInitialize(const std::string& argsJson,
                                 const Mem::OperationContext& context) {
    return getAgentMemTools().driverInitialize(argsJson, false, context);
}

// canonical memory_read
std::string execMemoryRead(const std::string& argsJson,
                           const Mem::OperationContext& context) {
    return getAgentMemTools().memoryRead(argsJson, false, context);
}

// canonical memory_read_value
std::string execMemoryReadValue(const std::string& argsJson,
                                const Mem::OperationContext& context) {
    return getAgentMemTools().memoryReadValue(argsJson, false, context);
}

// memory_write
std::string execMemoryWrite(const std::string& argsJson,
                            const Mem::OperationContext& context) {
    return getAgentMemTools().memoryWrite(argsJson, false, context);
}

// canonical memory_write_value
std::string execMemoryWriteValue(const std::string& argsJson,
                                 const Mem::OperationContext& context) {
    return getAgentMemTools().memoryWriteValue(argsJson, false, context);
}

// scan session
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

// canonical module_list
std::string execModuleList(const std::string& argsJson,
                           const Mem::OperationContext& context) {
    return getAgentMemTools().moduleList(argsJson, false, context);
}

// process_list
std::string execListProcesses(const std::string& argsJson,
                              const Mem::OperationContext& context) {
    return getAgentMemTools().processList(argsJson, context);
}

// process_open. MemService resolves the optional name, performs the target
// switch, and returns a new snapshot.
std::string execOpenProcess(const std::string& argsJson,
                            const Mem::OperationContext& context) {
    return getAgentMemTools().processOpen(argsJson, context);
}

// canonical module_resolve
std::string execModuleResolve(const std::string& argsJson,
                              const Mem::OperationContext& context) {
    return getAgentMemTools().moduleResolve(argsJson, false, context);
}

// canonical pointer_resolve
std::string execPointerResolve(const std::string& argsJson,
                               const Mem::OperationContext& context) {
    return getAgentMemTools().pointerResolve(argsJson, false, context);
}

std::string execDisassemble(
    const std::string& argsJson,
    const Mem::OperationContext& context) {
    return getAgentMemTools().disassemble(argsJson, context);
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

// lua_execute
std::string execLuaExecute(const std::string& argsJson,
                           const Mem::OperationContext& context) {
#ifdef HAVE_LUAJIT
    try {
        const json args = json::parse(argsJson.empty() ? std::string("{}") : argsJson);
        const std::string code = requiredStringArg(
            args, {"code"}, "code", kMaxToolLuaCodeBytes);
        const int timeoutSeconds = optionalIntArg(
            args, "timeout_seconds", kDefaultToolLuaTimeoutSeconds,
            1, kMaxToolLuaTimeoutSeconds);
        if (context.cancellation &&
            context.cancellation->load(std::memory_order_acquire)) {
            json cancelled;
            cancelled["success"] = false;
            cancelled["error"] = {
                {"code", "cancel_requested"},
                {"message", "Lua execution was cancelled before it started"},
                {"retryable", false},
            };
            cancelled["completion"] = "cancel_requested";
            return cancelled.dump();
        }

        const Mem::OperationContext current =
            Mem::getSystemMemService().captureContext(true);
        if (const auto contextError = validateAgentRunContext(
                context, current, ToolTargetPolicy::Bound)) {
            json rejected;
            rejected["success"] = false;
            rejected["error"] = {
                {"code", Mem::errorCodeName(contextError->code)},
                {"message", contextError->message},
                {"retryable", contextError->retryable},
            };
            rejected["completion"] = "rejected_before_start";
            return rejected.dump();
        }

        const auto requestedDeadline = std::chrono::steady_clock::now() +
            std::chrono::seconds(timeoutSeconds);
        const auto effectiveDeadline =
            (std::min)(requestedDeadline, context.deadline);
        if (std::chrono::steady_clock::now() >= effectiveDeadline) {
            json timedOut;
            timedOut["success"] = false;
            timedOut["error"] = {
                {"code", "timeout"},
                {"message", "Lua execution deadline expired before it started"},
                {"retryable", false},
            };
            timedOut["completion"] = "timed_out_before_start";
            return timedOut.dump();
        }

        SocketIoTimeout::ScopedTimeout luaTimeout(effectiveDeadline);
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
            const bool timedOut =
                engine.GetLastError() == "Lua execution timed out";
            err["success"] = false;
            err["error"] = {
                {"code", timedOut ? "timeout" : "internal_error"},
                {"message", engine.GetLastError()},
                {"retryable", false},
            };
            err["output"] = output;
            err["completion"] = timedOut ? "timed_out" : "completed";
            return err.dump();
        }
        json result;
        result["success"] = true;
        result["output"] = output;
        const bool cancelledAfterStart = context.cancellation &&
            context.cancellation->load(std::memory_order_acquire);
        const bool completedAfterDeadline =
            std::chrono::steady_clock::now() >= context.deadline;
        result["completed_after_cancel_request"] = cancelledAfterStart;
        result["completed_after_deadline"] = completedAfterDeadline;
        result["completion"] = cancelledAfterStart
            ? "completed_after_cancel_request"
            : (completedAfterDeadline
                   ? "completed_after_deadline"
                   : "completed");
        return makeOk(result);
    } catch (const std::exception& e) {
        return makeError(std::string("lua_execute: ") + e.what());
    }
#else
    (void)context;
    (void)argsJson;
    return makeError("lua_execute unavailable: AMem was built without LuaJIT");
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

constexpr const char* kSchemaEmptyObject = R"JSON({
  "type": "object",
  "properties": {}
})JSON";

constexpr const char* kSchemaDriverInitialize = R"JSON({
  "type": "object",
  "required": ["card"],
  "properties": {
    "card": {
      "type": "string",
      "description": "Driver authorization card/key string",
      "minLength": 1,
      "maxLength": 4096
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
    "count": {
      "type": "integer",
      "minimum": 1,
      "maximum": 100
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

constexpr const char* kSchemaLuaExecute = R"JSON({
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
        "driver_initialize",
        "Initialize the Android memory driver with an authorization card/key. Requires user confirmation.",
        kSchemaDriverInitialize,
        ToolSafety::Write,
        &execDriverInitialize,
        ToolTargetPolicy::None);

    registerTool(
        "memory_read",
        "Read up to 65536 bytes from an explicit 0x-prefixed target address.",
        kSchemaMemoryRead,
        ToolSafety::ReadOnly,
        &execMemoryRead,
        ToolTargetPolicy::Bound);

    registerTool(
        "memory_read_value",
        "Read one typed scalar from an explicit 0x-prefixed target address.",
        kSchemaMemoryReadValue,
        ToolSafety::ReadOnly,
        &execMemoryReadValue,
        ToolTargetPolicy::Bound);

    registerTool(
        "memory_write",
        "Write up to 4096 bytes to an explicit 0x-prefixed target address. Requires user confirmation.",
        kSchemaMemoryWrite,
        ToolSafety::Write,
        &execMemoryWrite,
        ToolTargetPolicy::Bound);

    registerTool(
        "memory_write_value",
        "Write one typed scalar to an explicit 0x-prefixed target address. Requires user confirmation.",
        kSchemaMemoryWriteValue,
        ToolSafety::Write,
        &execMemoryWriteValue,
        ToolTargetPolicy::Bound);

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
        "module_list",
        "List and page modules loaded in the attached target process.",
        kSchemaListModules,
        ToolSafety::ReadOnly,
        &execModuleList,
        ToolTargetPolicy::Bound);

    registerTool(
        "module_resolve",
        "Resolve an exact module name, basename, or unique substring.",
        kSchemaModuleResolve,
        ToolSafety::ReadOnly,
        &execModuleResolve,
        ToolTargetPolicy::Bound);

    registerTool(
        "process_list",
        "List and page processes available on the connected Android device.",
        kSchemaProcessList,
        ToolSafety::ReadOnly,
        &execListProcesses,
        ToolTargetPolicy::None);

    registerTool(
        "process_open",
        "Attach AMem to an observed process id. Requires user confirmation.",
        kSchemaOpenProcess,
        ToolSafety::Write,
        &execOpenProcess,
        ToolTargetPolicy::Selection);

    registerTool(
        "pointer_resolve",
        "Resolve a module-relative pointer chain in one target-bound read transaction.",
        kSchemaPointerResolve,
        ToolSafety::ReadOnly,
        &execPointerResolve,
        ToolTargetPolicy::Bound);

    registerTool(
        "disassemble",
        "Read bounded ARM64 instruction encodings from an explicit target address.",
        kSchemaDisassemble,
        ToolSafety::ReadOnly,
        &execDisassemble,
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
        "Read the newest bounded batch of hit and register data for a hardware breakpoint.",
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
        "symbol_resolve",
        "Resolve a symbol by module name in one target-bound symbol transaction.",
        kSchemaSymbolResolve,
        ToolSafety::ReadOnly,
        &execSymbolResolve,
        ToolTargetPolicy::Bound);

    registerTool(
        "symbol_list",
        "List one page of symbols by module name in a target-bound symbol session.",
        kSchemaSymbolList,
        ToolSafety::ReadOnly,
        &execSymbolList,
        ToolTargetPolicy::Bound);

#ifdef HAVE_LUAJIT
    registerTool(
        "lua_execute",
        "Execute Lua code inside AMem. Requires user confirmation; execution cannot be retracted after it starts.",
        kSchemaLuaExecute,
        ToolSafety::Write,
        &execLuaExecute,
        ToolTargetPolicy::Bound);

#endif
}

} // namespace AI

#endif // HAVE_AI_CHAT
