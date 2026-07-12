#pragma once

#include "MemResult.h"
#include "MemTypes.h"

namespace Mem {

class IMemService {
public:
    virtual ~IMemService() = default;

    virtual OperationContext captureContext(bool includeTarget) const = 0;
    virtual Result<Status> status(const OperationContext& context) = 0;
    virtual Result<ProcessPage> listProcesses(
        const OperationContext& context,
        const ProcessListRequest& request) = 0;
    virtual Result<OpenProcessResult> openProcess(
        const OperationContext& context,
        const OpenProcessRequest& request) = 0;
    virtual Result<ModulePage> listModules(
        const OperationContext& context,
        const ModuleListRequest& request) = 0;
    virtual Result<ResolvedModule> resolveModule(
        const OperationContext& context,
        const ModuleResolveRequest& request) = 0;
    virtual Result<MemoryBlock> readMemory(
        const OperationContext& context,
        const MemoryReadRequest& request) = 0;
    virtual Result<ScalarValue> readValue(
        const OperationContext& context,
        const ValueReadRequest& request) = 0;
    virtual Result<WriteReceipt> writeMemory(
        const OperationContext& context,
        const MemoryWriteRequest& request) = 0;
    virtual Result<WriteReceipt> writeValue(
        const OperationContext& context,
        const ValueWriteRequest& request) = 0;
};

} // namespace Mem
