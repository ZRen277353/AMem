#include "AgentMemTools.h"

#include "../../mem/Address.h"
#include "../../mem/ValueCodec.h"
#include "../../third_party/nlohmann/json.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace AI {

namespace {

using nlohmann::json;

json resultMeta(uint64_t durationMs,
                uint64_t connectionGeneration,
                const Mem::TargetSnapshot* target = nullptr) {
    json meta;
    meta["duration_ms"] = durationMs;
    meta["connection_generation"] = connectionGeneration;
    if (target) {
        meta["process_revision"] = target->processRevision;
        meta["pid"] = target->pid;
    }
    return meta;
}

std::string errorResult(const Mem::Error& error, uint64_t durationMs) {
    json output;
    output["success"] = false;
    output["error"] = {
        {"code", Mem::errorCodeName(error.code)},
        {"message", error.message},
        {"retryable", error.retryable},
    };
    output["meta"] = {{"duration_ms", durationMs}};
    return output.dump();
}

std::string exceptionResult(const char* operation, const std::exception& error) {
    return errorResult(
        Mem::Error{Mem::ErrorCode::InvalidArgument,
                   std::string(operation) + ": " + error.what(), false},
        0);
}

int requiredPositiveInt(const json& args, const char* key) {
    if (!args.contains(key) || !args[key].is_number_integer()) {
        throw std::runtime_error(std::string(key) + " must be an integer");
    }
    const long long value = args[key].get<long long>();
    if (value <= 0 || value > (std::numeric_limits<int>::max)()) {
        throw std::runtime_error(std::string(key) +
                                 " must be a positive 32-bit integer");
    }
    return static_cast<int>(value);
}

size_t optionalSize(const json& args,
                    const char* key,
                    size_t fallback,
                    size_t maximum) {
    if (!args.contains(key) || args[key].is_null()) {
        return fallback;
    }
    if (!args[key].is_number_unsigned() && !args[key].is_number_integer()) {
        throw std::runtime_error(std::string(key) + " must be an integer");
    }
    const long long value = args[key].get<long long>();
    if (value < 0 || static_cast<unsigned long long>(value) > maximum) {
        throw std::runtime_error(std::string(key) + " is outside the allowed range");
    }
    return static_cast<size_t>(value);
}

bool optionalBool(const json& args, const char* key, bool fallback) {
    if (!args.contains(key) || args.at(key).is_null()) {
        return fallback;
    }
    if (!args.at(key).is_boolean()) {
        throw std::runtime_error(std::string(key) + " must be a boolean");
    }
    return args.at(key).get<bool>();
}

std::string optionalString(const json& args, const char* key) {
    if (!args.contains(key) || args[key].is_null()) {
        return {};
    }
    if (!args[key].is_string()) {
        throw std::runtime_error(std::string(key) + " must be a string");
    }
    const std::string value = args[key].get<std::string>();
    if (value.size() > Mem::kMaxTextParameterBytes) {
        throw std::runtime_error(std::string(key) + " exceeds 4096 bytes");
    }
    return value;
}

std::string scalarValueText(const json& args) {
    if (!args.contains("value")) {
        throw std::runtime_error("value is required");
    }
    const json& value = args.at("value");
    std::string text;
    if (value.is_string()) {
        text = value.get<std::string>();
    } else if (value.is_number_unsigned()) {
        text = std::to_string(value.get<unsigned long long>());
    } else if (value.is_number_integer()) {
        text = std::to_string(value.get<long long>());
    } else if (value.is_number_float()) {
        text = value.dump();
    } else {
        throw std::runtime_error("value must be a string or number");
    }
    if (text.size() > Mem::kMaxScalarValueTextBytes) {
        throw std::runtime_error("value exceeds 256 bytes");
    }
    return text;
}

Mem::Result<Mem::ScalarType> scalarTypeArgument(
    const json& args,
    bool allowLegacyArguments) {
    std::string type;
    if (allowLegacyArguments && args.contains("value_type") &&
        !args.at("value_type").is_null()) {
        type = optionalString(args, "value_type");
    } else {
        type = optionalString(args, "data_type");
    }
    if (type.empty()) {
        type = "dword";
    }
    return Mem::parseScalarType(type);
}

std::string moduleNameArgument(const json& args,
                               bool allowLegacyArguments) {
    const char* key = "module_name";
    if (allowLegacyArguments && !args.contains(key) &&
        args.contains("name")) {
        key = "name";
    }
    const std::string name = optionalString(args, key);
    if (name.empty()) {
        throw std::runtime_error(
            allowLegacyArguments
                ? "module_name or name must be a non-empty string"
                : "module_name must be a non-empty string");
    }
    return name;
}

Mem::Result<uint64_t> parseAddressArgument(const json& value,
                                           bool allowLegacyAddress) {
    if (value.is_string()) {
        return Mem::parseAddress(value.get<std::string>(), !allowLegacyAddress);
    }
    if (allowLegacyAddress && value.is_number_unsigned()) {
        return Mem::Result<uint64_t>::success(value.get<uint64_t>());
    }
    if (allowLegacyAddress && value.is_number_integer()) {
        const long long address = value.get<long long>();
        if (address >= 0) {
            return Mem::Result<uint64_t>::success(
                static_cast<uint64_t>(address));
        }
    }
    return Mem::Result<uint64_t>::failure(
        Mem::ErrorCode::InvalidArgument,
        allowLegacyAddress
            ? "address must be a non-negative integer or hexadecimal string"
            : "address must be an explicit 0x-prefixed hexadecimal string");
}

std::string pointerModuleNameArgument(const json& args,
                                      bool allowLegacyArguments) {
    const char* key = "module_name";
    if (allowLegacyArguments && !args.contains(key) &&
        args.contains("module")) {
        key = "module";
    }
    const std::string name = optionalString(args, key);
    if (name.empty()) {
        throw std::runtime_error(
            allowLegacyArguments
                ? "module_name or module must be a non-empty string"
                : "module_name must be a non-empty string");
    }
    return name;
}

std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return value;
}

