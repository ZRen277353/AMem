#include "SystemMemService.h"

#include "MemService.h"
#include "../gui/AppContext.h"
#include "../gui/MemoryTypes.h"
#include "../socket/client_singleton.h"
#include "../socket/SocketCommand.h"

#include <limits>
#include <utility>

namespace Mem {

namespace {

const char* architectureName(int type) {
    switch (type) {
        case MemType_Null:    return "Null";
        case MemType_IO:      return "IO";
        case MemType_Syscall: return "Syscall";
        case MemType_Kernel:  return "Kernel";
        case MemType_SysHook: return "SysHook";
        default:              return "Unknown";
    }
}

bool fetchSystemModules(std::vector<ModuleInfo>& modules) {
    std::vector<ModuleInfoItem> items;
    if (!FetchModuleList(items, PORT_MAIN)) {
        return false;
    }

    modules.clear();
    modules.reserve(items.size());
    for (auto& item : items) {
        ModuleInfo module;
        module.base = item.base;
        module.size = item.size > 0
            ? static_cast<uint64_t>(item.size)
            : 0;
        module.type = item.type;
        module.flag = item.flag;
        module.name = std::move(item.name);
        modules.push_back(std::move(module));
    }
    return true;
}

class SystemReadTransaction final : public IMemReadTransaction {
public:
    explicit SystemReadTransaction(const OperationContext& context)
        : lease_(PORT_MAIN) {
        valid_ = static_cast<bool>(lease_) && context.target &&
                 lease_.generation() == context.connectionGeneration &&
                 AppContext::Get().matchesStableTarget(
                     *context.target, lease_.generation());
    }

    bool valid() const {
        return valid_ && static_cast<bool>(lease_);
    }

    bool fetchModules(std::vector<ModuleInfo>& modules) override {
        return valid() && fetchSystemModules(modules);
    }

    bool readMemory(uint64_t address,
                    uint32_t size,
                    std::vector<unsigned char>& bytes) override {
        return valid() &&
               ReadProcessMemoryBytes(address, size, bytes, PORT_MAIN);
    }

private:
    SocketCommand::TransactionLease lease_;
    bool valid_ = false;
};

uint32_t scanDataTypeFlag(ScanDataType type) {
    switch (type) {
        case ScanDataType::Byte:   return BYTE_;
        case ScanDataType::Word:   return WORD_;
        case ScanDataType::Dword:  return DWORD_;
        case ScanDataType::Qword:  return QWORD_;
        case ScanDataType::Xor:    return XOR_;
        case ScanDataType::Float:  return FLOAT_;
        case ScanDataType::Double: return DOUBLE_;
        case ScanDataType::Bytes:  return 0;
        default:                   return 0;
    }
}

uint32_t scanModeFlag(ScanMode mode) {
    switch (mode) {
        case ScanMode::Exact:       return _ACCURATE_VAL;
        case ScanMode::Greater:     return _LARGER_THAN_VAL;
        case ScanMode::Less:        return _LESS_THAN_VAL;
        case ScanMode::Between:     return _BETWEEN_VAL;
        case ScanMode::Unknown:     return _UNKNOW_VAL;
        case ScanMode::Increased:   return _ADD_UNKNOW_VAL;
        case ScanMode::IncreasedBy: return _ADD_ACCURATE_VAL;
        case ScanMode::Decreased:   return _SUB_UNKNOW_VAL;
        case ScanMode::DecreasedBy: return _SUB_ACCURATE_VAL;
        case ScanMode::Changed:     return _CHANGED_VAL;
        case ScanMode::Unchanged:   return _UNCHANGED_VAL;
        default:                    return 0;
    }
}

struct ScanCancelContext {
    CancellationToken cancellation;
    bool stopIssued = false;
};

void requestScanStop(float, uint64_t, uint64_t, uint64_t, void* userData) {
    auto* context = static_cast<ScanCancelContext*>(userData);
    if (!context || context->stopIssued || !context->cancellation ||
        !context->cancellation->load(std::memory_order_acquire)) {
        return;
    }
    context->stopIssued = true;
    (void)StopSearchScan(PORT_DEBUG);
}

ScanExecutionBackendResult toBackendScanResult(
    const ScanExecutionIoResult& io,
    const CancellationToken& cancellation) {
    ScanExecutionBackendResult result;
    result.requestStarted = io.requestStarted;
    result.responseReceived = io.responseReceived;
    result.resultCount = io.resultCount;
    result.cancelRequested = cancellation &&
        cancellation->load(std::memory_order_acquire);
    return result;
}

BreakpointMutationBackendResult toBreakpointBackendResult(
    const BreakpointMutationIoResult& io) {
    BreakpointMutationBackendResult result;
    result.requestStarted = io.requestStarted;
    result.responseReceived = io.responseReceived;
    result.applied = io.applied;
    return result;
}

class SystemScanTransaction final : public IMemScanTransaction {
public:
    explicit SystemScanTransaction(const OperationContext& context)
        : lease_(PORT_MAIN), cancellation_(context.cancellation) {
        valid_ = static_cast<bool>(lease_) && context.target &&
                 lease_.generation() == context.connectionGeneration &&
                 AppContext::Get().matchesStableTarget(
                     *context.target, lease_.generation());
    }

