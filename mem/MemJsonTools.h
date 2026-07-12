#pragma once

#include "IMemService.h"

#include <string>

namespace Mem {

class MemJsonTools {
public:
    explicit MemJsonTools(IMemService& service);

    std::string status(const std::string& argsJson,
                       const Mem::OperationContext& context);
    std::string driverInitialize(const std::string& argsJson,
                                 const Mem::OperationContext& context);
    std::string processList(const std::string& argsJson,
                            const Mem::OperationContext& context);
    std::string processOpen(const std::string& argsJson,
                            const Mem::OperationContext& context);
    std::string moduleList(const std::string& argsJson,
                           const Mem::OperationContext& context);
    std::string moduleResolve(const std::string& argsJson,
                              const Mem::OperationContext& context);
    std::string pointerResolve(const std::string& argsJson,
                               const Mem::OperationContext& context);
    std::string disassemble(const std::string& argsJson,
                            const Mem::OperationContext& context);
    std::string symbolResolve(const std::string& argsJson,
                              const Mem::OperationContext& context);
    std::string symbolList(const std::string& argsJson,
                           const Mem::OperationContext& context);
    std::string breakpointSet(const std::string& argsJson,
                              const Mem::OperationContext& context);
    std::string breakpointRemove(const std::string& argsJson,
                                 const Mem::OperationContext& context);
    std::string breakpointSuspend(const std::string& argsJson,
                                  const Mem::OperationContext& context);
    std::string breakpointResume(const std::string& argsJson,
                                 const Mem::OperationContext& context);
    std::string breakpointHits(const std::string& argsJson,
                               const Mem::OperationContext& context);
    std::string scanStart(const std::string& argsJson,
                          const Mem::OperationContext& context);
    std::string scanRefine(const std::string& argsJson,
                           const Mem::OperationContext& context);
    std::string scanResults(const std::string& argsJson,
                            const Mem::OperationContext& context);
    std::string scanClear(const std::string& argsJson,
                          const Mem::OperationContext& context);
    std::string memoryRead(const std::string& argsJson,
                           const Mem::OperationContext& context);
    std::string memoryReadValue(const std::string& argsJson,
                                const Mem::OperationContext& context);
    std::string memoryWrite(const std::string& argsJson,
                            const Mem::OperationContext& context);
    std::string memoryWriteValue(const std::string& argsJson,
                                 const Mem::OperationContext& context);

private:
    IMemService& service_;
};

} // namespace Mem
