#pragma once
#ifdef HAVE_AI_CHAT

#include "AIProvider.h"

namespace AI {

// Anthropic Messages API provider.
//
// Differences vs OpenAI-style providers that shape this class:
//   * "system" is a top-level request parameter, NOT a message role.
//   * Tool schemas live in a top-level "tools" array with shape
//     {name, description, input_schema}.
//   * Tool results are returned as user-role messages whose content is a
//     "tool_result" content block referencing the original "tool_use_id".
//   * The SSE stream uses named events (message_start, content_block_start,
//     content_block_delta, content_block_stop, message_delta, message_stop).
//     Each event's JSON payload carries a "type" field matching the event
//     name, so we dispatch off the data payload rather than the "event:" line
//     (the shared SSE parser only surfaces "data:" lines).
class ClaudeProvider : public AIProvider {
public:
    std::string getName() const override { return "anthropic"; }
    std::string getDefaultBaseUrl() const override {
        return "https://api.anthropic.com/v1";
    }
    ProviderCapabilities getCapabilities() const override {
        return { /*supportsStreaming=*/true,
                 /*supportsToolCalling=*/true,
                 /*maxContextTokens=*/200000 };
    }

    void configure(const ProviderConfig& config) override;
    const ProviderConfig& getConfig() const override;

    void sendCompletion(const CompletionRequest& request,
                        std::atomic<bool>& cancelFlag) override;

private:
    ProviderConfig config_;

    // Serialize the request to the Anthropic Messages API JSON body.
    std::string buildRequestBody(const CompletionRequest& request);

    // Parse a non-streaming Anthropic Messages API response body. Static
    // because it holds no provider state; keeps the HTTP completion lambda
    // free of any `this`-lifetime concerns.
    static CompletionResponse parseFullResponse(const std::string& body);
};

} // namespace AI

#endif // HAVE_AI_CHAT
