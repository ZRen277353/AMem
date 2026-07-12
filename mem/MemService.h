#pragma once

#include "IMemBackend.h"
#include "IMemService.h"

#include <mutex>
#include <optional>

namespace Mem {

class MemService final : public IMemService {
public:
    explicit MemService(IMemBackend& backend);

    OperationContext captureContext(bool includeTarget) const override;
    Result<Status> status(const OperationContext& context) override;
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
    Result<ResolvedSymbol> resolveSymbol(
        const OperationContext& context,
        const SymbolResolveRequest& request) override;
    Result<SymbolPage> listSymbols(
        const OperationContext& context,
        const SymbolListRequest& request) override;
    Result<ScanSummary> startScan(
        const OperationContext& context,
        const ScanStartRequest& request) override;
    Result<ScanSummary> refineScan(
        const OperationContext& context,
        const ScanRefineRequest& request) override;
    Result<ScanResultPage> scanResults(
        const OperationContext& context,
        const ScanResultsRequest& request) override;
    Result<ScanClearResult> clearScan(
        const OperationContext& context,
        const ScanClearRequest& request) override;
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

    IMemBackend& backend_;
    std::mutex symbolMutex_;
    std::mutex scanMutex_;
    std::optional<ScanSessionSnapshot> scanSession_;
};

} // namespace Mem
