#pragma once
#ifdef HAVE_AI_CHAT

#include "AIProvider.h"

#include <string>

namespace AI {

void applyToolCallRedaction(ToolCall& call);

const std::string& toolCallArgumentsForDisplay(const ToolCall& call);

} // namespace AI

#endif // HAVE_AI_CHAT
