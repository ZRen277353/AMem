#pragma once
#ifdef HAVE_AI_CHAT

#include "AIProvider.h"

#include <string>

namespace AI {

struct StreamEventObservation {
    bool started = false;
    bool terminal = false;
    ProviderError error;
};

StreamEventObservation InspectClaudeStreamEvent(
    const std::string& eventData);
StreamEventObservation InspectOpenAICompatibleStreamEvent(
    const std::string& eventData,
    const std::string& providerName);

class StreamTerminalTracker {
public:
    void observe(StreamEventObservation observation);
    void fail(ProviderError error);

    bool started() const;
    bool terminal() const;
    ProviderError validationError(const std::string& providerName) const;
    bool applyValidation(const std::string& providerName,
                         CompletionResponse& response) const;

private:
    bool started_ = false;
    bool terminal_ = false;
    ProviderError firstError_;
};

} // namespace AI

#endif // HAVE_AI_CHAT