uint64_t scanEpochArgument(const json& args) {
    if (!args.contains("scan_epoch")) {
        throw std::runtime_error("scan_epoch is required");
    }
    const json& value = args.at("scan_epoch");
    if (value.is_number_unsigned()) {
        return value.get<uint64_t>();
    }
    if (value.is_number_integer()) {
        const long long epoch = value.get<long long>();
        if (epoch >= 0) {
            return static_cast<uint64_t>(epoch);
        }
    }
    throw std::runtime_error("scan_epoch must be a non-negative integer");
}

Mem::ScanMode scanModeArgument(const json& args,
                               const char* fallback = "exact") {
    std::string mode = optionalString(args, "mode");
    if (mode.empty()) {
        mode = fallback;
    }
    mode = lowerAscii(mode);
    if (mode == "exact") return Mem::ScanMode::Exact;
    if (mode == "greater") return Mem::ScanMode::Greater;
    if (mode == "less") return Mem::ScanMode::Less;
    if (mode == "between") return Mem::ScanMode::Between;
    if (mode == "unknown") return Mem::ScanMode::Unknown;
    if (mode == "increased") return Mem::ScanMode::Increased;
    if (mode == "increased_by") return Mem::ScanMode::IncreasedBy;
    if (mode == "decreased") return Mem::ScanMode::Decreased;
    if (mode == "decreased_by") return Mem::ScanMode::DecreasedBy;
    if (mode == "changed") return Mem::ScanMode::Changed;
    if (mode == "unchanged") return Mem::ScanMode::Unchanged;
    throw std::runtime_error("unsupported scan mode: " + mode);
}

const char* scanModeName(Mem::ScanMode mode) {
    switch (mode) {
        case Mem::ScanMode::Exact:       return "exact";
        case Mem::ScanMode::Greater:     return "greater";
        case Mem::ScanMode::Less:        return "less";
        case Mem::ScanMode::Between:     return "between";
        case Mem::ScanMode::Unknown:     return "unknown";
        case Mem::ScanMode::Increased:   return "increased";
        case Mem::ScanMode::IncreasedBy: return "increased_by";
        case Mem::ScanMode::Decreased:   return "decreased";
        case Mem::ScanMode::DecreasedBy: return "decreased_by";
        case Mem::ScanMode::Changed:     return "changed";
        case Mem::ScanMode::Unchanged:   return "unchanged";
        default:                         return "unknown";
    }
}

Mem::ScanDataType scanDataTypeFromScalar(Mem::ScalarType type) {
    switch (type) {
        case Mem::ScalarType::Byte:   return Mem::ScanDataType::Byte;
        case Mem::ScalarType::Word:   return Mem::ScanDataType::Word;
        case Mem::ScalarType::Dword:  return Mem::ScanDataType::Dword;
        case Mem::ScalarType::Qword:  return Mem::ScanDataType::Qword;
        case Mem::ScalarType::Xor:    return Mem::ScanDataType::Xor;
        case Mem::ScalarType::Float:  return Mem::ScanDataType::Float;
        case Mem::ScalarType::Double: return Mem::ScanDataType::Double;
        default:                      return Mem::ScanDataType::Dword;
    }
}

Mem::Result<Mem::ScalarType> scanScalarTypeArgument(const json& args) {
    std::string type = optionalString(args, "data_type");
    if (type.empty()) {
        type = "dword";
    }
    return Mem::parseScalarType(type);
}

std::string scanScalarText(const json& args, const char* key) {
    if (!args.contains(key)) {
        throw std::runtime_error(std::string(key) + " is required");
    }
    const json& value = args.at(key);
    std::string text;
    if (value.is_string()) {
        text = value.get<std::string>();
    } else if (value.is_number_unsigned()) {
        text = std::to_string(value.get<unsigned long long>());
    } else if (value.is_number_integer()) {
        text = std::to_string(value.get<long long>());
    } else if (value.is_number_float()) {
        text = value.dump();
    } else {
        throw std::runtime_error(std::string(key) +
                                 " must be a string or number");
    }
    if (text.size() > Mem::kMaxScalarValueTextBytes) {
        throw std::runtime_error(std::string(key) + " exceeds 256 bytes");
    }
    return text;
}

Mem::ScanMemoryRegion scanMemoryRegionArgument(const json& args) {
    std::string region = optionalString(args, "memory_type");
    if (region.empty()) region = "all";
    region = lowerAscii(region);
    if (region == "all") return Mem::ScanMemoryRegion::All;
    if (region == "anonymous") return Mem::ScanMemoryRegion::Anonymous;
    if (region == "c_alloc") return Mem::ScanMemoryRegion::CAlloc;
    if (region == "c_heap") return Mem::ScanMemoryRegion::CHeap;
    if (region == "c_data") return Mem::ScanMemoryRegion::CData;
    if (region == "c_bss") return Mem::ScanMemoryRegion::CBss;
    if (region == "java_heap") return Mem::ScanMemoryRegion::JavaHeap;
    if (region == "java") return Mem::ScanMemoryRegion::Java;
    if (region == "stack") return Mem::ScanMemoryRegion::Stack;
    if (region == "video") return Mem::ScanMemoryRegion::Video;
    if (region == "code_app") return Mem::ScanMemoryRegion::CodeApp;
    if (region == "code_system") return Mem::ScanMemoryRegion::CodeSystem;
    if (region == "ashmem") return Mem::ScanMemoryRegion::Ashmem;
    if (region == "bad") return Mem::ScanMemoryRegion::Bad;
    if (region == "other") return Mem::ScanMemoryRegion::Other;
    throw std::runtime_error("unsupported memory_type: " + region);
}

