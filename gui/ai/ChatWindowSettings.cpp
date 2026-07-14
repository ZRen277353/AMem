#ifdef HAVE_AI_CHAT

#include "ChatWindow.h"

#include "ApiKeyStore.h"
#include "AiSettings.h"
#include "DefaultSystemPrompt.h"
#include "HttpClient.h"
#include "ProviderRegistry.h"
#include "ProviderTrust.h"
#include "SecureMemory.h"
#include "ToolExecutor.h"

#include "../ColorScheme.h"
#include "../Gui.h"
#include "../../imgui/imgui.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

namespace AI {

namespace {

// Copy plaintext from a std::string into a fixed-size char array for ImGui
// InputText. Truncates silently if too long. Using memcpy + explicit null
// terminator avoids MSVC warnings on the deprecated strncpy.
template <size_t N>
void copyIntoBuf(char (&dst)[N], const std::string& src) {
    const size_t n = std::min(src.size(), N - 1);
    if (n) std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

// Case-insensitive prefix match used to validate URL schemes. Keeps the
// comparison tolerant of HTTPS:// or Https:// style capitalisations that
// users sometimes paste in.
bool startsWithICase(const std::string& s, const char* prefix) {
    const size_t n = std::strlen(prefix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        const unsigned char a = static_cast<unsigned char>(s[i]);
        const unsigned char b = static_cast<unsigned char>(prefix[i]);
        if (std::tolower(a) != std::tolower(b)) return false;
    }
    return true;
}

// AC 10.7 / 11.5: endpoint URLs must use https://. validateSettings()
// enforces this on save; the per-field inline display uses the same
// helper so the two paths can never disagree.
bool isHttpsUrl(const std::string& url) {
    return startsWithICase(url, "https://");
}

} // namespace

void ChatWindow::loadSettingsDraft() {
    settingsDraft_.clear();

    auto& keyStore = ApiKeyStore::getInstance();
    auto& registry = ProviderRegistry::getInstance();
    const AiSettingsData settings = AiSettings::getInstance().get();

    copyIntoBuf(settingsDraft_.systemPrompt, settings.systemPrompt);
    copyIntoBuf(settingsDraft_.proxyHost, settings.proxy.host);
    settingsDraft_.tokenLimit = settings.tokenLimit;
    settingsDraft_.executionTimeout = settings.executionTimeout;
    settingsDraft_.maxAgentSteps = settings.maxAgentSteps;
    settingsDraft_.maxToolCallsPerTurn = settings.maxToolCallsPerTurn;
    settingsDraft_.autoApproveWrites = settings.autoApproveWrites;
    settingsDraft_.autoApproveLuaExecution =
        settings.autoApproveLuaExecution;
    settingsDraft_.proxyEnabled = settings.proxy.enabled;
    settingsDraft_.proxyPort = settings.proxy.port;

    for (const std::string& name : registry.getProviderNames()) {
        ProviderSettingsDraft& buffer = settingsDraft_.provider(name);
        buffer.wasConfigured = keyStore.hasConfig(name);
        AIProvider* provider = registry.getProvider(name);

        ProviderConfig config;
        if (keyStore.loadConfig(name, config)) {
            copyIntoBuf(buffer.apiKey, config.apiKey);
            copyIntoBuf(buffer.baseUrl, config.baseUrl);
            copyIntoBuf(buffer.model, config.model);
            copyIntoBuf(buffer.apiVersion, config.apiVersion);
            buffer.contextWindowTokens = config.contextWindowTokens;
        }
        secureClearString(config.apiKey);

        if (buffer.baseUrl[0] == '\0') {
            if (provider) {
                copyIntoBuf(buffer.baseUrl, provider->getDefaultBaseUrl());
            }
        }
        if (provider) {
            ProviderConfig trustConfig;
            trustConfig.baseUrl = buffer.baseUrl;
            trustConfig.trustedBaseUrl = config.trustedBaseUrl;
            buffer.endpointTrusted =
                isProviderEndpointTrusted(*provider, trustConfig);
        }
    }

    settingsDraft_.loaded = true;
}

void ChatWindow::drawSettingsPanel() {
    if (!showSettings_) return;

    if (!settingsDraft_.loaded) {
        loadSettingsDraft();
    }

    ImGui::SetNextWindowSize(ImVec2(640.0f, 460.0f), ImGuiCond_Appearing);
    ImGui::OpenPopup("AI Chat Settings");
    if (!ImGui::BeginPopupModal("AI Chat Settings", &showSettings_,
                                ImGuiWindowFlags_NoCollapse)) {
        // Dismissed via the title-bar [X], Esc, or click-outside rather
        // than the Save/Cancel buttons. Drop cached edits so the next open
        // reloads fresh values from disk instead of resurrecting stale
        // unsaved edits (matches the Cancel button's reset).
        if (!showSettings_) {
            settingsDraft_.clear();
        }
        return;
    }

    auto& keyStore = ApiKeyStore::getInstance();
    auto& registry = ProviderRegistry::getInstance();
    const std::vector<std::string> providerNames = registry.getProviderNames();

    // Surface any decryption failures from a previous load so the user
    // knows why an API key field is blank (AC 10.5).
    const std::vector<std::string> failures = keyStore.getDecryptionFailures();
    if (!failures.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ColorScheme::Error);
        for (const std::string& name : failures) {
            ImGui::TextWrapped(
                "The stored API key for '%s' could not be decrypted. Please re-enter it below.",
                name.c_str());
        }
        ImGui::PopStyleColor();
        if (ImGui::Button("Dismiss warnings")) {
            keyStore.clearDecryptionFailures();
        }
        ImGui::Separator();
    }

