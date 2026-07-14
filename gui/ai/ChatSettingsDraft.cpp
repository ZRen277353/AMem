#ifdef HAVE_AI_CHAT

#include "ChatSettingsDraft.h"

#include "SecureMemory.h"

namespace AI {

ProviderSettingsDraft::~ProviderSettingsDraft() {
    clear();
}

void ProviderSettingsDraft::clear() noexcept {
    secureClearMemory(apiKey, sizeof(apiKey));
    secureClearMemory(baseUrl, sizeof(baseUrl));
    secureClearMemory(model, sizeof(model));
    secureClearMemory(apiVersion, sizeof(apiVersion));
    secureClearString(validationError);
    contextWindowTokens = 0;
    showApiKey = false;
    dirty = false;
    wasConfigured = false;
    forgetRequested = false;
    endpointTrusted = false;
}

ChatSettingsDraft::~ChatSettingsDraft() {
    clear();
}

ProviderSettingsDraft& ChatSettingsDraft::provider(const std::string& name) {
    return providers.try_emplace(name).first->second;
}

void ChatSettingsDraft::clear() noexcept {
    providers.clear();
    secureClearMemory(systemPrompt, sizeof(systemPrompt));
    secureClearMemory(proxyHost, sizeof(proxyHost));
    tokenLimit = 0;
    executionTimeout = 0;
    maxAgentSteps = 0;
    maxToolCallsPerTurn = 0;
    proxyPort = 0;
    autoApproveWrites = false;
    autoApproveLuaExecution = true;
    proxyEnabled = false;
    loaded = false;
}

} // namespace AI

#endif // HAVE_AI_CHAT