const char* scanMemoryRegionName(Mem::ScanMemoryRegion region) {
    switch (region) {
        case Mem::ScanMemoryRegion::All:        return "all";
        case Mem::ScanMemoryRegion::Anonymous:  return "anonymous";
        case Mem::ScanMemoryRegion::CAlloc:     return "c_alloc";
        case Mem::ScanMemoryRegion::CHeap:      return "c_heap";
        case Mem::ScanMemoryRegion::CData:      return "c_data";
        case Mem::ScanMemoryRegion::CBss:       return "c_bss";
        case Mem::ScanMemoryRegion::JavaHeap:   return "java_heap";
        case Mem::ScanMemoryRegion::Java:       return "java";
        case Mem::ScanMemoryRegion::Stack:      return "stack";
        case Mem::ScanMemoryRegion::Video:      return "video";
        case Mem::ScanMemoryRegion::CodeApp:    return "code_app";
        case Mem::ScanMemoryRegion::CodeSystem: return "code_system";
        case Mem::ScanMemoryRegion::Ashmem:     return "ashmem";
        case Mem::ScanMemoryRegion::Bad:        return "bad";
        case Mem::ScanMemoryRegion::Other:      return "other";
        default:                                return "unknown";
    }
}

const char* scanDataTypeName(Mem::ScanDataType type) {
    switch (type) {
        case Mem::ScanDataType::Byte:   return "byte";
        case Mem::ScanDataType::Word:   return "word";
        case Mem::ScanDataType::Dword:  return "dword";
        case Mem::ScanDataType::Qword:  return "qword";
        case Mem::ScanDataType::Xor:    return "xor";
        case Mem::ScanDataType::Float:  return "float";
        case Mem::ScanDataType::Double: return "double";
        case Mem::ScanDataType::Bytes:  return "bytes";
        default:                        return "unknown";
    }
}

const char* scanKindName(Mem::ScanStartKind kind) {
    switch (kind) {
        case Mem::ScanStartKind::Value:       return "value";
        case Mem::ScanStartKind::Unknown:     return "unknown";
        case Mem::ScanStartKind::BytePattern: return "byte_pattern";
        default:                              return "unknown";
    }
}

void parseCanonicalScanRange(const json& args,
                             uint64_t& start,
                             uint64_t& end) {
    start = 0;
    end = (std::numeric_limits<uint64_t>::max)();
    if (args.contains("start") && !args.at("start").is_null()) {
        const auto parsed = parseAddressArgument(args.at("start"), false);
        if (!parsed.ok()) throw std::runtime_error(parsed.error().message);
        start = parsed.value();
    }
    if (args.contains("end") && !args.at("end").is_null()) {
        const auto parsed = parseAddressArgument(args.at("end"), false);
        if (!parsed.ok()) throw std::runtime_error(parsed.error().message);
        end = parsed.value();
    }
    if (start > end) {
        throw std::runtime_error("scan start must be <= end");
    }
}

json scanSessionJson(const Mem::ScanSessionSnapshot& session) {
    return {
        {"scan_epoch", session.epoch},
        {"kind", scanKindName(session.kind)},
        {"data_type", scanDataTypeName(session.dataType)},
        {"mode", scanModeName(session.mode)},
        {"memory_type", scanMemoryRegionName(session.memoryRegion)},
        {"start", Mem::formatAddress(session.start)},
        {"end", Mem::formatAddress(session.end)},
        {"result_count", session.resultCount},
    };
}

std::string writeHexArgument(const json& args, bool allowLegacyArguments) {
    const char* key = "data_hex";
    if (allowLegacyArguments && !args.contains(key)) {
        if (args.contains("hex_string")) {
            key = "hex_string";
        } else if (args.contains("hex")) {
            key = "hex";
        }
    }
    if (!args.contains(key) || !args.at(key).is_string()) {
        throw std::runtime_error(
            allowLegacyArguments
                ? "data_hex, hex_string, or hex must be a string"
                : "data_hex must be a string");
    }
    const std::string value = args.at(key).get<std::string>();
    if (value.size() > 16u * 1024u) {
        throw std::runtime_error(std::string(key) + " exceeds 16384 bytes");
    }
    return value;
}

std::vector<unsigned char> parseHexBytes(const std::string& input) {
    std::string digits;
    digits.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        const char c = input[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',') {
            continue;
        }
        if (c == '0' && i + 1 < input.size() &&
            (input[i + 1] == 'x' || input[i + 1] == 'X')) {
            ++i;
            continue;
        }
        digits.push_back(c);
    }
    if (digits.empty()) {
        throw std::runtime_error("hex byte string must contain at least one byte");
    }
    if ((digits.size() % 2u) != 0u) {
        throw std::runtime_error("hex byte string has an odd number of nibbles");
    }

    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        return -1;
    };

    std::vector<unsigned char> bytes;
    bytes.reserve(digits.size() / 2u);
    for (size_t i = 0; i < digits.size(); i += 2u) {
        const int high = nibble(digits[i]);
        const int low = nibble(digits[i + 1u]);
        if (high < 0 || low < 0) {
            throw std::runtime_error("hex byte string contains a non-hex character");
        }
        bytes.push_back(static_cast<unsigned char>((high << 4) | low));
    }
    return bytes;
}

