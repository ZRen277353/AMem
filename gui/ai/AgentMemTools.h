#pragma once

#include "../../mem/IMemService.h"

#include <string>

namespace AI {

class AgentMemTools {
public:
    explicit AgentMemTools(Mem::IMemService& service);

    std::string status(const std::string& argsJson,
                       const Mem::OperationContext& context);
    std::string serverVersion(const std::string& argsJson,
                              const Mem::OperationContext& context);
    std::string architecture(const std::string& argsJson,
                             const Mem::OperationContext& context);
    std::string processList(const std::string& argsJson,
                            const Mem::OperationContext& context);
    std::string processOpen(const std::string& argsJson,
                            const Mem::OperationContext& context);
    std::string moduleList(const std::string& argsJson,
                           bool allowLegacyArguments,
                           const Mem::OperationContext& context);
    std::string moduleResolve(const std::string& argsJson,
                              bool allowLegacyArguments,
                              const Mem::OperationContext& context);
    std::string pointerResolve(const std::string& argsJson,
                               bool allowLegacyArguments,
                               const Mem::OperationContext& context);
    std::string memoryRead(const std::string& argsJson,
                           bool allowLegacyAddress,
                           const Mem::OperationContext& context);
    std::string memoryReadValue(const std::string& argsJson,
                                bool allowLegacyArguments,
                                const Mem::OperationContext& context);
    std::string memoryWrite(const std::string& argsJson,
                            bool allowLegacyArguments,
                            const Mem::OperationContext& context);
    std::string memoryWriteValue(const std::string& argsJson,
                                 bool allowLegacyArguments,
                                 const Mem::OperationContext& context);

private:
    Mem::IMemService& service_;
};

AgentMemTools& getAgentMemTools();

} // namespace AI