    bool valid() const {
        return valid_ && static_cast<bool>(lease_);
    }

    uint64_t scanEpoch() const override {
        return GetSocketMgr().GetScanEpoch();
    }

    bool setRange(ScanMemoryRegion memoryRegion) override {
        return valid() && ScanSetRange(
            static_cast<int>(memoryRegion), PORT_MAIN);
    }

    ScanExecutionBackendResult startScan(
        const ScanStartRequest& request) override {
        if (!valid()) {
            return {};
        }
        ScanCancelContext cancel{cancellation_, false};
        std::vector<unsigned char> value = request.value;
        ScanExecutionIoResult io;
        if (request.kind == ScanStartKind::BytePattern) {
            io = ScanHEXValueTracked(
                request.start, request.end, value,
                &requestScanStop, &cancel, PORT_MAIN);
        } else if (request.kind == ScanStartKind::Unknown) {
            const uint32_t flags =
                scanModeFlag(request.mode) |
                scanDataTypeFlag(request.dataType);
            io = ScanFuzzyValueTracked(
                flags, &requestScanStop, &cancel,
                request.start, request.end, PORT_MAIN);
        } else {
            const uint32_t flags =
                scanModeFlag(request.mode) |
                scanDataTypeFlag(request.dataType);
            io = ScanValueTracked(
                flags, value, &requestScanStop, &cancel,
                request.start, request.end, PORT_MAIN);
        }
        return toBackendScanResult(io, cancellation_);
    }

    ScanExecutionBackendResult refineScan(
        const ScanSessionSnapshot& session,
        const ScanRefineRequest& request) override {
        if (!valid()) {
            return {};
        }
        ScanCancelContext cancel{cancellation_, false};
        std::vector<unsigned char> value = request.value;
        const uint32_t flags =
            scanModeFlag(request.mode) |
            scanDataTypeFlag(session.dataType);
        const ScanExecutionIoResult io = ScanNextValueTracked(
            value, static_cast<int>(flags),
            &requestScanStop, &cancel,
            session.start, session.end, PORT_MAIN);
        return toBackendScanResult(io, cancellation_);
    }

    bool scanResultCount(int& count) override {
        if (!valid()) {
            return false;
        }
        count = GetScanResultCount(PORT_MAIN);
        return count >= 0;
    }

    bool fetchScanResults(
        size_t offset,
        size_t limit,
        std::vector<ScanResultItem>& results) override {
        if (!valid() ||
            offset > static_cast<size_t>((std::numeric_limits<int>::max)()) ||
            limit > static_cast<size_t>((std::numeric_limits<int>::max)())) {
            return false;
        }
        std::vector<std::pair<uint64_t, uint64_t>> raw;
        if (!GetScanResult(static_cast<int>(offset),
                           static_cast<int>(limit),
                           raw,
                           PORT_MAIN)) {
            return false;
        }
        results.clear();
        results.reserve(raw.size());
        for (const auto& item : raw) {
            results.push_back(ScanResultItem{item.first, item.second});
        }
        return true;
    }

