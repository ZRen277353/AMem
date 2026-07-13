#pragma once
#ifdef HAVE_AI_CHAT

#include "AIProvider.h"
#include "../../third_party/nlohmann/json.hpp"

#include <cstddef>
#include <string>

namespace AI {

bool appendAssistantContentWithinLimit(std::string& destination,
                                       const std::string& fragment,
                                       std::string& error);

bool appendToolArgumentsWithinLimit(ToolCall& destination,
                                    const std::string& fragment,
                                    size_t& totalArgumentBytes,
                                    std::string& error);

bool appendToolArgumentTextWithinLimit(std::string& destination,
                                       const std::string& fragment,
                                       size_t& totalArgumentBytes,
                                       std::string& error);

bool validateProviderMessageLimits(const ChatMessage& message,
                                   std::string& error);

bool validateChatMessageLimits(const ChatMessage& message,
                               size_t contentLimit,
                               std::string& error);

size_t chatMessagePayloadBytes(const ChatMessage& message);

ProviderError providerLimitError(const std::string& error);

bool parseProviderResponseJson(const std::string& serialized,
                               nlohmann::json& document,
                               std::string& error);

bool parseProviderEventJson(const std::string& serialized,
                            nlohmann::json& document,
                            std::string& error);

bool parseToolArgumentJson(const std::string& serialized,
                           nlohmann::json& document,
                           std::string& error);

bool parseToolResultJson(const std::string& serialized,
                         nlohmann::json& document,
                         std::string& error);

} // namespace AI

#endif // HAVE_AI_CHAT