    // One tab per registered provider.
    if (ImGui::BeginTabBar("##provider_tabs")) {
        for (const std::string& name : providerNames) {
            if (ImGui::BeginTabItem(name.c_str())) {
                drawProviderSettings(name);
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }

    ImGui::Separator();

    // Proxy configuration block. Applied globally to HttpClient on save.
    ImGui::TextDisabled("Proxy (optional)");
    ImGui::Checkbox("Enable proxy", &settingsDraft_.proxyEnabled);
    if (settingsDraft_.proxyEnabled) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(220.0f);
        ImGui::InputText("Host", settingsDraft_.proxyHost,
                         sizeof(settingsDraft_.proxyHost));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        ImGui::InputInt("Port", &settingsDraft_.proxyPort);
    }

    ImGui::Separator();

    // General AI settings (non-secret, stored in ai_settings.json).
    ImGui::TextDisabled("General");

    ImGui::Text("System Prompt:");
    ImGui::SameLine();
    // "Restore default" is intentionally right next to the label so it
    // reads as "system prompt — with a reset shortcut" rather than a
    // standalone button floating elsewhere in the panel.
    if (ImGui::SmallButton("Restore default##sysprompt")) {
        const size_t n = std::min(std::strlen(kDefaultSystemPrompt),
                                  sizeof(settingsDraft_.systemPrompt) - 1);
        std::memcpy(settingsDraft_.systemPrompt, kDefaultSystemPrompt, n);
        settingsDraft_.systemPrompt[n] = '\0';
    }
    ImGui::InputTextMultiline("##ai_system_prompt", settingsDraft_.systemPrompt,
                              sizeof(settingsDraft_.systemPrompt),
                              ImVec2(-1.0f, 80.0f));

    // Token limit — clamped to [1000, 1000000] in AiSettings::setTokenLimit.
    // Explicit button layout instead of ImGui::InputInt's built-in steppers
    // so the -/+ controls have guaranteed hit targets regardless of item-
    // width constraints (the compound InputInt layout splits its width
    // across 3 sub-items and squishes the buttons to unclickable slivers
    // when a narrow SetNextItemWidth is active).
    {
        constexpr int kStep = 1000;
        constexpr int kStepFast = 50000;
        constexpr int kMinLimit = 1000;
        constexpr int kMaxLimit = 1000000;
        ImGui::Text("Token limit");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140.0f);
        ImGui::InputInt("##tokenlimit_input", &settingsDraft_.tokenLimit, 0, 0);
        settingsDraft_.tokenLimit =
            std::clamp(settingsDraft_.tokenLimit, kMinLimit, kMaxLimit);
        ImGui::SameLine();
        if (ImGui::Button("-##tokenlimit")) {
            settingsDraft_.tokenLimit =
                std::max(kMinLimit, settingsDraft_.tokenLimit - kStep);
        }
        ImGui::SameLine();
        if (ImGui::Button("+##tokenlimit")) {
            settingsDraft_.tokenLimit =
                std::min(kMaxLimit, settingsDraft_.tokenLimit + kStep);
        }
        ImGui::SameLine();
        if (ImGui::Button("--##tokenlimit")) {
            settingsDraft_.tokenLimit =
                std::max(kMinLimit, settingsDraft_.tokenLimit - kStepFast);
        }
        ImGui::SameLine();
        if (ImGui::Button("++##tokenlimit")) {
            settingsDraft_.tokenLimit =
                std::min(kMaxLimit, settingsDraft_.tokenLimit + kStepFast);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(%d..%d)", kMinLimit, kMaxLimit);
    }

    // Tool execution timeout — same pattern, [1, 300] seconds.
    {
        constexpr int kStep = 1;
        constexpr int kStepFast = 10;
        constexpr int kMinTimeout = 1;
        constexpr int kMaxTimeout = 300;
        ImGui::Text("Tool timeout (s)");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140.0f);
        ImGui::InputInt("##tooltimeout_input",
                        &settingsDraft_.executionTimeout, 0, 0);
        settingsDraft_.executionTimeout = std::clamp(
            settingsDraft_.executionTimeout, kMinTimeout, kMaxTimeout);
        ImGui::SameLine();
        if (ImGui::Button("-##tooltimeout")) {
            settingsDraft_.executionTimeout = std::max(
                kMinTimeout, settingsDraft_.executionTimeout - kStep);
        }
        ImGui::SameLine();
        if (ImGui::Button("+##tooltimeout")) {
            settingsDraft_.executionTimeout = std::min(
                kMaxTimeout, settingsDraft_.executionTimeout + kStep);
        }
        ImGui::SameLine();
        if (ImGui::Button("--##tooltimeout")) {
            settingsDraft_.executionTimeout = std::max(
                kMinTimeout, settingsDraft_.executionTimeout - kStepFast);
        }
        ImGui::SameLine();
        if (ImGui::Button("++##tooltimeout")) {
            settingsDraft_.executionTimeout = std::min(
                kMaxTimeout, settingsDraft_.executionTimeout + kStepFast);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(%d..%d)", kMinTimeout, kMaxTimeout);
    }

    {
        constexpr int kMinSteps = 1;
        constexpr int kMaxSteps = 64;
        ImGui::Text("Agent step limit");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140.0f);
        ImGui::InputInt("##agentsteps_input",
                        &settingsDraft_.maxAgentSteps, 0, 0);
        settingsDraft_.maxAgentSteps = std::clamp(
            settingsDraft_.maxAgentSteps, kMinSteps, kMaxSteps);
        ImGui::SameLine();
        if (ImGui::Button("-##agentsteps")) {
            settingsDraft_.maxAgentSteps = std::max(
                kMinSteps, settingsDraft_.maxAgentSteps - 1);
        }
        ImGui::SameLine();
        if (ImGui::Button("+##agentsteps")) {
            settingsDraft_.maxAgentSteps = std::min(
                kMaxSteps, settingsDraft_.maxAgentSteps + 1);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(%d..%d)", kMinSteps, kMaxSteps);
    }

    {
        constexpr int kMinCalls = 1;
        constexpr int kMaxCalls = 64;
        ImGui::Text("Tool calls per turn");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140.0f);
        ImGui::InputInt("##toolcalls_input",
                        &settingsDraft_.maxToolCallsPerTurn, 0, 0);
        settingsDraft_.maxToolCallsPerTurn = std::clamp(
            settingsDraft_.maxToolCallsPerTurn, kMinCalls, kMaxCalls);
        ImGui::SameLine();
        if (ImGui::Button("-##toolcalls")) {
            settingsDraft_.maxToolCallsPerTurn = std::max(
                kMinCalls, settingsDraft_.maxToolCallsPerTurn - 1);
        }
        ImGui::SameLine();
        if (ImGui::Button("+##toolcalls")) {
            settingsDraft_.maxToolCallsPerTurn = std::min(
                kMaxCalls, settingsDraft_.maxToolCallsPerTurn + 1);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(%d..%d)", kMinCalls, kMaxCalls);
    }

    // ---- Lua-permission policy ----------------------------------------
    ImGui::Separator();
    ImGui::TextDisabled("Lua Execution Permission");
    ImGui::Checkbox("Allow Lua execution without confirmation",
                    &settingsDraft_.autoApproveLuaExecution);
    if (settingsDraft_.autoApproveLuaExecution) {
        ImGui::TextDisabled(
            "Default: applies to both AI Chat and Native IPC.");
    } else {
        ImGui::TextDisabled(
            "lua_execute requires per-request confirmation in both entry points.");
    }

    // ---- Write-permission policy --------------------------------------
    // Lua is controlled independently above. This switch applies only to
    // the remaining write-classified tools.
    ImGui::Separator();
    ImGui::TextDisabled("AI Write Permissions");
    ImGui::Checkbox("Auto-approve AI write operations (YOLO mode)",
                    &settingsDraft_.autoApproveWrites);
    if (settingsDraft_.autoApproveWrites) {
        ImGui::PushStyleColor(ImGuiCol_Text, ColorScheme::ErrorBright);
        ImGui::TextWrapped(
            "WARNING: the AI will execute memory writes, breakpoints, and "
            "process-attach requests without asking. Enable only when you "
            "fully trust the active model + prompt.");
        ImGui::PopStyleColor();
    } else {
        ImGui::TextDisabled(
            "Default: every write-classified tool call opens a confirmation modal.");
    }

    ImGui::Separator();

    // Save commits all valid buffers; Cancel drops edits.
    if (ImGui::Button("Save", ImVec2(120.0f, 32.0f))) {
        if (validateSettings()) {
            std::vector<ProviderConfigChange> changes;
            for (auto& kv : settingsDraft_.providers) {
                const std::string& pname = kv.first;
                ProviderSettingsDraft& buf = kv.second;
                if (buf.forgetRequested) {
                    ProviderConfigChange change;
                    change.providerName = pname;
                    change.remove = true;
                    changes.push_back(std::move(change));
                    continue;
                }
                if (!buf.dirty) {
                    continue;
                }
                if (std::string(buf.apiKey).empty()) {
                    continue;
                }
                ProviderConfigChange change;
                change.providerName = pname;
                change.config.apiKey = buf.apiKey;
                change.config.baseUrl = buf.baseUrl;
                change.config.model = buf.model;
                change.config.apiVersion = buf.apiVersion;
                if (AIProvider* provider = registry.getProvider(pname)) {
                    if (!isDefaultProviderEndpoint(*provider, buf.baseUrl) &&
                        buf.endpointTrusted) {
                        change.config.trustedBaseUrl =
                            canonicalEndpointForTrust(buf.baseUrl);
                    }
                }
                change.config.contextWindowTokens =
                    buf.contextWindowTokens;
                changes.push_back(std::move(change));
            }

            const bool providerChangesSaved = changes.empty() ||
                keyStore.applyChangesAndSave(changes, kConfigFile);
            if (!providerChangesSaved) {
                Gui::log("[AI Chat] provider settings were not saved");
                for (ProviderConfigChange& change : changes) {
                    secureClearString(change.config.apiKey);
                }
            } else {
                for (ProviderConfigChange& change : changes) {
                    if (AIProvider* provider = registry.getProvider(
                            change.providerName)) {
                        if (change.remove) {
                            ProviderConfig cleared;
                            cleared.baseUrl = provider->getDefaultBaseUrl();
                            provider->configure(cleared);
                        } else {
                            provider->configure(change.config);
                        }
                        if (change.providerName == currentProvider_) {
                            currentModel_ = provider->getConfig().model;
                        }
                    }
                    secureClearString(change.config.apiKey);
                }

                // Apply proxy configuration to the HTTP client singleton.
                ProxyConfig proxy;
                proxy.enabled = settingsDraft_.proxyEnabled;
                proxy.host = settingsDraft_.proxyHost;
                proxy.port = settingsDraft_.proxyPort;
                HttpClient::getInstance().setProxy(proxy);

                // Persist general AI settings to ai_settings.json.
                AiSettingsData settings = AiSettings::getInstance().get();
                settings.systemPrompt = settingsDraft_.systemPrompt;
                settings.tokenLimit = settingsDraft_.tokenLimit;
                settings.executionTimeout = settingsDraft_.executionTimeout;
                settings.maxAgentSteps = settingsDraft_.maxAgentSteps;
                settings.maxToolCallsPerTurn =
                    settingsDraft_.maxToolCallsPerTurn;
                settings.autoApproveWrites = settingsDraft_.autoApproveWrites;
                settings.autoApproveLuaExecution =
                    settingsDraft_.autoApproveLuaExecution;
                if (settings.systemPrompt.empty()) {
                    settings.systemPrompt = kDefaultSystemPrompt;
                }
                settings.proxy = proxy;
                AiSettings::getInstance().set(settings);
                ToolExecutor::getInstance().setExecutionTimeout(
                    settings.executionTimeout);
                session_.setSystemPrompt(settings.systemPrompt);
                session_.setTokenLimit(settings.tokenLimit);
                maxAgentSteps_ = settings.maxAgentSteps;
                maxToolCallsPerTurn_ = settings.maxToolCallsPerTurn;

                Gui::log("[AI Chat] settings saved");
                settingsDraft_.clear();
                showSettings_ = false;
                ImGui::CloseCurrentPopup();
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120.0f, 32.0f))) {
        // Drop in-memory edits; next open reloads from ApiKeyStore.
        settingsDraft_.clear();
        showSettings_ = false;
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

void ChatWindow::drawProviderSettings(const std::string& providerName) {
    ProviderSettingsDraft& buf = settingsDraft_.provider(providerName);

    // ---- API key (masked by default, reveal toggle, AC 10.1) ----------
    ImGui::Text("API Key:");
    ImGui::SameLine();
    ImGui::Checkbox(("Show##" + providerName).c_str(), &buf.showApiKey);
    if (buf.wasConfigured && !buf.forgetRequested) {
        ImGui::SameLine();
        if (ImGui::SmallButton(("Forget saved key##" + providerName).c_str())) {
            secureClearMemory(buf.apiKey, sizeof(buf.apiKey));
            secureClearMemory(buf.baseUrl, sizeof(buf.baseUrl));
            secureClearMemory(buf.model, sizeof(buf.model));
            secureClearMemory(buf.apiVersion, sizeof(buf.apiVersion));
            buf.contextWindowTokens = 0;
            buf.endpointTrusted = false;
            buf.showApiKey = false;
            buf.dirty = true;
            buf.forgetRequested = true;
            buf.validationError.clear();
        }
    }

    if (buf.forgetRequested) {
        ImGui::PushStyleColor(ImGuiCol_Text, ColorScheme::Warning);
        ImGui::TextUnformatted("Saved provider configuration will be removed on Save.");
        ImGui::PopStyleColor();
        if (ImGui::SmallButton(("Undo##forget_" + providerName).c_str())) {
            ProviderConfig config;
            if (ApiKeyStore::getInstance().loadConfig(providerName, config)) {
                buf.clear();
                buf.wasConfigured = true;
                copyIntoBuf(buf.apiKey, config.apiKey);
                copyIntoBuf(buf.baseUrl, config.baseUrl);
                copyIntoBuf(buf.model, config.model);
                copyIntoBuf(buf.apiVersion, config.apiVersion);
                buf.contextWindowTokens = config.contextWindowTokens;
                if (AIProvider* provider =
                        ProviderRegistry::getInstance().getProvider(providerName)) {
                    buf.endpointTrusted =
                        isProviderEndpointTrusted(*provider, config);
                }
            }
            secureClearString(config.apiKey);
        }
        return;
    }

    const ImGuiInputTextFlags keyFlags =
        buf.showApiKey ? 0 : ImGuiInputTextFlags_Password;
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputText(("##" + providerName + "_apikey").c_str(),
                         buf.apiKey, sizeof(buf.apiKey), keyFlags)) {
        buf.dirty = true;
    }

    // ---- Endpoint URL + reset (AC 11.1, 11.3, 11.4) -------------------
    ImGui::Text("Endpoint URL:");
    ImGui::SetNextItemWidth(-100.0f);
    if (ImGui::InputText(("##" + providerName + "_url").c_str(),
                         buf.baseUrl, sizeof(buf.baseUrl))) {
        buf.dirty = true;
        if (AIProvider* provider =
                ProviderRegistry::getInstance().getProvider(providerName)) {
            buf.endpointTrusted =
                isDefaultProviderEndpoint(*provider, buf.baseUrl);
        } else {
            buf.endpointTrusted = false;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(("Reset##" + providerName).c_str())) {
        if (AIProvider* p = ProviderRegistry::getInstance().getProvider(providerName)) {
            copyIntoBuf(buf.baseUrl, p->getDefaultBaseUrl());
            buf.endpointTrusted = true;
            buf.dirty = true;
        }
    }

    if (AIProvider* provider =
            ProviderRegistry::getInstance().getProvider(providerName)) {
        if (!isDefaultProviderEndpoint(*provider, buf.baseUrl)) {
            ImGui::PushStyleColor(ImGuiCol_Text, ColorScheme::Warning);
            ImGui::TextWrapped("Custom endpoint: %s", buf.baseUrl);
            ImGui::PopStyleColor();
            if (ImGui::Checkbox(("Trust this endpoint##" + providerName).c_str(),
                                &buf.endpointTrusted)) {
                buf.dirty = true;
            }
        } else {
            buf.endpointTrusted = true;
        }
    }

    // ---- Model name ----------------------------------------------------
    ImGui::Text("Model:");
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputText(("##" + providerName + "_model").c_str(),
                         buf.model, sizeof(buf.model))) {
        buf.dirty = true;
    }

    ImGui::Text("Context window:");
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::InputInt(("##" + providerName + "_context").c_str(),
                        &buf.contextWindowTokens, 0, 0)) {
        buf.contextWindowTokens =
            std::clamp(buf.contextWindowTokens, 0, 2000000);
        buf.dirty = true;
    }
    ImGui::SameLine();
    if (AIProvider* provider =
            ProviderRegistry::getInstance().getProvider(providerName)) {
        ImGui::TextDisabled("default: %d",
                            provider->getCapabilities().maxContextTokens);
    }

    // ---- API version (Anthropic only) ---------------------------------
    if (providerName == "anthropic" || providerName == "claude") {
        ImGui::Text("API Version:");
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputText(("##" + providerName + "_ver").c_str(),
                             buf.apiVersion, sizeof(buf.apiVersion))) {
            buf.dirty = true;
        }
    }

