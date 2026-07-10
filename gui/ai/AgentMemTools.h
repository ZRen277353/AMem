#pragma once

#include "../../mem/IMemService.h"

#include <string>

namespace AI {

class AgentMemTools {
public:
    explicit AgentMemTools(Mem::IMemService& service);

    std::string status(const std::string& argsJson);
    std::string serverVersion(const std::string& argsJson);
    std::string architecture(const std::string& argsJson);
    std::string processList(const std::string& argsJson);
    std::string processOpen(const std::string& argsJson);
    std::string memoryRead(const std::string& argsJson,
                           bool allowLegacyAddress);

private:
    Mem::IMemService& service_;
};

AgentMemTools& getAgentMemTools();

} // namespace AI
