#include "AgentMemTools.h"

#include "../../mem/Address.h"
#include "../../third_party/nlohmann/json.hpp"

#include <algorithm>
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

std::string AgentMemTools::status(const std::string& /*argsJson*/) {
    const Mem::OperationContext context = service_.captureContext(false);
    const auto response = service_.status(context);
    if (!response.ok()) {
        return errorResult(response.error(), response.durationMs());
    }

    const Mem::Status& status = response.value();
    json output;
    output["success"] = true;
    output["connected"] = status.connected;
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

std::string AgentMemTools::serverVersion(const std::string& /*argsJson*/) {
    const Mem::OperationContext context = service_.captureContext(false);
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

std::string AgentMemTools::architecture(const std::string& /*argsJson*/) {
    const Mem::OperationContext context = service_.captureContext(false);
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

std::string AgentMemTools::processList(const std::string& argsJson) {
    try {
        const json args = json::parse(argsJson.empty() ? "{}" : argsJson);
        Mem::ProcessListRequest request;
        request.filter = optionalString(args, "filter");
        request.offset = optionalSize(args, "offset", 0,
                                      (std::numeric_limits<int>::max)());
        request.limit = optionalSize(args, "count", 200,
                                     Mem::kMaxProcessPageSize);

        const Mem::OperationContext context = service_.captureContext(false);
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

std::string AgentMemTools::processOpen(const std::string& argsJson) {
    try {
        const json args = json::parse(argsJson.empty() ? "{}" : argsJson);
        Mem::OpenProcessRequest request;
        request.pid = requiredPositiveInt(args, "pid");
        request.name = optionalString(args, "name");

        const Mem::OperationContext context = service_.captureContext(false);
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

std::string AgentMemTools::memoryRead(const std::string& argsJson,
                                      bool allowLegacyAddress) {
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

        const Mem::OperationContext context = service_.captureContext(true);
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

} // namespace AI
