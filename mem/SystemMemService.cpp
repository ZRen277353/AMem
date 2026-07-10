#include "SystemMemService.h"

#include "MemService.h"
#include "../gui/AppContext.h"
#include "../socket/client_singleton.h"

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

    bool readMemory(uint64_t address,
                    uint32_t size,
                    std::vector<unsigned char>& bytes) override {
        return ReadProcessMemoryBytes(address, size, bytes, PORT_MAIN);
    }
};

} // namespace

IMemService& getSystemMemService() {
    static SystemMemBackend backend;
    static MemService service(backend);
    return service;
}

} // namespace Mem