std::string compactHex(const std::vector<unsigned char>& bytes) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string output;
    output.reserve(bytes.size() * 2);
    for (unsigned char byte : bytes) {
        output.push_back(kHex[(byte >> 4) & 0x0F]);
        output.push_back(kHex[byte & 0x0F]);
    }
    return output;
}

std::string spacedHex(const std::vector<unsigned char>& bytes) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string output;
    if (!bytes.empty()) {
        output.reserve(bytes.size() * 3 - 1);
    }
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (i != 0) {
            output.push_back(' ');
        }
        output.push_back(kHex[(bytes[i] >> 4) & 0x0F]);
        output.push_back(kHex[bytes[i] & 0x0F]);
    }
    return output;
}

} // namespace

AgentMemTools::AgentMemTools(Mem::IMemService& service) : service_(service) {}

std::string AgentMemTools::status(
    const std::string& /*argsJson*/,
    const Mem::OperationContext& context) {
    const auto response = service_.status(context);
    if (!response.ok()) {
        return errorResult(response.error(), response.durationMs());
    }

    const Mem::Status& status = response.value();
    json output;
    output["success"] = true;
    output["connected"] = status.connected;
    output["connection_poisoned"] = status.connectionPoisoned;
    output["pid"] = status.target.pid;
    output["process_name"] = status.processName;
    output["handle"] = status.target.processHandle;
    output["process_revision"] = status.target.processRevision;
    output["connection_generation"] = status.connectionGeneration;
    output["server"] = {
        {"available", status.serverVersion.has_value()},
        {"version", status.serverVersion.value_or(0)},
        {"version_string", status.serverVersionString},
    };
    output["architecture"] = {
        {"available", status.architectureType.has_value()},
        {"type", status.architectureType.value_or(0)},
        {"name", status.architectureName},
    };
    output["features"] = {
#ifdef HAVE_LUAJIT
        {"lua", true},
#else
        {"lua", false},
#endif
#ifdef HAVE_CAPSTONE
        {"disassembly", true},
#else
        {"disassembly", false},
#endif
#ifdef HAVE_KEYSTONE
        {"assembly", true},
#else
        {"assembly", false},
#endif
    };
    output["meta"] = resultMeta(response.durationMs(),
                                status.connectionGeneration,
                                &status.target);
    return output.dump();
}

std::string AgentMemTools::serverVersion(
    const std::string& /*argsJson*/,
    const Mem::OperationContext& context) {
    const auto response = service_.status(context);
    if (!response.ok()) {
        return errorResult(response.error(), response.durationMs());
    }
    if (!response.value().serverVersion) {
        return errorResult(
            Mem::Error{Mem::ErrorCode::ProtocolError,
                       "server version is unavailable", true},
            response.durationMs());
    }

    json output;
    output["success"] = true;
    output["version"] = *response.value().serverVersion;
    output["version_string"] = response.value().serverVersionString;
    output["meta"] = resultMeta(response.durationMs(),
                                response.value().connectionGeneration);
    return output.dump();
}

std::string AgentMemTools::architecture(
    const std::string& /*argsJson*/,
    const Mem::OperationContext& context) {
    const auto response = service_.status(context);
    if (!response.ok()) {
        return errorResult(response.error(), response.durationMs());
    }
    if (!response.value().architectureType) {
        return errorResult(
            Mem::Error{Mem::ErrorCode::ProtocolError,
                       "memory architecture is unavailable", true},
            response.durationMs());
    }

    json output;
    output["success"] = true;
    output["type"] = *response.value().architectureType;
    output["name"] = response.value().architectureName;
    output["meta"] = resultMeta(response.durationMs(),
                                response.value().connectionGeneration);
    return output.dump();
}

std::string AgentMemTools::processList(
    const std::string& argsJson,
    const Mem::OperationContext& context) {
    try {
        const json args = json::parse(argsJson.empty() ? "{}" : argsJson);
        Mem::ProcessListRequest request;
        request.filter = optionalString(args, "filter");
        request.offset = optionalSize(args, "offset", 0,
                                      (std::numeric_limits<int>::max)());
        request.limit = optionalSize(args, "count", 200,
                                     Mem::kMaxProcessPageSize);

        const auto response = service_.listProcesses(context, request);
        if (!response.ok()) {
            return errorResult(response.error(), response.durationMs());
        }

        json processes = json::array();
        for (const auto& process : response.value().items) {
            processes.push_back({{"pid", process.pid}, {"name", process.name}});
        }

        json output;
        output["success"] = true;
        output["processes"] = std::move(processes);
        output["total"] = response.value().total;
        output["offset"] = response.value().offset;
        output["count"] = response.value().items.size();
        output["truncated"] = response.value().nextOffset.has_value();
        output["next_cursor"] = response.value().nextOffset
                                    ? json(*response.value().nextOffset)
                                    : json(nullptr);
        output["meta"] = resultMeta(response.durationMs(),
                                    context.connectionGeneration);
        return output.dump();
    } catch (const std::exception& error) {
        return exceptionResult("process_list", error);
    }
}

std::string AgentMemTools::processOpen(
    const std::string& argsJson,
    const Mem::OperationContext& context) {
    try {
        const json args = json::parse(argsJson.empty() ? "{}" : argsJson);
        Mem::OpenProcessRequest request;
        request.pid = requiredPositiveInt(args, "pid");
        request.name = optionalString(args, "name");

        const auto response = service_.openProcess(context, request);
        if (!response.ok()) {
            return errorResult(response.error(), response.durationMs());
        }

        const Mem::TargetSnapshot& target = response.value().target;
        json output;
        output["success"] = true;
        output["pid"] = target.pid;
        output["name"] = response.value().name;
        output["handle"] = target.processHandle;
        output["process_revision"] = target.processRevision;
        output["connection_generation"] = target.connectionGeneration;
        output["meta"] = resultMeta(response.durationMs(),
                                    target.connectionGeneration,
                                    &target);
        return output.dump();
    } catch (const std::exception& error) {
        return exceptionResult("process_open", error);
    }
}

