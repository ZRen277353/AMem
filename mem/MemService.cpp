#include "MemService.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <limits>
#include <utility>

namespace Mem {

namespace {

using Clock = std::chrono::steady_clock;

uint64_t elapsedMilliseconds(Clock::time_point start) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - start).count();
    return elapsed > 0 ? static_cast<uint64_t>(elapsed) : 0;
}

template <typename T>
Result<T> failureFrom(const Error& error, Clock::time_point start) {
    return Result<T>::failure(error, elapsedMilliseconds(start));
}

std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return value;
}

} // namespace

MemService::MemService(IMemBackend& backend) : backend_(backend) {}

OperationContext MemService::captureContext(bool includeTarget) const {
    OperationContext context;
    context.connectionGeneration = backend_.connectionGeneration();
    if (includeTarget) {
        context.target = backend_.targetSnapshot();
    }
    return context;
}

std::optional<Error> MemService::validateContext(
    const OperationContext& context,
    bool requireConnected,
    bool requireTarget,
    bool checkCancellation) const {
    if (checkCancellation && context.cancellation &&
        context.cancellation->load(std::memory_order_acquire)) {
        return Error{ErrorCode::CancelRequested,
                     "operation was cancelled before completion", false};
    }

    if (checkCancellation && Clock::now() >= context.deadline) {
        return Error{ErrorCode::Timeout,
                     "operation deadline has expired", true};
    }

    const uint64_t currentGeneration = backend_.connectionGeneration();
    if (currentGeneration != context.connectionGeneration) {
        return Error{ErrorCode::ConnectionChanged,
                     "connection changed while the operation was pending",
                     true};
    }

    if (requireConnected && backend_.isConnectionPoisoned()) {
        return Error{ErrorCode::ConnectionPoisoned,
                     "connection is poisoned and must be reconnected", true};
    }

    if (requireConnected && !backend_.isConnected()) {
        return Error{ErrorCode::NotConnected,
                     "AMem is not connected to the Android server", true};
    }

    if (requireTarget) {
        if (!context.target || !context.target->isAttached()) {
            return Error{ErrorCode::NoTarget,
                         "no target process is attached", false};
        }
        if (context.target->connectionGeneration !=
            context.connectionGeneration) {
            return Error{ErrorCode::ConnectionChanged,
                         "target belongs to a different connection generation",
                         true};
        }

        const TargetSnapshot currentTarget = backend_.targetSnapshot();
        if (currentTarget != *context.target) {
            return Error{ErrorCode::TargetChanged,
                         "target process changed while the operation was pending",
                         false};
        }
    }

    return std::nullopt;
}

Result<Status> MemService::status(const OperationContext& context) {
    const auto start = Clock::now();
    if (const auto error = validateContext(context, false, false, true)) {
        return failureFrom<Status>(*error, start);
    }

    Status value;
    value.connected = backend_.isConnected();
    value.connectionPoisoned = backend_.isConnectionPoisoned();
    value.connectionGeneration = backend_.connectionGeneration();
    value.target = backend_.targetSnapshot();
    value.processName = backend_.processName();
    if (backend_.targetSnapshot() != value.target) {
        return Result<Status>::failure(
            ErrorCode::TargetChanged,
            "target process changed while status was being collected",
            false,
            elapsedMilliseconds(start));
    }

    if (value.connected) {
        int version = 0;
        std::string versionString;
        if (backend_.fetchServerVersion(version, versionString)) {
            value.serverVersion = version;
            value.serverVersionString = std::move(versionString);
        }

        int architectureType = 0;
        std::string architectureName;
        if (backend_.fetchArchitecture(architectureType, architectureName)) {
            value.architectureType = architectureType;
            value.architectureName = std::move(architectureName);
        }
    }

    if (const auto error = validateContext(context, false, false, true)) {
        return failureFrom<Status>(*error, start);
    }
    if (backend_.targetSnapshot() != value.target) {
        return Result<Status>::failure(
            ErrorCode::TargetChanged,
            "target process changed while status was being collected",
            false,
            elapsedMilliseconds(start));
    }
    return Result<Status>::success(std::move(value), elapsedMilliseconds(start));
}

Result<ProcessPage> MemService::listProcesses(
    const OperationContext& context,
    const ProcessListRequest& request) {
    const auto start = Clock::now();
    if (request.limit == 0 || request.limit > kMaxProcessPageSize) {
        return Result<ProcessPage>::failure(
            ErrorCode::InvalidArgument,
            "process page limit must be between 1 and 1000",
            false,
            elapsedMilliseconds(start));
    }
    if (request.filter.size() > kMaxTextParameterBytes) {
        return Result<ProcessPage>::failure(
            ErrorCode::InvalidArgument,
            "process filter exceeds 4096 bytes",
            false,
            elapsedMilliseconds(start));
    }
    if (const auto error = validateContext(context, true, false, true)) {
        return failureFrom<ProcessPage>(*error, start);
    }

    std::vector<ProcessInfo> allProcesses;
    if (!backend_.fetchProcesses(allProcesses)) {
        if (const auto error = validateContext(context, true, false, true)) {
            return failureFrom<ProcessPage>(*error, start);
        }
        return Result<ProcessPage>::failure(
            ErrorCode::ProtocolError,
            "failed to fetch the process list from the Android server",
            true,
            elapsedMilliseconds(start));
    }

    if (const auto error = validateContext(context, true, false, true)) {
        return failureFrom<ProcessPage>(*error, start);
    }

    const std::string filter = lowerAscii(request.filter);
    std::vector<ProcessInfo> filtered;
    filtered.reserve(allProcesses.size());
    for (auto& process : allProcesses) {
        if (filter.empty() ||
            lowerAscii(process.name).find(filter) != std::string::npos) {
            filtered.push_back(std::move(process));
        }
    }

    ProcessPage page;
    page.total = filtered.size();
    page.offset = std::min(request.offset, page.total);
    const size_t end = page.offset +
                       std::min(request.limit, page.total - page.offset);
    page.items.reserve(end - page.offset);
    for (size_t i = page.offset; i < end; ++i) {
        page.items.push_back(std::move(filtered[i]));
    }
    if (end < page.total) {
        page.nextOffset = end;
    }
    return Result<ProcessPage>::success(std::move(page),
                                        elapsedMilliseconds(start));
}

