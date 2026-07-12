#pragma once

#include "MemTypes.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Mem {

class IMemReadTransaction {
public:
    virtual ~IMemReadTransaction() = default;

    virtual bool fetchModules(std::vector<ModuleInfo>& modules) = 0;
    virtual bool readMemory(uint64_t address,
                            uint32_t size,
                            std::vector<unsigned char>& bytes) = 0;
};

class IMemBackend {
public:
    virtual ~IMemBackend() = default;

    virtual bool isConnected() const = 0;
    virtual bool isConnectionPoisoned() const = 0;
    virtual uint64_t connectionGeneration() const = 0;
    virtual TargetSnapshot targetSnapshot() const = 0;
    virtual std::string processName() const = 0;

    virtual bool fetchServerVersion(int& version,
                                    std::string& versionString) = 0;
    virtual bool fetchArchitecture(int& type, std::string& name) = 0;
    virtual bool fetchProcesses(std::vector<ProcessInfo>& processes) = 0;
    virtual bool openProcess(int pid, const std::string& name) = 0;
    virtual bool fetchModules(std::vector<ModuleInfo>& modules) = 0;
    virtual std::unique_ptr<IMemReadTransaction> beginReadTransaction(
        const OperationContext& context) = 0;
    virtual bool readMemory(uint64_t address,
                            uint32_t size,
                            std::vector<unsigned char>& bytes) = 0;
    virtual MemoryWriteBackendResult writeMemory(
        uint64_t address,
        const std::vector<unsigned char>& bytes) = 0;
};

} // namespace Mem
