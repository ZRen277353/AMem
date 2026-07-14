#pragma once

#ifdef HAVE_AI_CHAT

#include <map>
#include <string>

namespace AI {

struct ProviderSettingsDraft {
    char apiKey[512] = {};
    char baseUrl[2049] = {};
    char model[256] = {};
    char apiVersion[64] = {};
    int contextWindowTokens = 0;
    bool showApiKey = false;
    bool dirty = false;
    bool wasConfigured = false;
    bool forgetRequested = false;
    bool endpointTrusted = false;
    std::string validationError;

    ProviderSettingsDraft() = default;
    ~ProviderSettingsDraft();
    ProviderSettingsDraft(const ProviderSettingsDraft&) = delete;
    ProviderSettingsDraft& operator=(const ProviderSettingsDraft&) = delete;
    ProviderSettingsDraft(ProviderSettingsDraft&&) = delete;
    ProviderSettingsDraft& operator=(ProviderSettingsDraft&&) = delete;

    void clear() noexcept;
};

struct ChatSettingsDraft {
    std::map<std::string, ProviderSettingsDraft> providers;
    char systemPrompt[4096] = {};
    char proxyHost[256] = {};
    int tokenLimit = 0;
    int executionTimeout = 0;
    int maxAgentSteps = 0;
    int maxToolCallsPerTurn = 0;
    int proxyPort = 0;
    bool autoApproveWrites = false;
    bool autoApproveLuaExecution = true;
    bool proxyEnabled = false;
    bool loaded = false;

    ChatSettingsDraft() = default;
    ~ChatSettingsDraft();
    ChatSettingsDraft(const ChatSettingsDraft&) = delete;
    ChatSettingsDraft& operator=(const ChatSettingsDraft&) = delete;

    ProviderSettingsDraft& provider(const std::string& name);
    void clear() noexcept;
};

} // namespace AI

#endif // HAVE_AI_CHAT