Result<OpenProcessResult> MemService::openProcess(
    const OperationContext& context,
    const OpenProcessRequest& request) {
    const auto start = Clock::now();
    if (request.pid <= 0) {
        return Result<OpenProcessResult>::failure(
            ErrorCode::InvalidArgument,
            "pid must be a positive integer",
            false,
            elapsedMilliseconds(start));
    }
    if (request.name.size() > kMaxTextParameterBytes) {
        return Result<OpenProcessResult>::failure(
            ErrorCode::InvalidArgument,
            "process name exceeds 4096 bytes",
            false,
            elapsedMilliseconds(start));
    }
    if (const auto error = validateContext(context, true, false, true)) {
        return failureFrom<OpenProcessResult>(*error, start);
    }

    std::string resolvedName = request.name;
    if (resolvedName.empty()) {
        std::vector<ProcessInfo> processes;
        if (backend_.fetchProcesses(processes)) {
            for (const auto& process : processes) {
                if (process.pid == request.pid) {
                    resolvedName = process.name;
                    break;
                }
            }
        }
    }

    if (const auto error = validateContext(context, true, false, true)) {
        return failureFrom<OpenProcessResult>(*error, start);
    }
    if (!backend_.openProcess(request.pid, resolvedName)) {
        if (const auto error = validateContext(context, true, false, false)) {
            return failureFrom<OpenProcessResult>(*error, start);
        }
        return Result<OpenProcessResult>::failure(
            ErrorCode::ProtocolError,
            "failed to open the requested process",
            false,
            elapsedMilliseconds(start));
    }

    if (const auto error = validateContext(context, true, false, false)) {
        return failureFrom<OpenProcessResult>(*error, start);
    }
    const TargetSnapshot target = backend_.targetSnapshot();
    if (!target.isAttached() || target.pid != request.pid ||
        target.connectionGeneration != context.connectionGeneration) {
        return Result<OpenProcessResult>::failure(
            ErrorCode::TargetChanged,
            "opened process does not match the requested target",
            false,
            elapsedMilliseconds(start));
    }
    OpenProcessResult result;
    result.target = target;
    result.name = backend_.processName();
    if (backend_.targetSnapshot() != target) {
        return Result<OpenProcessResult>::failure(
            ErrorCode::TargetChanged,
            "target process changed while open result was being collected",
            false,
            elapsedMilliseconds(start));
    }
    return Result<OpenProcessResult>::success(
        std::move(result), elapsedMilliseconds(start));
}

Result<MemoryBlock> MemService::readMemory(
    const OperationContext& context,
    const MemoryReadRequest& request) {
    const auto start = Clock::now();
    if (request.size == 0 || request.size > kMaxAgentMemoryReadBytes) {
        return Result<MemoryBlock>::failure(
            ErrorCode::InvalidArgument,
            "memory read size must be between 1 and 65536 bytes",
            false,
            elapsedMilliseconds(start));
    }
    if (request.address >
        (std::numeric_limits<uint64_t>::max)() - (request.size - 1u)) {
        return Result<MemoryBlock>::failure(
            ErrorCode::InvalidArgument,
            "memory read range overflows the uint64 address space",
            false,
            elapsedMilliseconds(start));
    }
    if (const auto error = validateContext(context, true, true, true)) {
        return failureFrom<MemoryBlock>(*error, start);
    }

    std::vector<unsigned char> bytes;
    if (!backend_.readMemory(request.address, request.size, bytes)) {
        if (const auto error = validateContext(context, true, true, true)) {
            return failureFrom<MemoryBlock>(*error, start);
        }
        return Result<MemoryBlock>::failure(
            ErrorCode::ProtocolError,
            "failed to read target process memory",
            true,
            elapsedMilliseconds(start));
    }

    if (const auto error = validateContext(context, true, true, true)) {
        return failureFrom<MemoryBlock>(*error, start);
    }
    if (bytes.empty() || bytes.size() > request.size) {
        return Result<MemoryBlock>::failure(
            ErrorCode::ProtocolError,
            "memory read returned an invalid byte count",
            false,
            elapsedMilliseconds(start));
    }

    MemoryBlock block;
    block.address = request.address;
    block.bytes = std::move(bytes);
    block.target = *context.target;
    return Result<MemoryBlock>::success(std::move(block),
                                        elapsedMilliseconds(start));
}

} // namespace Mem