    // ---- Inline validation (AC 10.7, 11.5) ----------------------------
    //
    // Empty rows are treated as "unconfigured" and allowed — the user
    // can keep default provider entries around without being forced to
    // fill them in. We only error when the row is *partially* filled:
    // an API key without a URL, or vice versa.
    const std::string apiKeyStr = buf.apiKey;
    const std::string urlStr = buf.baseUrl;
    const bool keyEmpty = apiKeyStr.empty();
    const bool urlEmpty = urlStr.empty();
    buf.validationError.clear();
    if (keyEmpty && !buf.dirty) {
        // Unedited provider rows with a default URL are still unconfigured.
    } else if (keyEmpty && urlEmpty) {
        // Fully blank → treated as "not configured"; no error.
    } else if (keyEmpty) {
        buf.validationError = "API key is required when an endpoint URL is set.";
    } else if (urlEmpty) {
        buf.validationError = "Endpoint URL is required when an API key is set.";
    } else if (!isHttpsUrl(urlStr)) {
        buf.validationError = "Endpoint URL must begin with https://";
    } else if (!buf.endpointTrusted) {
        buf.validationError = "Custom endpoint must be explicitly trusted.";
    }

    if (!buf.validationError.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ColorScheme::Error);
        ImGui::TextWrapped("%s", buf.validationError.c_str());
        ImGui::PopStyleColor();
    }
}

