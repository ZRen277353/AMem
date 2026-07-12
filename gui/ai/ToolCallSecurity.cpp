#ifdef HAVE_AI_CHAT

#include "ToolCallSecurity.h"

#include "../../third_party/nlohmann/json.hpp"

namespace AI {

namespace {

bool hasSensitiveArguments(const std::string& toolName) {
    return toolName == "driver_initialize" || toolName == "init_driver";
}

} // namespace

void applyToolCallRedaction(ToolCall& call) {
    call.redactedArguments.clear();
    if (!hasSensitiveArguments(call.name)) {
        return;
    }

    try {
        nlohmann::json args = call.arguments.empty()
            ? nlohmann::json::object()
            : nlohmann::json::parse(call.arguments);
        if (!args.is_object()) {
            call.redactedArguments = R"({"redacted":true})";
            return;
        }
        for (const char* key : {"card", "card_name"}) {
            if (args.contains(key)) {
                args[key] = "[REDACTED]";
            }
        }
        call.redactedArguments = args.dump();
    } catch (const nlohmann::json::exception&) {
        call.redactedArguments = R"({"redacted":true})";
    }
}

const std::string& toolCallArgumentsForDisplay(const ToolCall& call) {
    return call.redactedArguments.empty()
        ? call.arguments
        : call.redactedArguments;
}

} // namespace AI

#endif // HAVE_AI_CHAT
