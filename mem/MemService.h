#pragma once

#include "IMemBackend.h"
#include "IMemService.h"

#include <mutex>
#include <optional>
#include <functional>

namespace Mem {

class MemService final : public IMemService {
public:
    explicit MemService(IMemBackend& backend);

    OperationContext captureContext(bool includeTarget) const override;
    Result<Status> status(const OperationContext& context) override;
    Result<DriverInitializationReceipt> initializeDriver(
        const OperationContext& context,
        const DriverInitializeRequest& request) override;
    Result<ProcessPage> listProcesses(
        const OperationContext& context,
        const ProcessListRequest& request) override;
    Result<OpenProcessResult> openProcess(
        const OperationContext& context,
        const OpenProcessRequest& request) override;
    Result<ModulePage> listModules(
        const OperationContext& context,
        const ModuleListRequest& request) override;
    Result<ResolvedModule> resolveModule(
        const OperationContext& context,
        const ModuleResolveRequest& request) override;
    Result<PointerResolution> resolvePointer(
        const OperationContext& context,
        const PointerResolveRequest& request) override;
    Result<DisassemblyBlock> disassemble(
        const OperationContext& context,
        const DisassemblyRequest& request) override;
    Result<ResolvedSymbol> resolveSymbol(
        const OperationContext& context,
        const SymbolResolveRequest& request) override;
    Result<SymbolPage> listSymbols(
        const OperationContext& context,
        const SymbolListRequest& request) override;
    Result<SymbolTable> loadSymbolTable(
        const OperationContext& context,
        const SymbolTableRequest& request) override;
    Result<BreakpointMutationReceipt> setBreakpoint(
        const OperationContext& context,
        const BreakpointSetRequest& request) override;
    Result<BreakpointMutationReceipt> removeBreakpoint(
        const OperationContext& context,
        const BreakpointAddressRequest& request) override;
    Result<BreakpointMutationReceipt> suspendBreakpoint(
        const OperationContext& context,
        const BreakpointAddressRequest& request) override;
    Result<BreakpointMutationReceipt> resumeBreakpoint(
        const OperationContext& context,
        const BreakpointAddressRequest& request) override;
    Result<BreakpointHitBatch> breakpointHitBatch(
        const OperationContext& context,
        const BreakpointHitBatchRequest& request) override;
    Result<ScanSummary> startScan(
        const OperationContext& context,
        const ScanStartRequest& request,
        const ScanProgressSink& progress = {}) override;
    Result<ScanSummary> refineScan(
        const OperationContext& context,
        const ScanRefineRequest& request,
        const ScanProgressSink& progress = {}) override;
    Result<ScanResultPage> scanResults(
        const OperationContext& context,
        const ScanResultsRequest& request) override;
    Result<ScanClearResult> clearScan(
        const OperationContext& context,
        const ScanClearRequest& request) override;
    Result<ScanRemoveResult> removeScanResults(
        const OperationContext& context,
        const ScanRemoveRequest& request) override;
    Result<MemoryBlock> readMemory(
        const OperationContext& context,
        const MemoryReadRequest& request) override;
    Result<ScalarValue> readValue(
        const OperationContext& context,
        const ValueReadRequest& request) override;
    Result<WriteReceipt> writeMemory(
        const OperationContext& context,
        const MemoryWriteRequest& request) override;
    Result<WriteReceipt> writeValue(
        const OperationContext& context,
        const ValueWriteRequest& request) override;

private:
    std::optional<Error> validateContext(const OperationContext& context,
                                         bool requireConnected,
                                         bool requireTarget,
                                         bool checkCancellation) const;
    Result<BreakpointMutationReceipt> mutateBreakpoint(
        const OperationContext& context,
        uint64_t address,
        BreakpointAction action,
        const std::function<BreakpointMutationBackendResult()>& operation,
        std::optional<BreakpointAccess> access = std::nullopt,
        std::optional<uint32_t> size = std::nullopt);

    IMemBackend& backend_;
    std::mutex breakpointMutex_;
    std::mutex symbolMutex_;
    std::mutex scanMutex_;
    std::optional<ScanSessionSnapshot> scanSession_;
};

} // namespace Mem