std::string AgentMemTools::moduleList(
    const std::string& argsJson,
    bool allowLegacyArguments,
    const Mem::OperationContext& context) {
    try {
        const json args = json::parse(argsJson.empty() ? "{}" : argsJson);
        Mem::ModuleListRequest request;
        request.filter = optionalString(args, "filter");
        request.offset = optionalSize(args, "offset", 0,
                                      (std::numeric_limits<int>::max)());
        request.limit = optionalSize(
            args,
            "count",
            allowLegacyArguments ? 1000 : 200,
            Mem::kMaxModulePageSize);

        const auto response = service_.listModules(context, request);
        if (!response.ok()) {
            return errorResult(response.error(), response.durationMs());
        }

        json modules = json::array();
        for (const auto& module : response.value().items) {
            modules.push_back({
                {"name", module.name},
                {"base", Mem::formatAddress(module.base)},
                {"size", module.size},
                {"type", module.type},
                {"flag", module.flag},
            });
        }

        json output;
        output["success"] = true;
        output["modules"] = std::move(modules);
        output["total"] = response.value().total;
        output["offset"] = response.value().offset;
        output["count"] = response.value().items.size();
        output["truncated"] = response.value().nextOffset.has_value();
        output["next_cursor"] = response.value().nextOffset
                                    ? json(*response.value().nextOffset)
                                    : json(nullptr);
        output["meta"] = resultMeta(response.durationMs(),
                                    response.value().target.connectionGeneration,
                                    &response.value().target);
        return output.dump();
    } catch (const std::exception& error) {
        return exceptionResult(
            allowLegacyArguments ? "legacy module list" : "module_list",
            error);
    }
}

std::string AgentMemTools::moduleResolve(
    const std::string& argsJson,
    bool allowLegacyArguments,
    const Mem::OperationContext& context) {
    try {
        const json args = json::parse(argsJson.empty() ? "{}" : argsJson);
        Mem::ModuleResolveRequest request;
        request.name = moduleNameArgument(args, allowLegacyArguments);

        const auto response = service_.resolveModule(context, request);
        if (!response.ok()) {
            return errorResult(response.error(), response.durationMs());
        }

        const Mem::ModuleInfo& module = response.value().module;
        json output;
        output["success"] = true;
        output["query"] = request.name;
        output["module"] = module.name;
        output["base"] = Mem::formatAddress(module.base);
        output["size"] = module.size;
        output["type"] = module.type;
        output["flag"] = module.flag;
        output["meta"] = resultMeta(response.durationMs(),
                                    response.value().target.connectionGeneration,
                                    &response.value().target);
        return output.dump();
    } catch (const std::exception& error) {
        return exceptionResult(
            allowLegacyArguments ? "get_module_base" : "module_resolve",
            error);
    }
}

std::string AgentMemTools::pointerResolve(
    const std::string& argsJson,
    bool allowLegacyArguments,
    const Mem::OperationContext& context) {
    try {
        const json args = json::parse(argsJson.empty() ? "{}" : argsJson);
        if (!args.contains("base_offset")) {
            throw std::runtime_error("base_offset is required");
        }

        Mem::PointerResolveRequest request;
        request.moduleName =
            pointerModuleNameArgument(args, allowLegacyArguments);
        const auto baseOffset = parseAddressArgument(
            args.at("base_offset"), allowLegacyArguments);
        if (!baseOffset.ok()) {
            return errorResult(baseOffset.error(), baseOffset.durationMs());
        }
        request.baseOffset = baseOffset.value();

        if (args.contains("offsets") && !args.at("offsets").is_null()) {
            if (!args.at("offsets").is_array()) {
                throw std::runtime_error("offsets must be an array");
            }
            if (args.at("offsets").size() > Mem::kMaxPointerOffsetCount) {
                throw std::runtime_error("offsets chain is too long");
            }
            request.offsets.reserve(args.at("offsets").size());
            for (const auto& value : args.at("offsets")) {
                const auto offset =
                    parseAddressArgument(value, allowLegacyArguments);
                if (!offset.ok()) {
                    return errorResult(offset.error(), offset.durationMs());
                }
                request.offsets.push_back(offset.value());
            }
        }
        request.dereferenceFinal =
            optionalBool(args, "deref_final", true);
        if (allowLegacyArguments && request.offsets.empty()) {
            // The old helper returned module_base + base_offset immediately
            // for an empty chain, regardless of deref_final.
            request.dereferenceFinal = false;
        }

        const auto response = service_.resolvePointer(context, request);
        if (!response.ok()) {
            return errorResult(response.error(), response.durationMs());
        }

        const Mem::PointerResolution& value = response.value();
        json output;
        output["success"] = true;
        output["query"] = request.moduleName;
        output["module"] = value.module.name;
        output["module_base"] = Mem::formatAddress(value.module.base);
        output["base_offset"] = Mem::formatAddress(value.baseOffset);
        output["start_address"] = Mem::formatAddress(value.startAddress);
        output["address"] = Mem::formatAddress(value.address);
        output["dereference_count"] = value.dereferenceCount;
        output["dereferenced_final"] = value.dereferencedFinal;
        output["meta"] = resultMeta(response.durationMs(),
                                    value.target.connectionGeneration,
                                    &value.target);
        return output.dump();
    } catch (const std::exception& error) {
        return exceptionResult(
            allowLegacyArguments ? "resolve_offset_chain" : "pointer_resolve",
            error);
    }
}

