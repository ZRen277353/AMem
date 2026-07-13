#pragma once
#ifdef HAVE_AI_CHAT

#include "AIProvider.h"

namespace AI {

// OpenAI Chat Completions Provider.
//
// Custom OpenAI-compatible gateways remain supported through provider
// settings, but they require an endpoint-bound trust confirmation.
class OpenAIProvider : public AIProvider {
public:
    std::string getName() const override { return "openai"; }
    std::string getDefaultBaseUrl() const override {
        return "https://api.openai.com/v1";
    }
    ProviderCapabilities getCapabilities() const override {
        return { true, true, 128000 };
    }

    void configure(const ProviderConfig& config) override;
    const ProviderConfig& getConfig() const override;

    void sendCompletion(const CompletionRequest& request,
                        CancellationToken cancelToken) override;

private:
    ProviderConfig config_;

    // Build the JSON request body for OpenAI Chat Completions API.
    std::string buildRequestBody(const CompletionRequest& request) const;

    // Parse a single SSE chunk (one "data: {...}" payload) and return the
    // delta as a CompletionResponse. Does not aggregate across chunks.
    CompletionResponse parseSSEChunk(const std::string& chunk) const;

    // Parse a full non-streaming OpenAI response body.
    CompletionResponse parseFullResponse(const std::string& body) const;
};

} // namespace AI

#endif // HAVE_AI_CHAT
