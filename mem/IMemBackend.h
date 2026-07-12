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

class IMemScanTransaction {
public:
    virtual ~IMemScanTransaction() = default;

    virtual uint64_t scanEpoch() const = 0;
    virtual bool setRange(ScanMemoryRegion memoryRegion) = 0;
    virtual ScanExecutionBackendResult startScan(
        const ScanStartRequest& request,
        const ScanProgressSink& progress) = 0;
    virtual ScanExecutionBackendResult refineScan(
        const ScanSessionSnapshot& session,
        const ScanRefineRequest& request,
        const ScanProgressSink& progress) = 0;
    virtual bool scanResultCount(int& count) = 0;
    virtual bool fetchScanResults(
        size_t offset,
        size_t limit,
        std::vector<ScanResultItem>& results) = 0;
    virtual bool clearScan() = 0;
    virtual bool removeScanResults(
        const std::vector<uint64_t>& addresses) = 0;
};

class IMemSymbolTransaction {
public:
    virtual ~IMemSymbolTransaction() = default;

    virtual uint64_t symbolEpoch() const = 0;
    virtual bool fetchModules(std::vector<ModuleInfo>& modules) = 0;
    virtual bool initializeSymbols(uint64_t moduleBase,
                                   int& totalCount) = 0;
    virtual bool fetchSymbols(size_t offset,
                              size_t limit,
                              std::vector<SymbolInfo>& symbols,
                              int& totalCount) = 0;
    virtual bool findSymbol(uint64_t moduleBase,
                            const std::string& name,
                            uint64_t& address) = 0;
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
    virtual DriverInitializationBackendResult initializeDriver(
        const OperationContext& context,
        const std::string& card) = 0;
    virtual bool fetchProcesses(std::vector<ProcessInfo>& processes) = 0;
    virtual bool openProcess(int pid, const std::string& name) = 0;
    virtual bool fetchModules(std::vector<ModuleInfo>& modules) = 0;
    virtual std::unique_ptr<IMemReadTransaction> beginReadTransaction(
        const OperationContext& context) = 0;
    virtual uint64_t scanEpoch() const = 0;
    virtual std::unique_ptr<IMemScanTransaction> beginScanTransaction(
        const OperationContext& context) = 0;
    virtual uint64_t symbolEpoch() const = 0;
    virtual std::unique_ptr<IMemSymbolTransaction> beginSymbolTransaction(
        const OperationContext& context) = 0;
    virtual BreakpointMutationBackendResult setBreakpoint(
        uint64_t address, BreakpointAccess access, uint32_t size) = 0;
    virtual BreakpointMutationBackendResult removeBreakpoint(
        uint64_t address) = 0;
    virtual BreakpointMutationBackendResult suspendBreakpoint(
        uint64_t address) = 0;
    virtual BreakpointMutationBackendResult resumeBreakpoint(
        uint64_t address) = 0;
    virtual bool fetchBreakpointHitBatch(
        uint64_t address,
        size_t limit,
        std::vector<BreakpointHit>& hits,
        size_t& total) = 0;
    virtual bool readMemory(uint64_t address,
                            uint32_t size,
                            std::vector<unsigned char>& bytes) = 0;
    virtual MemoryWriteBackendResult writeMemory(
        uint64_t address,
        const std::vector<unsigned char>& bytes) = 0;
};

} // namespace Mem
