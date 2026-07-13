#ifdef HAVE_AI_CHAT

#include "ProviderResponseLimits.h"

#include "AiLimits.h"

namespace AI {

namespace {

bool fail(std::string& error, const char* message) {
    error = message;
    return false;
}

} // namespace

bool appendAssistantContentWithinLimit(std::string& destination,
                                       const std::string& fragment,
                                       std::string& error) {
    if (Limits::wouldExceed(destination.size(), fragment.size(),
                            Limits::kMaxAssistantContentBytes)) {
        return fail(error, "assistant content exceeds 4 MiB limit");
    }
    destination.append(fragment);
    return true;
}

bool appendToolArgumentsWithinLimit(ToolCall& destination,
                                    const std::string& fragment,
                                    size_t& totalArgumentBytes,
                                    std::string& error) {
    return appendToolArgumentTextWithinLimit(destination.arguments, fragment,
                                             totalArgumentBytes, error);
}

bool appendToolArgumentTextWithinLimit(std::string& destination,
                                       const std::string& fragment,
                                       size_t& totalArgumentBytes,
                                       std::string& error) {
    if (Limits::wouldExceed(destination.size(), fragment.size(),
                            Limits::kMaxToolArgumentsPerCallBytes)) {
        return fail(error, "tool call arguments exceed 512 KiB per-call limit");
    }
    if (Limits::wouldExceed(totalArgumentBytes, fragment.size(),
                            Limits::kMaxToolArgumentsPerMessageBytes)) {
        return fail(error, "tool call arguments exceed 4 MiB message limit");
    }
    destination.append(fragment);
    totalArgumentBytes += fragment.size();
    return true;
}

bool validateProviderMessageLimits(const ChatMessage& message,
                                   std::string& error) {
    if (!validateChatMessageLimits(
            message, Limits::kMaxAssistantContentBytes, error)) {
        if (message.content.size() > Limits::kMaxAssistantContentBytes) {
            error = "assistant content exceeds 4 MiB limit";
        }
        return false;
    }
    return true;
}

bool validateChatMessageLimits(const ChatMessage& message,
                               size_t contentLimit,
                               std::string& error) {
    if (message.content.size() > contentLimit) {
        return fail(error, "message content exceeds configured limit");
    }
    if (message.toolCalls.size() > Limits::kMaxToolCallsPerMessage) {
        return fail(error, "assistant response exceeds 64 tool calls");
    }

    size_t totalArgumentBytes = 0;
    for (const ToolCall& call : message.toolCalls) {
        if (call.id.size() > Limits::kMaxToolCallIdBytes) {
            return fail(error, "tool call id exceeds 256-byte limit");
        }
        if (call.name.size() > Limits::kMaxToolCallNameBytes) {
            return fail(error, "tool call name exceeds 64-byte limit");
        }
        if (call.arguments.size() > Limits::kMaxToolArgumentsPerCallBytes) {
            return fail(error,
                        "tool call arguments exceed 512 KiB per-call limit");
        }
        if (call.redactedArguments.size() >
            Limits::kMaxToolArgumentsPerCallBytes) {
            return fail(error,
                        "redacted tool arguments exceed 512 KiB per-call limit");
        }
        if (Limits::wouldExceed(totalArgumentBytes, call.arguments.size(),
                                Limits::kMaxToolArgumentsPerMessageBytes)) {
            return fail(error, "tool call arguments exceed 4 MiB message limit");
        }
        totalArgumentBytes += call.arguments.size();
        if (Limits::wouldExceed(totalArgumentBytes,
                                call.redactedArguments.size(),
                                Limits::kMaxToolArgumentsPerMessageBytes)) {
            return fail(error,
                        "tool arguments exceed 4 MiB in-memory message limit");
        }
        totalArgumentBytes += call.redactedArguments.size();
    }
    if (message.toolCallId.size() > Limits::kMaxToolCallIdBytes) {
        return fail(error, "tool result id exceeds 256-byte limit");
    }
    if (message.name.size() > Limits::kMaxToolCallNameBytes) {
        return fail(error, "message tool name exceeds 64-byte limit");
    }
    return true;
}

size_t chatMessagePayloadBytes(const ChatMessage& message) {
    size_t bytes = message.content.size() + message.toolCallId.size() +
                   message.name.size();
    for (const ToolCall& call : message.toolCalls) {
        bytes += call.id.size() + call.name.size() + call.arguments.size() +
                 call.redactedArguments.size();
    }
    return bytes;
}

ProviderError providerLimitError(const std::string& error) {
    ProviderError result;
    result.category = ErrorCategory::InvalidResponse;
    result.message = error;
    return result;
}

} // namespace AI

#endif // HAVE_AI_CHAT
