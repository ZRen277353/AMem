#pragma once

#include "IMemBackend.h"
#include "IMemService.h"

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
    Result<MemoryBlock> readMemory(
        const OperationContext& context,
        const MemoryReadRequest& request) override;

private:
    std::optional<Error> validateContext(const OperationContext& context,
                                         bool requireConnected,
                                         bool requireTarget,
                                         bool checkCancellation) const;

    IMemBackend& backend_;
};

} // namespace Mem
