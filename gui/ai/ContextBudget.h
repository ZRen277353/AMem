#pragma once

#ifdef HAVE_AI_CHAT

#include "AIProvider.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace AI {

struct ContextBudgetConfig {
    int userTokenLimit = 0;
    int providerContextTokens = 0;
    int configuredContextTokens = 0;
    bool configuredContextMayExceedProvider = false;
    int providerMaxOutputTokens = 4096;
};

struct ContextBudgetResult {
    bool success = false;
    std::vector<ChatMessage> messages;
    int contextWindowTokens = 0;
    int inputBudgetTokens = 0;
    int estimatedInputTokens = 0;
    int toolDefinitionTokens = 0;
    int outputTokenReserve = 0;
    size_t droppedMessages = 0;
    std::string error;
};

int estimateTextTokensConservative(std::string_view text) noexcept;
int estimateMessageTokensConservative(
    const ChatMessage& message) noexcept;
int estimateMessageTokensConservative(
    const std::vector<ChatMessage>& messages) noexcept;
int estimateToolDefinitionTokensConservative(
    const std::vector<ToolDefinition>& tools) noexcept;

// Removes the oldest complete user-owned conversation group while retaining
// a leading system prompt and the newest user group.
bool eraseOldestConversationGroup(std::vector<ChatMessage>& messages);

ContextBudgetResult prepareContextBudget(
    std::vector<ChatMessage> messages,
    const std::vector<ToolDefinition>& tools,
    const ContextBudgetConfig& config);

} // namespace AI

#endif // HAVE_AI_CHAT
