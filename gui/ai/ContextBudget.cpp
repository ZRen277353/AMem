#ifdef HAVE_AI_CHAT

#include "ContextBudget.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <sstream>

namespace AI {

namespace {

constexpr int kProviderEnvelopeTokens = 64;
constexpr int kMinimumOutputReserveTokens = 256;

int clampTokens(uint64_t tokens) noexcept {
    const uint64_t maximum =
        static_cast<uint64_t>(std::numeric_limits<int>::max());
    return static_cast<int>(std::min(tokens, maximum));
}

uint64_t addTokens(uint64_t total, uint64_t amount) noexcept {
    const uint64_t maximum =
        static_cast<uint64_t>(std::numeric_limits<int>::max());
    if (amount > maximum - std::min(total, maximum)) {
        return maximum;
    }
    return std::min(total + amount, maximum);
}

bool isContinuation(unsigned char byte) noexcept {
    return (byte & 0xC0u) == 0x80u;
}

} // namespace

int estimateTextTokensConservative(std::string_view text) noexcept {
    uint64_t tokens = 0;
    uint64_t asciiRun = 0;
    auto flushAscii = [&]() {
        tokens = addTokens(tokens, (asciiRun + 2u) / 3u);
        asciiRun = 0;
    };

    size_t i = 0;
    while (i < text.size()) {
        const unsigned char first = static_cast<unsigned char>(text[i]);
        if (first < 0x80u) {
            ++asciiRun;
            ++i;
            continue;
        }

        flushAscii();
        size_t width = 1;
        if ((first & 0xE0u) == 0xC0u) {
            width = 2;
        } else if ((first & 0xF0u) == 0xE0u) {
            width = 3;
        } else if ((first & 0xF8u) == 0xF0u) {
            width = 4;
        }

        bool valid = i + width <= text.size();
        for (size_t offset = 1; valid && offset < width; ++offset) {
            valid = isContinuation(
                static_cast<unsigned char>(text[i + offset]));
        }
        if (!valid) {
            width = 1;
        }

        tokens = addTokens(tokens, width == 4 ? 2u : 1u);
        i += width;
    }
    flushAscii();
    return clampTokens(tokens);
}

int estimateMessageTokensConservative(
    const ChatMessage& message) noexcept {
    uint64_t tokens = 0;
    tokens = addTokens(tokens, 6u);
    tokens = addTokens(tokens, static_cast<uint64_t>(
        estimateTextTokensConservative(message.content)));
    tokens = addTokens(tokens, static_cast<uint64_t>(
        estimateTextTokensConservative(message.toolCallId)));
    tokens = addTokens(tokens, static_cast<uint64_t>(
        estimateTextTokensConservative(message.name)));
    for (const ToolCall& call : message.toolCalls) {
        tokens = addTokens(tokens, 12u);
        tokens = addTokens(tokens, static_cast<uint64_t>(
            estimateTextTokensConservative(call.id)));
        tokens = addTokens(tokens, static_cast<uint64_t>(
            estimateTextTokensConservative(call.name)));
        tokens = addTokens(tokens, static_cast<uint64_t>(
            estimateTextTokensConservative(call.arguments)));
    }
    return clampTokens(tokens);
}

int estimateMessageTokensConservative(
    const std::vector<ChatMessage>& messages) noexcept {
    uint64_t tokens = 0;
    for (const ChatMessage& message : messages) {
        tokens = addTokens(tokens, static_cast<uint64_t>(
            estimateMessageTokensConservative(message)));
    }
    return clampTokens(tokens);
}

int estimateToolDefinitionTokensConservative(
    const std::vector<ToolDefinition>& tools) noexcept {
    uint64_t tokens = 0;
    for (const ToolDefinition& tool : tools) {
        tokens = addTokens(tokens, 20u);
        tokens = addTokens(tokens, static_cast<uint64_t>(
            estimateTextTokensConservative(tool.name)));
        tokens = addTokens(tokens, static_cast<uint64_t>(
            estimateTextTokensConservative(tool.description)));
        tokens = addTokens(tokens, static_cast<uint64_t>(
            estimateTextTokensConservative(tool.parametersSchema)));
    }
    return clampTokens(tokens);
}

bool eraseOldestConversationGroup(std::vector<ChatMessage>& messages) {
    size_t firstConversation = 0;
    while (firstConversation < messages.size() &&
           messages[firstConversation].role == Role::System) {
        ++firstConversation;
    }
    if (messages.size() - firstConversation <= 1) {
        return false;
    }

    size_t eraseEnd = messages.size();
    const bool startsWithUser =
        messages[firstConversation].role == Role::User;
    for (size_t i = firstConversation + 1; i < messages.size(); ++i) {
        if (messages[i].role == Role::User) {
            eraseEnd = i;
            break;
        }
    }

    if (startsWithUser && eraseEnd == messages.size()) {
        return false;
    }
    if (!startsWithUser && eraseEnd == messages.size()) {
        eraseEnd = firstConversation + 1;
    }
    if (eraseEnd <= firstConversation || eraseEnd > messages.size()) {
        return false;
    }

    messages.erase(
        messages.begin() + static_cast<std::ptrdiff_t>(firstConversation),
        messages.begin() + static_cast<std::ptrdiff_t>(eraseEnd));
    return true;
}

ContextBudgetResult prepareContextBudget(
    std::vector<ChatMessage> messages,
    const std::vector<ToolDefinition>& tools,
    const ContextBudgetConfig& config) {
    ContextBudgetResult result;

    int selectedProviderLimit = config.providerContextTokens;
    if (config.configuredContextTokens > 0) {
        selectedProviderLimit = config.configuredContextMayExceedProvider
            ? config.configuredContextTokens
            : std::min(config.providerContextTokens,
                       config.configuredContextTokens);
    }
    if (config.userTokenLimit <= 0 || selectedProviderLimit <= 0) {
        result.error = "context budget requires positive user and provider limits";
        return result;
    }

    result.contextWindowTokens =
        std::min(config.userTokenLimit, selectedProviderLimit);
    const int requestedOutput = config.providerMaxOutputTokens > 0
        ? config.providerMaxOutputTokens
        : 4096;
    const int proportionalReserve =
        std::max(kMinimumOutputReserveTokens,
                 result.contextWindowTokens / 4);
    result.outputTokenReserve =
        std::min(requestedOutput, proportionalReserve);
    if (result.contextWindowTokens <= result.outputTokenReserve) {
        result.error = "context window is too small for the output reserve";
        return result;
    }

    result.inputBudgetTokens =
        result.contextWindowTokens - result.outputTokenReserve;
    result.toolDefinitionTokens =
        estimateToolDefinitionTokensConservative(tools);

    const auto estimateInput = [&]() {
        const uint64_t total = addTokens(
            static_cast<uint64_t>(kProviderEnvelopeTokens),
            static_cast<uint64_t>(result.toolDefinitionTokens));
        return clampTokens(addTokens(
            total,
            static_cast<uint64_t>(
                estimateMessageTokensConservative(messages))));
    };

    result.estimatedInputTokens = estimateInput();
    while (result.estimatedInputTokens > result.inputBudgetTokens) {
        const size_t before = messages.size();
        if (!eraseOldestConversationGroup(messages)) {
            std::ostringstream error;
            error << "latest conversation requires "
                  << result.estimatedInputTokens
                  << " input tokens but the active budget allows "
                  << result.inputBudgetTokens
                  << " after reserving " << result.outputTokenReserve
                  << " output tokens and " << result.toolDefinitionTokens
                  << " tool-definition tokens";
            result.error = error.str();
            return result;
        }
        result.droppedMessages += before - messages.size();
        result.estimatedInputTokens = estimateInput();
    }

    result.messages = std::move(messages);
    result.success = true;
    return result;
}

} // namespace AI

#endif // HAVE_AI_CHAT