    bool clearScan() override {
        return valid() && ClearScanResult(PORT_MAIN);
    }

private:
    SocketCommand::TransactionLease lease_;
    CancellationToken cancellation_;
    bool valid_ = false;
};

class SystemSymbolTransaction final : public IMemSymbolTransaction {
public:
    explicit SystemSymbolTransaction(const OperationContext& context)
        : lease_(PORT_MAIN) {
        valid_ = static_cast<bool>(lease_) && context.target &&
                 lease_.generation() == context.connectionGeneration &&
                 AppContext::Get().matchesStableTarget(
                     *context.target, lease_.generation());
    }

    bool valid() const {
        return valid_ && static_cast<bool>(lease_);
    }

    uint64_t symbolEpoch() const override {
        return GetSocketMgr().GetSymbolEpoch();
    }

    bool fetchModules(std::vector<ModuleInfo>& modules) override {
        return valid() && fetchSystemModules(modules);
    }

    bool initializeSymbols(uint64_t moduleBase,
                           int& totalCount) override {
        return valid() && SymbolInit(moduleBase, totalCount, PORT_MAIN);
    }

    bool fetchSymbols(size_t offset,
                      size_t limit,
                      std::vector<SymbolInfo>& symbols,
                      int& totalCount) override {
        if (!valid() ||
            offset > static_cast<size_t>((std::numeric_limits<int>::max)()) ||
            limit > static_cast<size_t>((std::numeric_limits<int>::max)())) {
            return false;
        }
        std::vector<std::pair<uint64_t, std::string>> raw;
        if (!SymbolGetList(static_cast<int>(offset),
                           static_cast<int>(limit),
                           raw,
                           &totalCount,
                           PORT_MAIN)) {
            return false;
        }
        symbols.clear();
        symbols.reserve(raw.size());
        for (auto& item : raw) {
            symbols.push_back(SymbolInfo{item.first, std::move(item.second)});
        }
        return true;
    }

    bool findSymbol(uint64_t moduleBase,
                    const std::string& name,
                    uint64_t& address) override {
        return valid() && SymbolFind(moduleBase, name, address, PORT_MAIN);
    }

private:
    SocketCommand::TransactionLease lease_;
    bool valid_ = false;
};

class SystemMemBackend final : public IMemBackend {
public:
    bool isConnected() const override {
        return IsMultiPortConnected();
    }

    bool isConnectionPoisoned() const override {
        return IsConnectionPoisoned();
    }

    uint64_t connectionGeneration() const override {
        return GetSocketMgr().GetConnectionGeneration();
    }

    TargetSnapshot targetSnapshot() const override {
        return AppContext::Get().snapshotTarget(connectionGeneration());
    }

    std::string processName() const override {
        return AppContext::Get().getSelectedName();
    }

    bool fetchServerVersion(int& version,
                            std::string& versionString) override {
        ServerVersionInfo info;
        if (!FetchServerVersion(info, PORT_MAIN)) {
            return false;
        }
        version = info.version;
        versionString = std::move(info.versionString);
        return true;
    }

    bool fetchArchitecture(int& type, std::string& name) override {
        if (!GetMemType(type, PORT_MAIN)) {
            return false;
        }
        name = architectureName(type);
        return true;
    }

    bool fetchProcesses(std::vector<ProcessInfo>& processes) override {
        std::vector<ProcessInfoItem> items;
        if (!FetchProcessList(items, PORT_MAIN)) {
            return false;
        }

        processes.clear();
        processes.reserve(items.size());
        for (auto& item : items) {
            processes.push_back(ProcessInfo{item.pid, std::move(item.name)});
        }
        return true;
    }

    bool openProcess(int pid, const std::string& name) override {
        AppContext::Get().selectProcess(pid, name);
        const TargetSnapshot target = targetSnapshot();
        return target.isAttached() && target.pid == pid;
    }

