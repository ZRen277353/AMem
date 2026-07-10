#include "AgentMemTools.h"

#include "../../mem/SystemMemService.h"

namespace AI {

AgentMemTools& getAgentMemTools() {
    static AgentMemTools tools(Mem::getSystemMemService());
    return tools;
}

} // namespace AI