std::string AgentMemTools::scanStart(
    const std::string& argsJson,
    const Mem::OperationContext& context) {
    try {
        const json args = json::parse(argsJson.empty() ? "{}" : argsJson);
        Mem::ScanStartRequest request;
        request.memoryRegion = scanMemoryRegionArgument(args);
        parseCanonicalScanRange(args, request.start, request.end);

        if (args.contains("pattern_hex") &&
            !args.at("pattern_hex").is_null()) {
            if (args.contains("value") || args.contains("upper_value") ||
                args.contains("data_type")) {
                throw std::runtime_error(
                    "pattern_hex cannot be combined with scalar scan fields");
            }
            if (!args.at("pattern_hex").is_string()) {
                throw std::runtime_error("pattern_hex must be a string");
            }
            const std::string pattern =
                args.at("pattern_hex").get<std::string>();
            if (pattern.size() > 16u * 1024u) {
                throw std::runtime_error("pattern_hex exceeds 16384 bytes");
            }
            request.kind = Mem::ScanStartKind::BytePattern;
            request.dataType = Mem::ScanDataType::Bytes;
            request.mode = scanModeArgument(args, "exact");
            request.value = parseHexBytes(pattern);
        } else {
            const auto scalarType = scanScalarTypeArgument(args);
            if (!scalarType.ok()) {
                return errorResult(scalarType.error(),
                                   scalarType.durationMs());
            }
            request.dataType = scanDataTypeFromScalar(scalarType.value());
            request.mode = scanModeArgument(args, "exact");
            if (request.mode == Mem::ScanMode::Unknown) {
                if (args.contains("value") ||
                    args.contains("upper_value")) {
                    throw std::runtime_error(
                        "unknown scan mode does not accept comparison values");
                }
                request.kind = Mem::ScanStartKind::Unknown;
            } else {
                if (request.mode != Mem::ScanMode::Between &&
                    args.contains("upper_value")) {
                    throw std::runtime_error(
                        "upper_value is only valid for between mode");
                }
                request.kind = Mem::ScanStartKind::Value;
                const auto encoded = Mem::encodeScalarValue(
                    scalarType.value(), scanScalarText(args, "value"));
                if (!encoded.ok()) {
                    return errorResult(encoded.error(),
                                       encoded.durationMs());
                }
                request.value = encoded.value();
                if (request.mode == Mem::ScanMode::Between) {
                    const auto upper = Mem::encodeScalarValue(
                        scalarType.value(),
                        scanScalarText(args, "upper_value"));
                    if (!upper.ok()) {
                        return errorResult(upper.error(),
                                           upper.durationMs());
                    }
                    request.value.insert(request.value.end(),
                                         upper.value().begin(),
                                         upper.value().end());
                }
            }
        }

        const auto response = service_.startScan(context, request);
        if (!response.ok()) {
            return errorResult(response.error(), response.durationMs());
        }

        const Mem::ScanSummary& summary = response.value();
        json output = scanSessionJson(summary.session);
        output["success"] = true;
        output["scan_session"] = scanSessionJson(summary.session);
        if (summary.completedAfterCancelRequest) {
            output["completion"] = "completed_after_cancel_request";
        } else if (summary.completedAfterDeadline) {
            output["completion"] = "completed_after_deadline";
        } else {
            output["completion"] = "completed";
        }
        output["meta"] = resultMeta(response.durationMs(),
                                    summary.session.target.connectionGeneration,
                                    &summary.session.target);
        return output.dump();
    } catch (const std::exception& error) {
        return exceptionResult("scan_start", error);
    }
}

std::string AgentMemTools::scanRefine(
    const std::string& argsJson,
    const Mem::OperationContext& context) {
    try {
        const json args = json::parse(argsJson.empty() ? "{}" : argsJson);
        Mem::ScanRefineRequest request;
        request.expectedEpoch = scanEpochArgument(args);
        request.mode = scanModeArgument(args, "exact");

        const auto scalarType = scanScalarTypeArgument(args);
        if (!scalarType.ok()) {
            return errorResult(scalarType.error(), scalarType.durationMs());
        }
        request.dataType = scanDataTypeFromScalar(scalarType.value());
        const bool needsValue =
            request.mode == Mem::ScanMode::Exact ||
            request.mode == Mem::ScanMode::Greater ||
            request.mode == Mem::ScanMode::Less ||
            request.mode == Mem::ScanMode::Between ||
            request.mode == Mem::ScanMode::IncreasedBy ||
            request.mode == Mem::ScanMode::DecreasedBy;
        if (needsValue) {
            if (request.mode != Mem::ScanMode::Between &&
                args.contains("upper_value")) {
                throw std::runtime_error(
                    "upper_value is only valid for between mode");
            }
            const auto encoded = Mem::encodeScalarValue(
                scalarType.value(), scanScalarText(args, "value"));
            if (!encoded.ok()) {
                return errorResult(encoded.error(), encoded.durationMs());
            }
            request.value = encoded.value();
            if (request.mode == Mem::ScanMode::Between) {
                const auto upper = Mem::encodeScalarValue(
                    scalarType.value(),
                    scanScalarText(args, "upper_value"));
                if (!upper.ok()) {
                    return errorResult(upper.error(), upper.durationMs());
                }
                request.value.insert(request.value.end(),
                                     upper.value().begin(),
                                     upper.value().end());
            }
        } else if (args.contains("value") ||
                   args.contains("upper_value")) {
            throw std::runtime_error(
                "this scan refine mode does not accept comparison values");
        }

        const auto response = service_.refineScan(context, request);
        if (!response.ok()) {
            return errorResult(response.error(), response.durationMs());
        }

        const Mem::ScanSummary& summary = response.value();
        json output = scanSessionJson(summary.session);
        output["success"] = true;
        output["scan_session"] = scanSessionJson(summary.session);
        if (summary.completedAfterCancelRequest) {
            output["completion"] = "completed_after_cancel_request";
        } else if (summary.completedAfterDeadline) {
            output["completion"] = "completed_after_deadline";
        } else {
            output["completion"] = "completed";
        }
        output["meta"] = resultMeta(response.durationMs(),
                                    summary.session.target.connectionGeneration,
                                    &summary.session.target);
        return output.dump();
    } catch (const std::exception& error) {
        return exceptionResult("scan_refine", error);
    }
}

