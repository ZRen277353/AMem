#include "MemService.h"
#include "ValueCodec.h"

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

std::string moduleBaseName(const std::string& name) {
    const size_t separator = name.find_last_of("/\\");
    return separator == std::string::npos
        ? name
        : name.substr(separator + 1);
}

std::optional<Error> validateModules(
    const std::vector<ModuleInfo>& modules) {
    if (modules.size() > kMaxModuleResultCount) {
        return Error{ErrorCode::ProtocolError,
                     "module list exceeds the maximum item count", false};
    }

    size_t totalNameBytes = 0;
    for (const auto& module : modules) {
        if (module.size == 0 ||
            module.base >
                (std::numeric_limits<uint64_t>::max)() - module.size) {
            return Error{ErrorCode::ProtocolError,
                         "module list contains an invalid address range",
                         false};
        }
        if (module.name.size() > kMaxTextParameterBytes) {
            return Error{ErrorCode::ProtocolError,
                         "module list contains an overlong name", false};
        }
        if (totalNameBytes >
            kMaxModuleNameBytesTotal - module.name.size()) {
            return Error{ErrorCode::ProtocolError,
                         "module list exceeds the total name-size limit",
                         false};
        }
        totalNameBytes += module.name.size();
    }
    return std::nullopt;
}

std::optional<Error> findResolvedModule(
    const std::vector<ModuleInfo>& modules,
    const std::string& normalizedQuery,
    const ModuleInfo*& resolved) {
    std::vector<const ModuleInfo*> fullMatches;
    std::vector<const ModuleInfo*> baseNameMatches;
    std::vector<const ModuleInfo*> substringMatches;
    for (const auto& module : modules) {
        const std::string fullName = lowerAscii(module.name);
        if (fullName == normalizedQuery) {
            fullMatches.push_back(&module);
        }
        if (lowerAscii(moduleBaseName(module.name)) == normalizedQuery) {
            baseNameMatches.push_back(&module);
        }
        if (fullName.find(normalizedQuery) != std::string::npos) {
            substringMatches.push_back(&module);
        }
    }

    const std::vector<const ModuleInfo*>* matches = nullptr;
    if (!fullMatches.empty()) {
        matches = &fullMatches;
    } else if (!baseNameMatches.empty()) {
        matches = &baseNameMatches;
    } else {
        matches = &substringMatches;
    }
    if (matches->empty()) {
        return Error{ErrorCode::InvalidArgument,
                     "module_name did not match a loaded module", false};
    }
    if (matches->size() != 1) {
        return Error{
            ErrorCode::InvalidArgument,
            "module_name is ambiguous; use module_list with a narrower filter",
            false};
    }
    resolved = matches->front();
    return std::nullopt;
}

bool addAddressOffset(uint64_t base, uint64_t offset, uint64_t& result) {
    if (base > (std::numeric_limits<uint64_t>::max)() - offset) {
        return false;
    }
    result = base + offset;
    return true;
}

const char* breakpointActionName(BreakpointAction action) {
    switch (action) {
        case BreakpointAction::Set:     return "set";
        case BreakpointAction::Remove:  return "remove";
        case BreakpointAction::Suspend: return "suspend";
        case BreakpointAction::Resume:  return "resume";
        default:                        return "mutate";
    }
}

bool isValidBreakpointAccess(BreakpointAccess access) {
    switch (access) {
        case BreakpointAccess::Read:
        case BreakpointAccess::Write:
        case BreakpointAccess::ReadWrite:
        case BreakpointAccess::Execute:
            return true;
        default:
            return false;
    }
}

bool isValidBreakpointSize(uint32_t size) {
    return size == 1 || size == 2 || size == 4 || size == 8;
}

std::optional<uint64_t> decodePointer(
    const std::vector<unsigned char>& bytes) {
    if (bytes.size() != sizeof(uint64_t)) {
        return std::nullopt;
    }
    uint64_t value = 0;
    for (size_t i = 0; i < sizeof(uint64_t); ++i) {
        value |= static_cast<uint64_t>(bytes[i]) << (i * 8u);
    }
    return value;
}

size_t scanDataTypeSize(ScanDataType type) {
    switch (type) {
        case ScanDataType::Byte:   return 1;
        case ScanDataType::Word:   return 2;
        case ScanDataType::Dword:
        case ScanDataType::Xor:
        case ScanDataType::Float:  return 4;
        case ScanDataType::Qword:
        case ScanDataType::Double: return 8;
        case ScanDataType::Bytes:  return 0;
        default:                   return 0;
    }
}

bool isValidScanMemoryRegion(ScanMemoryRegion region) {
    switch (region) {
        case ScanMemoryRegion::All:
        case ScanMemoryRegion::Anonymous:
        case ScanMemoryRegion::CAlloc:
        case ScanMemoryRegion::CHeap:
        case ScanMemoryRegion::CData:
        case ScanMemoryRegion::CBss:
        case ScanMemoryRegion::JavaHeap:
        case ScanMemoryRegion::Java:
        case ScanMemoryRegion::Stack:
        case ScanMemoryRegion::Video:
        case ScanMemoryRegion::CodeApp:
        case ScanMemoryRegion::CodeSystem:
        case ScanMemoryRegion::Ashmem:
        case ScanMemoryRegion::Bad:
        case ScanMemoryRegion::Other:
            return true;
        default:
            return false;
    }
}

bool isValueStartMode(ScanMode mode) {
    return mode == ScanMode::Exact || mode == ScanMode::Greater ||
           mode == ScanMode::Less || mode == ScanMode::Between;
}

bool isRefineMode(ScanMode mode) {
    return mode == ScanMode::Exact || mode == ScanMode::Greater ||
           mode == ScanMode::Less || mode == ScanMode::Between ||
           mode == ScanMode::Increased || mode == ScanMode::IncreasedBy ||
           mode == ScanMode::Decreased || mode == ScanMode::DecreasedBy ||
           mode == ScanMode::Changed || mode == ScanMode::Unchanged;
}

bool scanModeRequiresValue(ScanMode mode) {
    return mode == ScanMode::Exact || mode == ScanMode::Greater ||
           mode == ScanMode::Less || mode == ScanMode::Between ||
           mode == ScanMode::IncreasedBy ||
           mode == ScanMode::DecreasedBy;
}

std::optional<Error> validateScanValue(
    ScanDataType dataType,
    ScanMode mode,
    const std::vector<unsigned char>& value,
    bool allowEmptyForValuelessMode) {
    const size_t scalarSize = scanDataTypeSize(dataType);
    if (scalarSize == 0) {
        return Error{ErrorCode::InvalidArgument,
                     "scan data_type must be a scalar type", false};
    }
    if (!scanModeRequiresValue(mode)) {
        if (!allowEmptyForValuelessMode || !value.empty()) {
            return Error{ErrorCode::InvalidArgument,
                         "scan mode does not accept a comparison value",
                         false};
        }
        return std::nullopt;
    }
    const size_t expected =
        mode == ScanMode::Between ? scalarSize * 2u : scalarSize;
    if (value.size() != expected) {
        return Error{
            ErrorCode::InvalidArgument,
            mode == ScanMode::Between
                ? "between scan requires exactly two encoded scalar values"
                : "scan value byte count does not match data_type",
            false};
    }
    return std::nullopt;
}

