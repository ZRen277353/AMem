#include "SystemMemService.h"

#include "MemService.h"
#include "../gui/AppContext.h"
#include "../socket/client_singleton.h"
#include "../socket/SocketCommand.h"

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
