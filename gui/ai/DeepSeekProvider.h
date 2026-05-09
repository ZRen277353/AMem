#pragma once
#ifdef HAVE_AI_CHAT

#include "AIProvider.h"

namespace AI {

// DeepSeek Chat Provider
//
// Uses an OpenAI-compatible Chat Completions API served at
// https://api.deepseek.com/chat/completions (note: no /v1 prefix).
// Request/response payloads match the OpenAI schema, including SSE streaming
// and function-calling tool_calls.
//
// Implemented standalone (not as a subclass of OpenAIProvider) so that future
// divergence between the two APIs can be accommodated without cascading
// changes.
class DeepSeekProvider : public AIProvider {
public:
    std::string getName() const override { return "deepseek"; }
    std::string getDefaultBaseUrl() const override {
        // DeepSeek's endpoint is served at the root, no /v1 suffix.
        return "https://api.deepseek.com";
    }
    ProviderCapabilities getCapabilities() const override {
        return { /*streaming*/ true, /*tools*/ true, /*maxContextTokens*/ 64000 };
    }

    void configure(const ProviderConfig& config) override;
    const ProviderConfig& getConfig() const override;

    void sendCompletion(const CompletionRequest& request,
                        std::atomic<bool>& cancelFlag) override;

private:
    ProviderConfig config_;

    // Build an OpenAI-compatible Chat Completions request body.
    std::string buildRequestBody(const CompletionRequest& request);

    // Parse a single SSE event payload (the JSON object from `data: {...}`).
    // Emits any delta text via request.onToken and accumulates tool_call
    // fragments into outMessage. Sets finished=true on `[DONE]` or when the
    // server signals finish_reason.
    void parseSSEChunk(const std::string& eventData,
                       const CompletionRequest& request,
                       ChatMessage& outMessage,
                       bool& finished);

    // Parse a non-streaming full JSON response body into a CompletionResponse.
    CompletionResponse parseFullResponse(const std::string& body);

    // Validate that every tool_call's arguments string is well-formed JSON.
    // If any tool_call has invalid JSON arguments, sets response.error to
    // InvalidResponse and returns false. Otherwise returns true.
    bool validateToolCallArguments(const ChatMessage& message,
                                   CompletionResponse& response);
};

} // namespace AI

#endif // HAVE_AI_CHAT