std::optional<Error> validateScanStartRequest(
    const ScanStartRequest& request) {
    if (request.start > request.end) {
        return Error{ErrorCode::InvalidArgument,
                     "scan start must be less than or equal to end", false};
    }
    if (!isValidScanMemoryRegion(request.memoryRegion)) {
        return Error{ErrorCode::InvalidArgument,
                     "scan memory region is unsupported", false};
    }
    if (request.value.size() > kMaxScanValueBytes) {
        return Error{ErrorCode::InvalidArgument,
                     "scan value exceeds 4096 bytes", false};
    }
    if (request.kind == ScanStartKind::BytePattern) {
        if (request.dataType != ScanDataType::Bytes ||
            request.mode != ScanMode::Exact || request.value.empty()) {
            return Error{
                ErrorCode::InvalidArgument,
                "byte-pattern scans require exact mode and non-empty pattern bytes",
                false};
        }
        return std::nullopt;
    }
    if (request.kind == ScanStartKind::Unknown) {
        if (request.dataType == ScanDataType::Bytes ||
            request.mode != ScanMode::Unknown || !request.value.empty()) {
            return Error{
                ErrorCode::InvalidArgument,
                "unknown scans require a scalar data_type and no value",
                false};
        }
        return std::nullopt;
    }
    if (!isValueStartMode(request.mode) ||
        request.dataType == ScanDataType::Bytes) {
        return Error{ErrorCode::InvalidArgument,
                     "value scan start mode or data_type is unsupported",
                     false};
    }
    return validateScanValue(
        request.dataType, request.mode, request.value, false);
}

Error noScanSessionError() {
    return Error{ErrorCode::NoScanSession,
                 "no active native scan session; start a scan first", false};
}