std::string AgentMemTools::scanResults(
    const std::string& argsJson,
    const Mem::OperationContext& context) {
    try {
        const json args = json::parse(argsJson.empty() ? "{}" : argsJson);
        Mem::ScanResultsRequest request;
        request.expectedEpoch = scanEpochArgument(args);
        request.offset = optionalSize(
            args, "offset", 0, Mem::kMaxScanResultCount);
        request.limit = optionalSize(
            args, "count", 100, Mem::kMaxScanResultPageSize);

        const auto response = service_.scanResults(context, request);
        if (!response.ok()) {
            return errorResult(response.error(), response.durationMs());
        }

        json items = json::array();
        for (const auto& item : response.value().items) {
            items.push_back({
                {"address", Mem::formatAddress(item.address)},
                {"value_text", std::to_string(item.value)},
                {"value_hex", Mem::formatAddress(item.value)},
            });
        }
        json output;
        output["success"] = true;
        output["scan_epoch"] = response.value().session.epoch;
        output["total"] = response.value().total;
        output["offset"] = response.value().offset;
        output["count"] = response.value().items.size();
        output["items"] = std::move(items);
        output["truncated"] = response.value().nextOffset.has_value();
        output["next_cursor"] = response.value().nextOffset
                                    ? json(*response.value().nextOffset)
                                    : json(nullptr);
        output["scan_session"] =
            scanSessionJson(response.value().session);
        output["meta"] = resultMeta(
            response.durationMs(),
            response.value().session.target.connectionGeneration,
            &response.value().session.target);
        return output.dump();
    } catch (const std::exception& error) {
        return exceptionResult("scan_results", error);
    }
}

std::string AgentMemTools::scanClear(
    const std::string& argsJson,
    const Mem::OperationContext& context) {
    try {
        const json args = json::parse(argsJson.empty() ? "{}" : argsJson);
        Mem::ScanClearRequest request;
        request.expectedEpoch = scanEpochArgument(args);
        const auto response = service_.clearScan(context, request);
        if (!response.ok()) {
            return errorResult(response.error(), response.durationMs());
        }

        json output;
        output["success"] = true;
        output["cleared"] = true;
        output["cleared_epoch"] = response.value().clearedEpoch;
        output["current_epoch"] = response.value().currentEpoch;
        if (response.value().completedAfterCancelRequest) {
            output["completion"] = "completed_after_cancel_request";
        } else if (response.value().completedAfterDeadline) {
            output["completion"] = "completed_after_deadline";
        } else {
            output["completion"] = "completed";
        }
        output["meta"] = resultMeta(
            response.durationMs(),
            response.value().target.connectionGeneration,
            &response.value().target);
        return output.dump();
    } catch (const std::exception& error) {
        return exceptionResult("scan_clear", error);
    }
}

std::string AgentMemTools::memoryRead(const std::string& argsJson,
                                      bool allowLegacyAddress,
                                      const Mem::OperationContext& context) {
    try {
        const json args = json::parse(argsJson.empty() ? "{}" : argsJson);
        if (!args.contains("address")) {
            throw std::runtime_error("address is required");
        }
        const auto parsedAddress =
            parseAddressArgument(args.at("address"), allowLegacyAddress);
        if (!parsedAddress.ok()) {
            return errorResult(parsedAddress.error(), parsedAddress.durationMs());
        }

        const size_t size = optionalSize(args, "size", 256,
                                         Mem::kMaxAgentMemoryReadBytes);
        Mem::MemoryReadRequest request;
        request.address = parsedAddress.value();
        request.size = static_cast<uint32_t>(size);

        const auto response = service_.readMemory(context, request);
        if (!response.ok()) {
            return errorResult(response.error(), response.durationMs());
        }

        const Mem::MemoryBlock& block = response.value();
        json output;
        output["success"] = true;
        output["address"] = Mem::formatAddress(block.address);
        output["size"] = block.bytes.size();
        output["hex"] = compactHex(block.bytes);
        output["data"] = spacedHex(block.bytes);
        output["meta"] = resultMeta(response.durationMs(),
                                    block.target.connectionGeneration,
                                    &block.target);
        return output.dump();
    } catch (const std::exception& error) {
        return exceptionResult("memory_read", error);
    }
}