bool ChatWindow::validateSettings() {
    bool allOk = true;
    for (auto& kv : settingsDraft_.providers) {
        const ProviderSettingsDraft& buf = kv.second;
        if (buf.forgetRequested) {
            continue;
        }
        const std::string apiKeyStr = buf.apiKey;
        const std::string urlStr = buf.baseUrl;
        // Skip rows the user never touched — tabs for other providers
        // shouldn't block a save that only edits general settings. A
        // partially-filled row (one field empty) is still an error.
        const bool keyEmpty = apiKeyStr.empty();
        const bool urlEmpty = urlStr.empty();
        if (keyEmpty && !buf.dirty) continue;
        if (keyEmpty && urlEmpty) continue;
        if (keyEmpty) {
            Gui::log("[AI Chat] cannot save: provider '%s' has empty API key",
                     kv.first.c_str());
            allOk = false;
            continue;
        }
        if (urlEmpty) {
            Gui::log("[AI Chat] cannot save: provider '%s' has empty endpoint URL",
                     kv.first.c_str());
            allOk = false;
            continue;
        }
        if (!isHttpsUrl(urlStr)) {
            Gui::log(
                "[AI Chat] cannot save: provider '%s' endpoint must be https:// (got: %s)",
                kv.first.c_str(), urlStr.c_str());
            allOk = false;
            continue;
        }
        if (!buf.endpointTrusted) {
            Gui::log(
                "[AI Chat] cannot save: provider '%s' custom endpoint is not trusted",
                kv.first.c_str());
            allOk = false;
        }
    }
    return allOk;
}

} // namespace AI

#endif // HAVE_AI_CHAT