    bool fetchModules(std::vector<ModuleInfo>& modules) override {
        return fetchSystemModules(modules);
    }

    std::unique_ptr<IMemReadTransaction> beginReadTransaction(
        const OperationContext& context) override {
        auto transaction = std::make_unique<SystemReadTransaction>(context);
        if (!transaction->valid()) {
            return nullptr;
        }
        return transaction;
    }

    uint64_t scanEpoch() const override {
        return GetSocketMgr().GetScanEpoch();
    }

    std::unique_ptr<IMemScanTransaction> beginScanTransaction(
        const OperationContext& context) override {
        auto transaction = std::make_unique<SystemScanTransaction>(context);
        if (!transaction->valid()) {
            return nullptr;
        }
        return transaction;
    }

    uint64_t symbolEpoch() const override {
        return GetSocketMgr().GetSymbolEpoch();
    }

    std::unique_ptr<IMemSymbolTransaction> beginSymbolTransaction(
        const OperationContext& context) override {
        auto transaction = std::make_unique<SystemSymbolTransaction>(context);
        if (!transaction->valid()) {
            return nullptr;
        }
        return transaction;
    }

    BreakpointMutationBackendResult setBreakpoint(
        uint64_t address,
        BreakpointAccess access,
        uint32_t size) override {
        return toBreakpointBackendResult(SetKernelBreakpointTracked(
            address, static_cast<uint32_t>(access), size, PORT_MAIN));
    }

    BreakpointMutationBackendResult removeBreakpoint(
        uint64_t address) override {
        return toBreakpointBackendResult(
            RemoveKernelBreakpointTracked(address, PORT_MAIN));
    }

    BreakpointMutationBackendResult suspendBreakpoint(
        uint64_t address) override {
        return toBreakpointBackendResult(
            SuspendKernelBreakpointTracked(address, PORT_MAIN));
    }

    BreakpointMutationBackendResult resumeBreakpoint(
        uint64_t address) override {
        return toBreakpointBackendResult(
            ResumeKernelBreakpointTracked(address, PORT_MAIN));
    }

    bool fetchBreakpointHits(
        uint64_t address,
        size_t offset,
        size_t limit,
        std::vector<BreakpointHit>& hits,
        size_t& total) override {
        std::vector<HW_HIT_INFO> raw;
        if (!ReadKernelBreakpointInfoPage(
                address, offset, limit, raw, total, PORT_MAIN)) {
            return false;
        }
        if (total > kMaxBreakpointHitCount || raw.size() > limit) {
            return false;
        }
        hits.clear();
        hits.reserve(raw.size());
        for (const auto& item : raw) {
            BreakpointHit hit;
            hit.hitAddress = item.hit_addr;
            hit.hitTime = item.hit_time;
            for (size_t index = 0; index < hit.registers.size(); ++index) {
                hit.registers[index] = item.regs_info.regs[index];
            }
            hit.stackPointer = item.regs_info.sp;
            hit.programCounter = item.regs_info.pc;
            hit.pstate = item.regs_info.pstate;
            hits.push_back(std::move(hit));
        }
        return true;
    }

    bool readMemory(uint64_t address,
                    uint32_t size,
                    std::vector<unsigned char>& bytes) override {
        return ReadProcessMemoryBytes(address, size, bytes, PORT_MAIN);
    }

    MemoryWriteBackendResult writeMemory(
        uint64_t address,
        const std::vector<unsigned char>& bytes) override {
        const MemoryWriteIoResult io = WriteProcessMemoryBytesTracked(
            address,
            static_cast<uint32_t>(bytes.size()),
            bytes,
            PORT_MAIN);
        MemoryWriteBackendResult result;
        result.requestStarted = io.requestStarted;
        result.responseReceived = io.responseReceived;
        result.writtenBytes = io.writtenBytes;
        return result;
    }
};

} // namespace

IMemService& getSystemMemService() {
    static SystemMemBackend backend;
    static MemService service(backend);
    return service;
}

} // namespace Mem