std::string AgentMemTools::memoryReadValue(
    const std::string& argsJson,
    bool allowLegacyArguments,
    const Mem::OperationContext& context) {
    try {
        const json args = json::parse(argsJson.empty() ? "{}" : argsJson);
        if (!args.contains("address")) {
            throw std::runtime_error("address is required");
        }
        const auto parsedAddress =
            parseAddressArgument(args.at("address"), allowLegacyArguments);
        if (!parsedAddress.ok()) {
            return errorResult(parsedAddress.error(),
                               parsedAddress.durationMs());
        }
        const auto parsedType = scalarTypeArgument(args, allowLegacyArguments);
        if (!parsedType.ok()) {
            return errorResult(parsedType.error(), parsedType.durationMs());
        }

        Mem::ValueReadRequest request;
        request.address = parsedAddress.value();
        request.type = parsedType.value();
        const auto response = service_.readValue(context, request);
        if (!response.ok()) {
            return errorResult(response.error(), response.durationMs());
        }

        const Mem::ScalarValue& value = response.value();
        const auto decoded = Mem::decodeScalarValue(value.type, value.bytes);
        if (!decoded.ok()) {
            return errorResult(decoded.error(), response.durationMs());
        }

        json output;
        output["success"] = true;
        output["address"] = Mem::formatAddress(value.address);
        output["data_type"] = Mem::scalarTypeName(value.type);
        output["hex"] = compactHex(value.bytes);
        output["value_text"] = Mem::formatScalarValue(decoded.value());
        if (decoded.value().floatingPoint) {
            output["value"] = std::isfinite(decoded.value().floatingValue)
                ? json(decoded.value().floatingValue)
                : json(nullptr);
        } else {
            output["value"] = decoded.value().integerValue;
            output["value_hex"] =
                Mem::formatAddress(decoded.value().integerValue);
        }
        output["meta"] = resultMeta(response.durationMs(),
                                    value.target.connectionGeneration,
                                    &value.target);
        return output.dump();
    } catch (const std::exception& error) {
        return exceptionResult(
            allowLegacyArguments ? "read_value" : "memory_read_value",
            error);
    }
}

std::string AgentMemTools::memoryWrite(
    const std::string& argsJson,
    bool allowLegacyArguments,
    const Mem::OperationContext& context) {
    try {
        const json args = json::parse(argsJson.empty() ? "{}" : argsJson);
        if (!args.contains("address")) {
            throw std::runtime_error("address is required");
        }
        const auto parsedAddress =
            parseAddressArgument(args.at("address"), allowLegacyArguments);
        if (!parsedAddress.ok()) {
            return errorResult(parsedAddress.error(), parsedAddress.durationMs());
        }

        Mem::MemoryWriteRequest request;
        request.address = parsedAddress.value();
        request.bytes = parseHexBytes(
            writeHexArgument(args, allowLegacyArguments));
        if (request.bytes.size() > Mem::kMaxAgentMemoryWriteBytes) {
            throw std::runtime_error(
                "memory write exceeds maximum size of 4096 bytes");
        }

        const auto response = service_.writeMemory(context, request);
        if (!response.ok()) {
            return errorResult(response.error(), response.durationMs());
        }

        const Mem::WriteReceipt& receipt = response.value();
        json output;
        output["success"] = true;
        output["address"] = Mem::formatAddress(receipt.address);
        output["requested_bytes"] = receipt.requestedBytes;
        output["written_bytes"] = receipt.writtenBytes;
        output["completed_after_cancel_request"] =
            receipt.completedAfterCancelRequest;
        output["completed_after_deadline"] =
            receipt.completedAfterDeadline;
        output["completion"] = receipt.completedAfterCancelRequest
            ? "completed_after_cancel_request"
            : (receipt.completedAfterDeadline
                   ? "completed_after_deadline"
                   : "completed");
        output["meta"] = resultMeta(response.durationMs(),
                                    receipt.target.connectionGeneration,
                                    &receipt.target);
        return output.dump();
    } catch (const std::exception& error) {
        return exceptionResult(
            allowLegacyArguments ? "write_bytes" : "memory_write", error);
    }
}

std::string AgentMemTools::memoryWriteValue(
    const std::string& argsJson,
    bool allowLegacyArguments,
    const Mem::OperationContext& context) {
    try {
        const json args = json::parse(argsJson.empty() ? "{}" : argsJson);
        if (!args.contains("address")) {
            throw std::runtime_error("address is required");
        }
        const auto parsedAddress =
            parseAddressArgument(args.at("address"), allowLegacyArguments);
        if (!parsedAddress.ok()) {
            return errorResult(parsedAddress.error(),
                               parsedAddress.durationMs());
        }
        const auto parsedType = scalarTypeArgument(args, allowLegacyArguments);
        if (!parsedType.ok()) {
            return errorResult(parsedType.error(), parsedType.durationMs());
        }
        const auto encoded = Mem::encodeScalarValue(
            parsedType.value(), scalarValueText(args));
        if (!encoded.ok()) {
            return errorResult(encoded.error(), encoded.durationMs());
        }

        Mem::ValueWriteRequest request;
        request.address = parsedAddress.value();
        request.type = parsedType.value();
        request.bytes = encoded.value();
        const auto response = service_.writeValue(context, request);
        if (!response.ok()) {
            return errorResult(response.error(), response.durationMs());
        }

        const Mem::WriteReceipt& receipt = response.value();
        json output;
        output["success"] = true;
        output["address"] = Mem::formatAddress(receipt.address);
        output["data_type"] = Mem::scalarTypeName(request.type);
        output["written_bytes"] = receipt.writtenBytes;
        output["hex"] = compactHex(request.bytes);
        output["completed_after_cancel_request"] =
            receipt.completedAfterCancelRequest;
        output["completed_after_deadline"] =
            receipt.completedAfterDeadline;
        output["completion"] = receipt.completedAfterCancelRequest
            ? "completed_after_cancel_request"
            : (receipt.completedAfterDeadline
                   ? "completed_after_deadline"
                   : "completed");
        output["meta"] = resultMeta(response.durationMs(),
                                    receipt.target.connectionGeneration,
                                    &receipt.target);
        return output.dump();
    } catch (const std::exception& error) {
        return exceptionResult(
            allowLegacyArguments ? "write_value" : "memory_write_value",
            error);
    }
}

} // namespace AI