Error scanSessionChangedError() {
    return Error{
        ErrorCode::ScanSessionChanged,
        "scan session changed or was replaced by another front end; start a new scan",
        false};
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
    const auto validateSelectionTarget = [&]() -> std::optional<Error> {
        if (!context.target) {
            return std::nullopt;
        }
        if (context.target->connectionGeneration !=
            context.connectionGeneration) {
            return Error{
                ErrorCode::ConnectionChanged,
                "target selection belongs to a different connection generation",
                true};
        }
        if (backend_.targetSnapshot() != *context.target) {
            return Error{
                ErrorCode::TargetChanged,
                "target process changed before process open was sent",
                false};
        }
        return std::nullopt;
    };

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
    if (const auto error = validateSelectionTarget()) {
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
    if (const auto error = validateSelectionTarget()) {
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

Result<ModulePage> MemService::listModules(
    const OperationContext& context,
    const ModuleListRequest& request) {
    const auto start = Clock::now();
    if (request.limit == 0 || request.limit > kMaxModulePageSize) {
        return Result<ModulePage>::failure(
            ErrorCode::InvalidArgument,
            "module page limit must be between 1 and 1000",
            false,
            elapsedMilliseconds(start));
    }
    if (request.filter.size() > kMaxTextParameterBytes) {
        return Result<ModulePage>::failure(
            ErrorCode::InvalidArgument,
            "module filter exceeds 4096 bytes",
            false,
            elapsedMilliseconds(start));
    }
    if (const auto error = validateContext(context, true, true, true)) {
        return failureFrom<ModulePage>(*error, start);
    }

    std::vector<ModuleInfo> allModules;
    if (!backend_.fetchModules(allModules)) {
        if (const auto error = validateContext(context, true, true, true)) {
            return failureFrom<ModulePage>(*error, start);
        }
        return Result<ModulePage>::failure(
            ErrorCode::ProtocolError,
            "failed to fetch the module list from the Android server",
            true,
            elapsedMilliseconds(start));
    }
    if (const auto error = validateContext(context, true, true, true)) {
        return failureFrom<ModulePage>(*error, start);
    }
    if (const auto error = validateModules(allModules)) {
        return failureFrom<ModulePage>(*error, start);
    }

    const std::string filter = lowerAscii(request.filter);
    std::vector<ModuleInfo> filtered;
    filtered.reserve(allModules.size());
    for (auto& module : allModules) {
        if (filter.empty() ||
            lowerAscii(module.name).find(filter) != std::string::npos) {
            filtered.push_back(std::move(module));
        }
    }

    ModulePage page;
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
    page.target = *context.target;
    return Result<ModulePage>::success(
        std::move(page), elapsedMilliseconds(start));
}

Result<ResolvedModule> MemService::resolveModule(
    const OperationContext& context,
    const ModuleResolveRequest& request) {
    const auto start = Clock::now();
    if (request.name.size() > kMaxTextParameterBytes) {
        return Result<ResolvedModule>::failure(
            ErrorCode::InvalidArgument,
            "module name exceeds 4096 bytes",
            false,
            elapsedMilliseconds(start));
    }
    const std::string query = lowerAscii(trimAscii(request.name));
    if (query.empty()) {
        return Result<ResolvedModule>::failure(
            ErrorCode::InvalidArgument,
            "module_name must not be empty",
            false,
            elapsedMilliseconds(start));
    }
    if (const auto error = validateContext(context, true, true, true)) {
        return failureFrom<ResolvedModule>(*error, start);
    }

    std::vector<ModuleInfo> modules;
    if (!backend_.fetchModules(modules)) {
        if (const auto error = validateContext(context, true, true, true)) {
            return failureFrom<ResolvedModule>(*error, start);
        }
        return Result<ResolvedModule>::failure(
            ErrorCode::ProtocolError,
            "failed to fetch modules while resolving module_name",
            true,
            elapsedMilliseconds(start));
    }
    if (const auto error = validateContext(context, true, true, true)) {
        return failureFrom<ResolvedModule>(*error, start);
    }
    if (const auto error = validateModules(modules)) {
        return failureFrom<ResolvedModule>(*error, start);
    }

    const ModuleInfo* matched = nullptr;
    if (const auto error = findResolvedModule(modules, query, matched)) {
        return failureFrom<ResolvedModule>(*error, start);
    }

    ResolvedModule result;
    result.module = *matched;
    result.target = *context.target;
    return Result<ResolvedModule>::success(
        std::move(result), elapsedMilliseconds(start));
}

Result<PointerResolution> MemService::resolvePointer(
    const OperationContext& context,
    const PointerResolveRequest& request) {
    const auto start = Clock::now();
    if (request.moduleName.size() > kMaxTextParameterBytes) {
        return Result<PointerResolution>::failure(
            ErrorCode::InvalidArgument,
            "module_name exceeds 4096 bytes",
            false,
            elapsedMilliseconds(start));
    }
    const std::string query = lowerAscii(trimAscii(request.moduleName));
    if (query.empty()) {
        return Result<PointerResolution>::failure(
            ErrorCode::InvalidArgument,
            "module_name must not be empty",
            false,
            elapsedMilliseconds(start));
    }
    if (request.offsets.size() > kMaxPointerOffsetCount) {
        return Result<PointerResolution>::failure(
            ErrorCode::InvalidArgument,
            "pointer offset chain exceeds 1024 entries",
            false,
            elapsedMilliseconds(start));
    }
    if (const auto error = validateContext(context, true, true, true)) {
        return failureFrom<PointerResolution>(*error, start);
    }

    auto transaction = backend_.beginReadTransaction(context);
    if (!transaction) {
        if (const auto error = validateContext(context, true, true, true)) {
            return failureFrom<PointerResolution>(*error, start);
        }
        return Result<PointerResolution>::failure(
            ErrorCode::ProtocolError,
            "failed to acquire the pointer read transaction",
            true,
            elapsedMilliseconds(start));
    }

    Error operationError;
    bool failed = false;
    std::vector<ModuleInfo> modules;
    if (!transaction->fetchModules(modules)) {
        operationError = Error{
            ErrorCode::ProtocolError,
            "failed to fetch modules inside the pointer transaction", true};
        failed = true;
    }
    if (!failed) {
        if (const auto error = validateContext(context, true, false, true)) {
            operationError = *error;
            failed = true;
        } else if (const auto error = validateModules(modules)) {
            operationError = *error;
            failed = true;
        }
    }

    const ModuleInfo* matched = nullptr;
    if (!failed) {
        if (const auto error = findResolvedModule(modules, query, matched)) {
            operationError = *error;
            failed = true;
        }
    }

    PointerResolution resolution;
    if (!failed) {
        resolution.module = *matched;
        resolution.baseOffset = request.baseOffset;
        if (!addAddressOffset(resolution.module.base,
                              request.baseOffset,
                              resolution.startAddress)) {
            operationError = Error{
                ErrorCode::InvalidArgument,
                "module base plus base_offset overflows the uint64 address space",
                false};
            failed = true;
        } else {
            resolution.address = resolution.startAddress;
        }
    }

    auto dereference = [&](uint64_t readAddress,
                           uint64_t offset,
                           bool addOffset) -> bool {
        if (const auto error = validateContext(context, true, false, true)) {
            operationError = *error;
            return false;
        }

        std::vector<unsigned char> bytes;
        if (!transaction->readMemory(readAddress, sizeof(uint64_t), bytes)) {
            operationError = Error{
                ErrorCode::ProtocolError,
                "failed to read a pointer inside the pointer transaction",
                true};
            return false;
        }
        const auto pointer = decodePointer(bytes);
        if (!pointer) {
            operationError = Error{
                ErrorCode::ProtocolError,
                "pointer read returned an invalid byte count", false};
            return false;
        }

        uint64_t next = *pointer;
        if (addOffset && !addAddressOffset(*pointer, offset, next)) {
            operationError = Error{
                ErrorCode::InvalidArgument,
                "pointer value plus offset overflows the uint64 address space",
                false};
            return false;
        }
        resolution.address = next;
        ++resolution.dereferenceCount;
        return true;
    };

    if (!failed) {
        for (uint64_t offset : request.offsets) {
            if (!dereference(resolution.address, offset, true)) {
                failed = true;
                break;
            }
        }
    }
    if (!failed && request.dereferenceFinal) {
        if (!dereference(resolution.address, 0, false)) {
            failed = true;
        } else {
            resolution.dereferencedFinal = true;
        }
    }

    transaction.reset();
    if (const auto error = validateContext(context, true, true, true)) {
        return failureFrom<PointerResolution>(*error, start);
    }
    if (failed) {
        return failureFrom<PointerResolution>(operationError, start);
    }

    resolution.target = *context.target;
    return Result<PointerResolution>::success(
        std::move(resolution), elapsedMilliseconds(start));
}

Result<ResolvedSymbol> MemService::resolveSymbol(
    const OperationContext& context,
    const SymbolResolveRequest& request) {
    const auto start = Clock::now();
    if (request.moduleName.size() > kMaxTextParameterBytes ||
        request.symbolName.size() > kMaxTextParameterBytes) {
        return Result<ResolvedSymbol>::failure(
            ErrorCode::InvalidArgument,
            "module_name and symbol_name must not exceed 4096 bytes",
            false,
            elapsedMilliseconds(start));
    }
    const std::string moduleQuery =
        lowerAscii(trimAscii(request.moduleName));
    const std::string symbolName = trimAscii(request.symbolName);
    if (moduleQuery.empty() || symbolName.empty()) {
        return Result<ResolvedSymbol>::failure(
            ErrorCode::InvalidArgument,
            "module_name and symbol_name must not be empty",
            false,
            elapsedMilliseconds(start));
    }

    std::lock_guard<std::mutex> symbolLock(symbolMutex_);
    if (const auto error = validateContext(context, true, true, true)) {
        return failureFrom<ResolvedSymbol>(*error, start);
    }
    auto transaction = backend_.beginSymbolTransaction(context);
    if (!transaction) {
        if (const auto error = validateContext(context, true, true, true)) {
            return failureFrom<ResolvedSymbol>(*error, start);
        }
        return Result<ResolvedSymbol>::failure(
            ErrorCode::ProtocolError,
            "failed to acquire the symbol transaction",
            true,
            elapsedMilliseconds(start));
    }

    std::vector<ModuleInfo> modules;
    if (!transaction->fetchModules(modules)) {
        return Result<ResolvedSymbol>::failure(
            ErrorCode::ProtocolError,
            "failed to fetch modules inside the symbol transaction",
            true,
            elapsedMilliseconds(start));
    }
    if (const auto error = validateContext(context, true, false, true)) {
        return failureFrom<ResolvedSymbol>(*error, start);
    }
    if (const auto error = validateModules(modules)) {
        return failureFrom<ResolvedSymbol>(*error, start);
    }
    const ModuleInfo* matched = nullptr;
    if (const auto error = findResolvedModule(modules, moduleQuery, matched)) {
        return failureFrom<ResolvedSymbol>(*error, start);
    }

    int totalCount = 0;
    if (!transaction->initializeSymbols(matched->base, totalCount)) {
        return Result<ResolvedSymbol>::failure(
            ErrorCode::ProtocolError,
            "failed to initialize the module symbol table",
            true,
            elapsedMilliseconds(start));
    }
    const uint64_t completedEpoch = transaction->symbolEpoch();
    if (totalCount < 0 ||
        static_cast<size_t>(totalCount) > kMaxSymbolResultCount) {
        return Result<ResolvedSymbol>::failure(
            ErrorCode::ProtocolError,
            "symbol table returned an invalid total count",
            false,
            elapsedMilliseconds(start));
    }
    if (const auto error = validateContext(context, true, false, true)) {
        return failureFrom<ResolvedSymbol>(*error, start);
    }

    uint64_t address = 0;
    if (!transaction->findSymbol(matched->base, symbolName, address) ||
        address == 0) {
        return Result<ResolvedSymbol>::failure(
            ErrorCode::InvalidArgument,
            "symbol_name did not resolve inside the selected module",
            false,
            elapsedMilliseconds(start));
    }

    ResolvedSymbol resolved;
    resolved.name = symbolName;
    resolved.address = address;
    resolved.session.epoch = completedEpoch;
    resolved.session.module = *matched;
    resolved.session.total = static_cast<size_t>(totalCount);
    resolved.session.target = *context.target;

    transaction.reset();
    if (const auto error = validateContext(context, true, true, true)) {
        return failureFrom<ResolvedSymbol>(*error, start);
    }
    if (backend_.symbolEpoch() != completedEpoch) {
        return Result<ResolvedSymbol>::failure(
            ErrorCode::SymbolSessionChanged,
            "symbol table changed before the resolve result was committed",
            false,
            elapsedMilliseconds(start));
    }
    return Result<ResolvedSymbol>::success(
        std::move(resolved), elapsedMilliseconds(start));
}

Result<SymbolPage> MemService::listSymbols(
    const OperationContext& context,
    const SymbolListRequest& request) {
    const auto start = Clock::now();
    if (request.moduleName.size() > kMaxTextParameterBytes) {
        return Result<SymbolPage>::failure(
            ErrorCode::InvalidArgument,
            "module_name exceeds 4096 bytes",
            false,
            elapsedMilliseconds(start));
    }
    const std::string moduleQuery =
        lowerAscii(trimAscii(request.moduleName));
    if (moduleQuery.empty()) {
        return Result<SymbolPage>::failure(
            ErrorCode::InvalidArgument,
            "module_name must not be empty",
            false,
            elapsedMilliseconds(start));
    }
    if (request.limit == 0 || request.limit > kMaxSymbolPageSize ||
        request.offset > static_cast<size_t>((std::numeric_limits<int>::max)())) {
        return Result<SymbolPage>::failure(
            ErrorCode::InvalidArgument,
            "symbol page must use offset <= INT_MAX and count from 1 to 1000",
            false,
            elapsedMilliseconds(start));
    }
    if (request.offset > 0 && !request.expectedEpoch) {
        return Result<SymbolPage>::failure(
            ErrorCode::InvalidArgument,
            "symbol_epoch is required when requesting a continuation page",
            false,
            elapsedMilliseconds(start));
    }

    std::lock_guard<std::mutex> symbolLock(symbolMutex_);
    if (const auto error = validateContext(context, true, true, true)) {
        return failureFrom<SymbolPage>(*error, start);
    }
    auto transaction = backend_.beginSymbolTransaction(context);
    if (!transaction) {
        if (const auto error = validateContext(context, true, true, true)) {
            return failureFrom<SymbolPage>(*error, start);
        }
        return Result<SymbolPage>::failure(
            ErrorCode::ProtocolError,
            "failed to acquire the symbol transaction",
            true,
            elapsedMilliseconds(start));
    }
    if (request.expectedEpoch &&
        transaction->symbolEpoch() != *request.expectedEpoch) {
        return Result<SymbolPage>::failure(
            ErrorCode::SymbolSessionChanged,
            "symbol table changed before the requested continuation page",
            false,
            elapsedMilliseconds(start));
    }

    std::vector<ModuleInfo> modules;
    if (!transaction->fetchModules(modules)) {
        return Result<SymbolPage>::failure(
            ErrorCode::ProtocolError,
            "failed to fetch modules inside the symbol transaction",
            true,
            elapsedMilliseconds(start));
    }
    if (const auto error = validateContext(context, true, false, true)) {
        return failureFrom<SymbolPage>(*error, start);
    }
    if (const auto error = validateModules(modules)) {
        return failureFrom<SymbolPage>(*error, start);
    }
    const ModuleInfo* matched = nullptr;
    if (const auto error = findResolvedModule(modules, moduleQuery, matched)) {
        return failureFrom<SymbolPage>(*error, start);
    }

    int initializedTotal = 0;
    if (!transaction->initializeSymbols(matched->base, initializedTotal)) {
        return Result<SymbolPage>::failure(
            ErrorCode::ProtocolError,
            "failed to initialize the module symbol table",
            true,
            elapsedMilliseconds(start));
    }
    const uint64_t completedEpoch = transaction->symbolEpoch();
    if (initializedTotal < 0 ||
        static_cast<size_t>(initializedTotal) > kMaxSymbolResultCount) {
        return Result<SymbolPage>::failure(
            ErrorCode::ProtocolError,
            "symbol table returned an invalid total count",
            false,
            elapsedMilliseconds(start));
    }
    if (const auto error = validateContext(context, true, false, true)) {
        return failureFrom<SymbolPage>(*error, start);
    }

    const size_t total = static_cast<size_t>(initializedTotal);
    const size_t pageOffset = (std::min)(request.offset, total);
    std::vector<SymbolInfo> symbols;
    if (pageOffset < total) {
        int fetchedTotal = 0;
        if (!transaction->fetchSymbols(pageOffset,
                                       request.limit,
                                       symbols,
                                       fetchedTotal)) {
            return Result<SymbolPage>::failure(
                ErrorCode::ProtocolError,
                "failed to fetch the symbol page",
                true,
                elapsedMilliseconds(start));
        }
        if (fetchedTotal != initializedTotal ||
            symbols.size() > request.limit || symbols.empty()) {
            return Result<SymbolPage>::failure(
                ErrorCode::ProtocolError,
                "symbol page is inconsistent with the initialized table",
                false,
                elapsedMilliseconds(start));
        }
    }

    size_t totalNameBytes = 0;
    for (const auto& symbol : symbols) {
        if (symbol.name.size() > kMaxSymbolNameBytes ||
            totalNameBytes > kMaxSymbolPageNameBytes - symbol.name.size()) {
            return Result<SymbolPage>::failure(
                ErrorCode::ProtocolError,
                "symbol page exceeds the name-size limit",
                false,
                elapsedMilliseconds(start));
        }
        totalNameBytes += symbol.name.size();
    }

    SymbolPage page;
    page.items = std::move(symbols);
    page.total = total;
    page.offset = pageOffset;
    const size_t end = page.offset + page.items.size();
    if (end < page.total) {
        page.nextOffset = end;
    }
    page.session.epoch = completedEpoch;
    page.session.module = *matched;
    page.session.total = total;
    page.session.target = *context.target;

    transaction.reset();
    if (const auto error = validateContext(context, true, true, true)) {
        return failureFrom<SymbolPage>(*error, start);
    }
    if (backend_.symbolEpoch() != completedEpoch) {
        return Result<SymbolPage>::failure(
            ErrorCode::SymbolSessionChanged,
            "symbol table changed before the page result was committed",
            false,
            elapsedMilliseconds(start));
    }
    return Result<SymbolPage>::success(
        std::move(page), elapsedMilliseconds(start));
}

Result<BreakpointMutationReceipt> MemService::mutateBreakpoint(
    const OperationContext& context,
    uint64_t address,
    BreakpointAction action,
    const std::function<BreakpointMutationBackendResult()>& operation,
    std::optional<BreakpointAccess> access,
    std::optional<uint32_t> size) {
    const auto start = Clock::now();
    if (address == 0) {
        return Result<BreakpointMutationReceipt>::failure(
            ErrorCode::InvalidArgument,
            "breakpoint address must not be zero",
            false,
            elapsedMilliseconds(start));
    }

    std::lock_guard<std::mutex> breakpointLock(breakpointMutex_);
    if (const auto error = validateContext(context, true, true, true)) {
        return failureFrom<BreakpointMutationReceipt>(*error, start);
    }

    const BreakpointMutationBackendResult backendResult = operation();
    if (!backendResult.responseReceived) {
        if (backendResult.requestStarted) {
            return Result<BreakpointMutationReceipt>::failure(
                ErrorCode::CompletionUnknown,
                std::string("breakpoint ") + breakpointActionName(action) +
                    " was sent but its completion could not be confirmed; reconnect before continuing and do not retry automatically",
                false,
                elapsedMilliseconds(start));
        }
        if (const auto error = validateContext(context, true, true, true)) {
            return failureFrom<BreakpointMutationReceipt>(*error, start);
        }
        return Result<BreakpointMutationReceipt>::failure(
            ErrorCode::ProtocolError,
            std::string("breakpoint ") + breakpointActionName(action) +
                " could not be sent",
            true,
            elapsedMilliseconds(start));
    }
    if (!backendResult.applied) {
        return Result<BreakpointMutationReceipt>::failure(
            ErrorCode::ProtocolError,
            std::string("Android server rejected breakpoint ") +
                breakpointActionName(action),
            false,
            elapsedMilliseconds(start));
    }
    if (const auto error = validateContext(context, true, true, false)) {
        return Result<BreakpointMutationReceipt>::failure(
            ErrorCode::CompletionUnknown,
            std::string("server confirmed breakpoint ") +
                breakpointActionName(action) +
                ", but the original target context is no longer current: " +
                error->message,
            false,
            elapsedMilliseconds(start));
    }

    BreakpointMutationReceipt receipt;
    receipt.address = address;
    receipt.action = action;
    receipt.access = access;
    receipt.size = size;
    receipt.completedAfterCancelRequest =
        context.cancellation &&
        context.cancellation->load(std::memory_order_acquire);
    receipt.completedAfterDeadline = Clock::now() >= context.deadline;
    receipt.target = *context.target;
    return Result<BreakpointMutationReceipt>::success(
        std::move(receipt), elapsedMilliseconds(start));
}

Result<BreakpointMutationReceipt> MemService::setBreakpoint(
    const OperationContext& context,
    const BreakpointSetRequest& request) {
    if (!isValidBreakpointAccess(request.access)) {
        return Result<BreakpointMutationReceipt>::failure(
            ErrorCode::InvalidArgument,
            "breakpoint access must be read, write, read_write, or execute");
    }
    if (!isValidBreakpointSize(request.size)) {
        return Result<BreakpointMutationReceipt>::failure(
            ErrorCode::InvalidArgument,
            "breakpoint size must be 1, 2, 4, or 8 bytes");
    }
    if (request.access == BreakpointAccess::Execute && request.size != 4) {
        return Result<BreakpointMutationReceipt>::failure(
            ErrorCode::InvalidArgument,
            "execute breakpoints require size 4");
    }
    return mutateBreakpoint(
        context,
        request.address,
        BreakpointAction::Set,
        [&] {
            return backend_.setBreakpoint(
                request.address, request.access, request.size);
        },
        request.access,
        request.size);
}

Result<BreakpointMutationReceipt> MemService::removeBreakpoint(
    const OperationContext& context,
    const BreakpointAddressRequest& request) {
    return mutateBreakpoint(
        context,
        request.address,
        BreakpointAction::Remove,
        [&] { return backend_.removeBreakpoint(request.address); });
}

Result<BreakpointMutationReceipt> MemService::suspendBreakpoint(
    const OperationContext& context,
    const BreakpointAddressRequest& request) {
    return mutateBreakpoint(
        context,
        request.address,
        BreakpointAction::Suspend,
        [&] { return backend_.suspendBreakpoint(request.address); });
}

Result<BreakpointMutationReceipt> MemService::resumeBreakpoint(
    const OperationContext& context,
    const BreakpointAddressRequest& request) {
    return mutateBreakpoint(
        context,
        request.address,
        BreakpointAction::Resume,
        [&] { return backend_.resumeBreakpoint(request.address); });
}

Result<BreakpointHitPage> MemService::breakpointHits(
    const OperationContext& context,
    const BreakpointHitsRequest& request) {
    const auto start = Clock::now();
    if (request.address == 0) {
        return Result<BreakpointHitPage>::failure(
            ErrorCode::InvalidArgument,
            "breakpoint address must not be zero",
            false,
            elapsedMilliseconds(start));
    }
    if (request.limit == 0 || request.limit > kMaxBreakpointHitPageSize ||
        request.offset > kMaxBreakpointHitCount) {
        return Result<BreakpointHitPage>::failure(
            ErrorCode::InvalidArgument,
            "breakpoint hits require offset <= 100000 and count from 1 to 100",
            false,
            elapsedMilliseconds(start));
    }

    std::lock_guard<std::mutex> breakpointLock(breakpointMutex_);
    if (const auto error = validateContext(context, true, true, true)) {
        return failureFrom<BreakpointHitPage>(*error, start);
    }

    std::vector<BreakpointHit> hits;
    size_t total = 0;
    if (!backend_.fetchBreakpointHits(
            request.address, request.offset, request.limit, hits, total)) {
        if (const auto error = validateContext(context, true, true, true)) {
            return failureFrom<BreakpointHitPage>(*error, start);
        }
        return Result<BreakpointHitPage>::failure(
            ErrorCode::ProtocolError,
            "failed to read breakpoint hits from the Android server",
            true,
            elapsedMilliseconds(start));
    }
    if (const auto error = validateContext(context, true, true, true)) {
        return failureFrom<BreakpointHitPage>(*error, start);
    }
    if (total > kMaxBreakpointHitCount || hits.size() > request.limit) {
        return Result<BreakpointHitPage>::failure(
            ErrorCode::ProtocolError,
            "breakpoint hit response exceeds service limits",
            false,
            elapsedMilliseconds(start));
    }

    BreakpointHitPage page;
    page.address = request.address;
    page.items = std::move(hits);
    page.total = total;
    page.offset = (std::min)(request.offset, total);
    const size_t end = page.offset + page.items.size();
    if (end < page.total) {
        if (page.items.empty()) {
            return Result<BreakpointHitPage>::failure(
                ErrorCode::ProtocolError,
                "breakpoint hit page is empty before the reported end",
                false,
                elapsedMilliseconds(start));
        }
        page.nextOffset = end;
    }
    page.target = *context.target;
    return Result<BreakpointHitPage>::success(
        std::move(page), elapsedMilliseconds(start));
}

Result<ScanSummary> MemService::startScan(
    const OperationContext& context,
    const ScanStartRequest& request) {
    const auto start = Clock::now();
    if (const auto error = validateScanStartRequest(request)) {
        return failureFrom<ScanSummary>(*error, start);
    }

    std::lock_guard<std::mutex> scanLock(scanMutex_);
    if (const auto error = validateContext(context, true, true, true)) {
        return failureFrom<ScanSummary>(*error, start);
    }

    auto transaction = backend_.beginScanTransaction(context);
    if (!transaction) {
        if (const auto error = validateContext(context, true, true, true)) {
            return failureFrom<ScanSummary>(*error, start);
        }
        return Result<ScanSummary>::failure(
            ErrorCode::ProtocolError,
            "failed to acquire the scan transaction",
            true,
            elapsedMilliseconds(start));
    }

    if (!transaction->setRange(request.memoryRegion)) {
        scanSession_.reset();
        transaction.reset();
        if (const auto error = validateContext(context, true, true, true)) {
            return failureFrom<ScanSummary>(*error, start);
        }
        return Result<ScanSummary>::failure(
            ErrorCode::CompletionUnknown,
            "scan range command may have been sent but could not be confirmed; reconnect before retrying",
            false,
            elapsedMilliseconds(start));
    }

    if (const auto error = validateContext(context, true, false, true)) {
        scanSession_.reset();
        transaction.reset();
        return failureFrom<ScanSummary>(*error, start);
    }

    const ScanExecutionBackendResult backendResult =
        transaction->startScan(request);
    const uint64_t completedEpoch = transaction->scanEpoch();
    transaction.reset();

    if (!backendResult.responseReceived) {
        scanSession_.reset();
        if (backendResult.requestStarted) {
            return Result<ScanSummary>::failure(
                ErrorCode::CompletionUnknown,
                "scan was sent but its terminal result could not be confirmed; reconnect before retrying",
                false,
                elapsedMilliseconds(start));
        }
        if (const auto error = validateContext(context, true, true, true)) {
            return failureFrom<ScanSummary>(*error, start);
        }
        return Result<ScanSummary>::failure(
            ErrorCode::ProtocolError,
            "scan could not be sent",
            true,
            elapsedMilliseconds(start));
    }
    if (backendResult.resultCount < 0 ||
        static_cast<size_t>(backendResult.resultCount) >
            kMaxScanResultCount) {
        scanSession_.reset();
        return Result<ScanSummary>::failure(
            ErrorCode::ProtocolError,
            "scan returned an invalid result count",
            false,
            elapsedMilliseconds(start));
    }
    if (const auto error = validateContext(context, true, true, false)) {
        scanSession_.reset();
        return failureFrom<ScanSummary>(*error, start);
    }
    if (backend_.scanEpoch() != completedEpoch) {
        scanSession_.reset();
        return failureFrom<ScanSummary>(scanSessionChangedError(), start);
    }

    ScanSessionSnapshot session;
    session.epoch = completedEpoch;
    session.kind = request.kind;
    session.dataType = request.dataType;
    session.mode = request.mode;
    session.memoryRegion = request.memoryRegion;
    session.start = request.start;
    session.end = request.end;
    session.resultCount =
        static_cast<size_t>(backendResult.resultCount);
    session.target = *context.target;
    scanSession_ = session;

    ScanSummary summary;
    summary.session = std::move(session);
    summary.completedAfterCancelRequest = backendResult.cancelRequested;
    summary.completedAfterDeadline = Clock::now() >= context.deadline;
    return Result<ScanSummary>::success(
        std::move(summary), elapsedMilliseconds(start));
}

Result<ScanSummary> MemService::refineScan(
    const OperationContext& context,
    const ScanRefineRequest& request) {
    const auto start = Clock::now();
    if (!isRefineMode(request.mode)) {
        return Result<ScanSummary>::failure(
            ErrorCode::InvalidArgument,
            "scan refine mode is unsupported",
            false,
            elapsedMilliseconds(start));
    }
    if (request.value.size() > kMaxScanValueBytes) {
        return Result<ScanSummary>::failure(
            ErrorCode::InvalidArgument,
            "scan refine value exceeds 4096 bytes",
            false,
            elapsedMilliseconds(start));
    }

    std::lock_guard<std::mutex> scanLock(scanMutex_);
    if (!scanSession_) {
        return failureFrom<ScanSummary>(noScanSessionError(), start);
    }
    if (request.expectedEpoch &&
        *request.expectedEpoch != scanSession_->epoch) {
        return failureFrom<ScanSummary>(scanSessionChangedError(), start);
    }
    if (!context.target || scanSession_->target != *context.target) {
        scanSession_.reset();
        return failureFrom<ScanSummary>(scanSessionChangedError(), start);
    }
    if (scanSession_->kind == ScanStartKind::BytePattern) {
        return Result<ScanSummary>::failure(
            ErrorCode::InvalidArgument,
            "byte-pattern scan sessions cannot be refined as scalar values",
            false,
            elapsedMilliseconds(start));
    }
    if (request.dataType &&
        *request.dataType != scanSession_->dataType) {
        return Result<ScanSummary>::failure(
            ErrorCode::InvalidArgument,
            "scan refine data_type must match the active scan session",
            false,
            elapsedMilliseconds(start));
    }
    if (const auto error = validateScanValue(
            scanSession_->dataType,
            request.mode,
            request.value,
            true)) {
        return failureFrom<ScanSummary>(*error, start);
    }
    if (const auto error = validateContext(context, true, true, true)) {
        return failureFrom<ScanSummary>(*error, start);
    }

    auto transaction = backend_.beginScanTransaction(context);
    if (!transaction) {
        if (const auto error = validateContext(context, true, true, true)) {
            return failureFrom<ScanSummary>(*error, start);
        }
        return Result<ScanSummary>::failure(
            ErrorCode::ProtocolError,
            "failed to acquire the scan transaction",
            true,
            elapsedMilliseconds(start));
    }
    if (transaction->scanEpoch() != scanSession_->epoch) {
        scanSession_.reset();
        transaction.reset();
        return failureFrom<ScanSummary>(scanSessionChangedError(), start);
    }

    ScanRefineRequest normalized = request;
    if (!scanModeRequiresValue(normalized.mode)) {
        normalized.value.assign(
            scanDataTypeSize(scanSession_->dataType), 0);
    }
    const ScanExecutionBackendResult backendResult =
        transaction->refineScan(*scanSession_, normalized);
    const uint64_t completedEpoch = transaction->scanEpoch();
    transaction.reset();

    if (!backendResult.responseReceived) {
        scanSession_.reset();
        if (backendResult.requestStarted) {
            return Result<ScanSummary>::failure(
                ErrorCode::CompletionUnknown,
                "scan refine was sent but its terminal result could not be confirmed; reconnect before retrying",
                false,
                elapsedMilliseconds(start));
        }
        if (const auto error = validateContext(context, true, true, true)) {
            return failureFrom<ScanSummary>(*error, start);
        }
        return Result<ScanSummary>::failure(
            ErrorCode::ProtocolError,
            "scan refine could not be sent",
            true,
            elapsedMilliseconds(start));
    }
    if (backendResult.resultCount < 0 ||
        static_cast<size_t>(backendResult.resultCount) >
            kMaxScanResultCount) {
        scanSession_.reset();
        return Result<ScanSummary>::failure(
            ErrorCode::ProtocolError,
            "scan refine returned an invalid result count",
            false,
            elapsedMilliseconds(start));
    }
    if (const auto error = validateContext(context, true, true, false)) {
        scanSession_.reset();
        return failureFrom<ScanSummary>(*error, start);
    }
    if (backend_.scanEpoch() != completedEpoch) {
        scanSession_.reset();
        return failureFrom<ScanSummary>(scanSessionChangedError(), start);
    }

    scanSession_->epoch = completedEpoch;
    scanSession_->mode = request.mode;
    scanSession_->resultCount =
        static_cast<size_t>(backendResult.resultCount);

    ScanSummary summary;
    summary.session = *scanSession_;
    summary.completedAfterCancelRequest = backendResult.cancelRequested;
    summary.completedAfterDeadline = Clock::now() >= context.deadline;
    return Result<ScanSummary>::success(
        std::move(summary), elapsedMilliseconds(start));
}

Result<ScanResultPage> MemService::scanResults(
    const OperationContext& context,
    const ScanResultsRequest& request) {
    const auto start = Clock::now();
    if (request.limit == 0 || request.limit > kMaxScanResultPageSize) {
        return Result<ScanResultPage>::failure(
            ErrorCode::InvalidArgument,
            "scan result page limit must be between 1 and 1000",
            false,
            elapsedMilliseconds(start));
    }

    std::lock_guard<std::mutex> scanLock(scanMutex_);
    if (!scanSession_) {
        return failureFrom<ScanResultPage>(noScanSessionError(), start);
    }
    if (request.expectedEpoch &&
        *request.expectedEpoch != scanSession_->epoch) {
        return failureFrom<ScanResultPage>(scanSessionChangedError(), start);
    }
    if (!context.target || scanSession_->target != *context.target) {
        scanSession_.reset();
        return failureFrom<ScanResultPage>(scanSessionChangedError(), start);
    }
    if (const auto error = validateContext(context, true, true, true)) {
        return failureFrom<ScanResultPage>(*error, start);
    }

    auto transaction = backend_.beginScanTransaction(context);
    if (!transaction) {
        if (const auto error = validateContext(context, true, true, true)) {
            return failureFrom<ScanResultPage>(*error, start);
        }
        return Result<ScanResultPage>::failure(
            ErrorCode::ProtocolError,
            "failed to acquire the scan transaction",
            true,
            elapsedMilliseconds(start));
    }
    if (transaction->scanEpoch() != scanSession_->epoch) {
        scanSession_.reset();
        transaction.reset();
        return failureFrom<ScanResultPage>(scanSessionChangedError(), start);
    }

    int count = -1;
    if (!transaction->scanResultCount(count) || count < 0 ||
        static_cast<size_t>(count) > kMaxScanResultCount) {
        transaction.reset();
        if (const auto error = validateContext(context, true, true, true)) {
            return failureFrom<ScanResultPage>(*error, start);
        }
        return Result<ScanResultPage>::failure(
            ErrorCode::ProtocolError,
            "failed to read a valid scan result count",
            true,
            elapsedMilliseconds(start));
    }

    const size_t total = static_cast<size_t>(count);
    const size_t offset = std::min(request.offset, total);
    std::vector<ScanResultItem> items;
    if (offset < total &&
        !transaction->fetchScanResults(offset, request.limit, items)) {
        transaction.reset();
        if (const auto error = validateContext(context, true, true, true)) {
            return failureFrom<ScanResultPage>(*error, start);
        }
        return Result<ScanResultPage>::failure(
            ErrorCode::ProtocolError,
            "failed to fetch the scan result page",
            true,
            elapsedMilliseconds(start));
    }
    if (transaction->scanEpoch() != scanSession_->epoch) {
        scanSession_.reset();
        transaction.reset();
        return failureFrom<ScanResultPage>(scanSessionChangedError(), start);
    }
    transaction.reset();

    if (const auto error = validateContext(context, true, true, true)) {
        return failureFrom<ScanResultPage>(*error, start);
    }
    if (backend_.scanEpoch() != scanSession_->epoch) {
        scanSession_.reset();
        return failureFrom<ScanResultPage>(scanSessionChangedError(), start);
    }
    if (items.size() > request.limit || items.size() > total - offset) {
        return Result<ScanResultPage>::failure(
            ErrorCode::ProtocolError,
            "scan result page returned an invalid item count",
            false,
            elapsedMilliseconds(start));
    }

    scanSession_->resultCount = total;
    ScanResultPage page;
    page.items = std::move(items);
    page.total = total;
    page.offset = offset;
    const size_t end = offset + page.items.size();
    if (end < total) {
        page.nextOffset = end;
    }
    page.session = *scanSession_;
    return Result<ScanResultPage>::success(
        std::move(page), elapsedMilliseconds(start));
}

Result<ScanClearResult> MemService::clearScan(
    const OperationContext& context,
    const ScanClearRequest& request) {
    const auto start = Clock::now();
    std::lock_guard<std::mutex> scanLock(scanMutex_);
    if (!scanSession_) {
        return failureFrom<ScanClearResult>(noScanSessionError(), start);
    }
    if (request.expectedEpoch &&
        *request.expectedEpoch != scanSession_->epoch) {
        return failureFrom<ScanClearResult>(scanSessionChangedError(), start);
    }
    if (!context.target || scanSession_->target != *context.target) {
        scanSession_.reset();
        return failureFrom<ScanClearResult>(scanSessionChangedError(), start);
    }
    if (const auto error = validateContext(context, true, true, true)) {
        return failureFrom<ScanClearResult>(*error, start);
    }

    const uint64_t clearedEpoch = scanSession_->epoch;
    auto transaction = backend_.beginScanTransaction(context);
    if (!transaction) {
        if (const auto error = validateContext(context, true, true, true)) {
            return failureFrom<ScanClearResult>(*error, start);
        }
        return Result<ScanClearResult>::failure(
            ErrorCode::ProtocolError,
            "failed to acquire the scan transaction",
            true,
            elapsedMilliseconds(start));
    }
    if (transaction->scanEpoch() != clearedEpoch) {
        scanSession_.reset();
        transaction.reset();
        return failureFrom<ScanClearResult>(scanSessionChangedError(), start);
    }

    const bool clearSent = transaction->clearScan();
    int remaining = -1;
    const bool countConfirmed = clearSent &&
        transaction->scanResultCount(remaining);
    const uint64_t currentEpoch = transaction->scanEpoch();
    scanSession_.reset();
    transaction.reset();

    if (!clearSent || !countConfirmed) {
        return Result<ScanClearResult>::failure(
            ErrorCode::CompletionUnknown,
            "scan clear may have been sent but could not be confirmed; reconnect before retrying",
            false,
            elapsedMilliseconds(start));
    }
    if (remaining != 0) {
        return Result<ScanClearResult>::failure(
            ErrorCode::ProtocolError,
            "scan clear was acknowledged by a non-empty result set",
            false,
            elapsedMilliseconds(start));
    }
    if (const auto error = validateContext(context, true, true, false)) {
        return failureFrom<ScanClearResult>(*error, start);
    }
    if (backend_.scanEpoch() != currentEpoch) {
        return failureFrom<ScanClearResult>(scanSessionChangedError(), start);
    }

    ScanClearResult result;
    result.clearedEpoch = clearedEpoch;
    result.currentEpoch = currentEpoch;
    result.completedAfterCancelRequest = context.cancellation &&
        context.cancellation->load(std::memory_order_acquire);
    result.completedAfterDeadline = Clock::now() >= context.deadline;
    result.target = *context.target;
    return Result<ScanClearResult>::success(
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

Result<ScalarValue> MemService::readValue(
    const OperationContext& context,
    const ValueReadRequest& request) {
    const auto start = Clock::now();
    const size_t size = scalarTypeSize(request.type);
    if (size == 0) {
        return Result<ScalarValue>::failure(
            ErrorCode::InvalidArgument,
            "invalid scalar data type",
            false,
            elapsedMilliseconds(start));
    }

    MemoryReadRequest rawRequest;
    rawRequest.address = request.address;
    rawRequest.size = static_cast<uint32_t>(size);
    auto raw = readMemory(context, rawRequest);
    if (!raw.ok()) {
        return Result<ScalarValue>::failure(
            raw.error(), elapsedMilliseconds(start));
    }
    if (raw.value().bytes.size() != size) {
        return Result<ScalarValue>::failure(
            ErrorCode::ProtocolError,
            "typed memory read returned fewer bytes than required",
            false,
            elapsedMilliseconds(start));
    }

    ScalarValue value;
    value.address = raw.value().address;
    value.type = request.type;
    value.bytes = std::move(raw.value().bytes);
    value.target = raw.value().target;
    return Result<ScalarValue>::success(
        std::move(value), elapsedMilliseconds(start));
}

Result<WriteReceipt> MemService::writeMemory(
    const OperationContext& context,
    const MemoryWriteRequest& request) {
    const auto start = Clock::now();
    if (request.bytes.empty() ||
        request.bytes.size() > kMaxAgentMemoryWriteBytes) {
        return Result<WriteReceipt>::failure(
            ErrorCode::InvalidArgument,
            "memory write size must be between 1 and 4096 bytes",
            false,
            elapsedMilliseconds(start));
    }
    if (request.address >
        (std::numeric_limits<uint64_t>::max)() -
            (request.bytes.size() - 1u)) {
        return Result<WriteReceipt>::failure(
            ErrorCode::InvalidArgument,
            "memory write range overflows the uint64 address space",
            false,
            elapsedMilliseconds(start));
    }
    if (const auto error = validateContext(context, true, true, true)) {
        return failureFrom<WriteReceipt>(*error, start);
    }

    const MemoryWriteBackendResult backendResult =
        backend_.writeMemory(request.address, request.bytes);
    if (!backendResult.responseReceived) {
        if (backendResult.requestStarted) {
            return Result<WriteReceipt>::failure(
                ErrorCode::CompletionUnknown,
                "memory write was sent but its completion could not be confirmed; reconnect before continuing and do not retry automatically",
                false,
                elapsedMilliseconds(start));
        }
        if (const auto error = validateContext(context, true, true, true)) {
            return failureFrom<WriteReceipt>(*error, start);
        }
        return Result<WriteReceipt>::failure(
            ErrorCode::ProtocolError,
            "memory write could not be sent",
            true,
            elapsedMilliseconds(start));
    }

    if (backendResult.writtenBytes < 0 ||
        static_cast<uint64_t>(backendResult.writtenBytes) >
            request.bytes.size()) {
        return Result<WriteReceipt>::failure(
            ErrorCode::ProtocolError,
            "memory write returned an invalid byte count",
            false,
            elapsedMilliseconds(start));
    }
    if (static_cast<size_t>(backendResult.writtenBytes) !=
        request.bytes.size()) {
        return Result<WriteReceipt>::failure(
            ErrorCode::ProtocolError,
            "server confirmed a partial memory write (" +
                std::to_string(backendResult.writtenBytes) + "/" +
                std::to_string(request.bytes.size()) +
                " bytes); do not retry automatically",
            false,
            elapsedMilliseconds(start));
    }

    if (const auto error = validateContext(context, true, true, false)) {
        return Result<WriteReceipt>::failure(
            ErrorCode::CompletionUnknown,
            "server confirmed the memory write, but the original target context is no longer current: " +
                error->message,
            false,
            elapsedMilliseconds(start));
    }

    WriteReceipt receipt;
    receipt.address = request.address;
    receipt.requestedBytes = static_cast<uint32_t>(request.bytes.size());
    receipt.writtenBytes =
        static_cast<uint32_t>(backendResult.writtenBytes);
    receipt.completedAfterCancelRequest =
        context.cancellation &&
        context.cancellation->load(std::memory_order_acquire);
    receipt.completedAfterDeadline = Clock::now() >= context.deadline;
    receipt.target = *context.target;
    return Result<WriteReceipt>::success(
        std::move(receipt), elapsedMilliseconds(start));
}

Result<WriteReceipt> MemService::writeValue(
    const OperationContext& context,
    const ValueWriteRequest& request) {
    const auto start = Clock::now();
    const size_t expectedSize = scalarTypeSize(request.type);
    if (expectedSize == 0 || request.bytes.size() != expectedSize) {
        return Result<WriteReceipt>::failure(
            ErrorCode::InvalidArgument,
            "typed memory write byte count does not match data_type",
            false,
            elapsedMilliseconds(start));
    }

    MemoryWriteRequest rawRequest;
    rawRequest.address = request.address;
    rawRequest.bytes = request.bytes;
    auto raw = writeMemory(context, rawRequest);
    if (!raw.ok()) {
        return Result<WriteReceipt>::failure(
            raw.error(), elapsedMilliseconds(start));
    }
    return Result<WriteReceipt>::success(
        std::move(raw.value()), elapsedMilliseconds(start));
}

} // namespace Mem
