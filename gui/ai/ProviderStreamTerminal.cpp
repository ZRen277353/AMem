#ifdef HAVE_AI_CHAT

#include "ProviderStreamTerminal.h"

#include "nlohmann/json.hpp"

#include <utility>

namespace AI {

namespace {

using nlohmann::json;

ProviderError invalidEvent(const std::string& message) {
    ProviderError error;
    error.category = ErrorCategory::InvalidResponse;
    error.message = message;
    return error;
}

} // namespace

StreamEventObservation InspectClaudeStreamEvent(
    const std::string& eventData) {
    StreamEventObservation observation;
    try {
        const json event = json::parse(eventData);
        if (!event.is_object() || !event.contains("type") ||
            !event["type"].is_string()) {
            observation.error = invalidEvent(
                "Claude stream event is missing a string 'type'");
            return observation;
        }

        const std::string type = event["type"].get<std::string>();
        observation.started = type == "message_start";
        observation.terminal = type == "message_stop";
    } catch (const json::exception& error) {
        observation.error = invalidEvent(
            std::string("Claude stream event is invalid JSON: ") +
            error.what());
    }
    return observation;
}

StreamEventObservation InspectOpenAICompatibleStreamEvent(
    const std::string& eventData,
    const std::string& providerName) {
    StreamEventObservation observation;
    if (eventData == "[DONE]") {
        observation.terminal = true;
        return observation;
    }

    try {
        const json event = json::parse(eventData);
        if (!event.is_object()) {
            observation.error = invalidEvent(
                providerName + " stream event must be an object");
            return observation;
        }
        if (!event.contains("choices")) {
            return observation;
        }
        if (!event["choices"].is_array()) {
            observation.error = invalidEvent(
                providerName + " stream event has invalid 'choices'");
            return observation;
        }
        if (event["choices"].empty()) {
            return observation;
        }

        const json& choice = event["choices"][0];
        if (!choice.is_object()) {
            observation.error = invalidEvent(
                providerName + " stream choice must be an object");
            return observation;
        }
        observation.started = true;

        if (choice.contains("delta") && !choice["delta"].is_object()) {
            observation.error = invalidEvent(
                providerName + " stream choice has invalid 'delta'");
            return observation;
        }
        if (choice.contains("finish_reason") &&
            !choice["finish_reason"].is_null()) {
            if (!choice["finish_reason"].is_string()) {
                observation.error = invalidEvent(
                    providerName +
                    " stream choice has invalid 'finish_reason'");
                return observation;
            }
            observation.terminal =
                !choice["finish_reason"].get<std::string>().empty();
        }
    } catch (const json::exception& error) {
        observation.error = invalidEvent(
            providerName + " stream event is invalid JSON: " +
            error.what());
    }
    return observation;
}

void StreamTerminalTracker::observe(StreamEventObservation observation) {
    started_ = started_ || observation.started;
    terminal_ = terminal_ || observation.terminal;
    if (observation.error) {
        fail(std::move(observation.error));
    }
}

void StreamTerminalTracker::fail(ProviderError error) {
    if (!firstError_ && error) {
        firstError_ = std::move(error);
    }
}

bool StreamTerminalTracker::started() const {
    return started_;
}

bool StreamTerminalTracker::terminal() const {
    return terminal_;
}

ProviderError StreamTerminalTracker::validationError(
    const std::string& providerName) const {
    if (firstError_) {
        return firstError_;
    }
    if (!started_) {
        return invalidEvent(
            providerName + " stream ended without a valid start event");
    }
    if (!terminal_) {
        return invalidEvent(
            providerName + " stream ended without a terminal event");
    }
    return {};
}

bool StreamTerminalTracker::applyValidation(
    const std::string& providerName,
    CompletionResponse& response) const {
    ProviderError error = validationError(providerName);
    if (!error) {
        return true;
    }
    response.error = std::move(error);
    return false;
}

} // namespace AI

#endif // HAVE_AI_CHAT
