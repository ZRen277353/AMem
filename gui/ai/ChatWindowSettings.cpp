#ifdef HAVE_AI_CHAT

#include "ChatWindow.h"

#include "ApiKeyStore.h"
#include "AiSettings.h"
#include "DefaultSystemPrompt.h"
#include "HttpClient.h"
#include "ProviderRegistry.h"
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

void ChatWindow::drawSettingsPanel() {
    if (!showSettings_) return;

    ImGui::SetNextWindowSize(ImVec2(640.0f, 460.0f), ImGuiCond_Appearing);
    ImGui::OpenPopup("AI Chat Settings");
    if (!ImGui::BeginPopupModal("AI Chat Settings", &showSettings_,
                                ImGuiWindowFlags_NoCollapse)) {
        return;
    }

    auto& keyStore = ApiKeyStore::getInstance();
    auto& registry = ProviderRegistry::getInstance();
    const std::vector<std::string> providerNames = registry.getProviderNames();

    // Lazily populate settingsBuffers_ from ApiKeyStore on first open so
    // the panel reflects the live on-disk configuration. Unpopulated
    // buffers get the provider's default base URL (AC 11.3).
    for (const std::string& name : providerNames) {
        if (settingsBuffers_.find(name) == settingsBuffers_.end()) {
            SettingsBuffer& buf = settingsBuffers_[name];
            ProviderConfig cfg;
            if (keyStore.loadConfig(name, cfg)) {
                copyIntoBuf(buf.apiKey, cfg.apiKey);
                copyIntoBuf(buf.baseUrl, cfg.baseUrl);
                copyIntoBuf(buf.model, cfg.model);
                copyIntoBuf(buf.apiVersion, cfg.apiVersion);
            }
            // If no baseUrl was stored (or no config existed), seed the
            // field with the provider's default so the user sees a
            // working URL immediately (AC 11.3).
            if (buf.baseUrl[0] == '\0') {
                if (AIProvider* p = registry.getProvider(name)) {
                    copyIntoBuf(buf.baseUrl, p->getDefaultBaseUrl());
                }
            }
        }
    }

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
    ImGui::Checkbox("Enable proxy", &proxyEnabled_);
    if (proxyEnabled_) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(220.0f);
        ImGui::InputText("Host", proxyHost_, sizeof(proxyHost_));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        ImGui::InputInt("Port", &proxyPort_);
    }

    ImGui::Separator();

    // General AI settings (non-secret, stored in ai_settings.json).
    ImGui::TextDisabled("General");

    AiSettingsData settings = AiSettings::getInstance().get();

    // The edit buffers below must survive across frames — `settings` is
    // a fresh snapshot on every draw, so writing straight back into its
    // fields and reading them next frame would revert every edit. The
    // systemPromptBuf / numericSettingsLoaded pattern keeps the user's
    // in-progress edits until Save or Cancel commits/discards them.
    static char systemPromptBuf[4096] = {};
    static bool systemPromptLoaded = false;
    static int tokenLimitBuf = 0;
    static int executionTimeoutBuf = 0;
    static bool autoApproveWritesBuf = false;
    static bool numericSettingsLoaded = false;
    if (!systemPromptLoaded) {
        const size_t n = std::min(settings.systemPrompt.size(),
                                  sizeof(systemPromptBuf) - 1);
        if (n) std::memcpy(systemPromptBuf, settings.systemPrompt.data(), n);
        systemPromptBuf[n] = '\0';
        systemPromptLoaded = true;
    }
    if (!numericSettingsLoaded) {
        tokenLimitBuf = settings.tokenLimit;
        executionTimeoutBuf = settings.executionTimeout;
        autoApproveWritesBuf = settings.autoApproveWrites;
        numericSettingsLoaded = true;
    }
    ImGui::Text("System Prompt:");
    ImGui::SameLine();
    // "Restore default" is intentionally right next to the label so it
    // reads as "system prompt — with a reset shortcut" rather than a
    // standalone button floating elsewhere in the panel.
    if (ImGui::SmallButton("Restore default##sysprompt")) {
        const size_t n = std::min(std::strlen(kDefaultSystemPrompt),
                                  sizeof(systemPromptBuf) - 1);
        std::memcpy(systemPromptBuf, kDefaultSystemPrompt, n);
        systemPromptBuf[n] = '\0';
    }
    ImGui::InputTextMultiline("##ai_system_prompt", systemPromptBuf,
                              sizeof(systemPromptBuf), ImVec2(-1.0f, 80.0f));

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
        ImGui::InputInt("##tokenlimit_input", &tokenLimitBuf, 0, 0);
        tokenLimitBuf = std::clamp(tokenLimitBuf, kMinLimit, kMaxLimit);
        ImGui::SameLine();
        if (ImGui::Button("-##tokenlimit")) {
            tokenLimitBuf = std::max(kMinLimit, tokenLimitBuf - kStep);
        }
        ImGui::SameLine();
        if (ImGui::Button("+##tokenlimit")) {
            tokenLimitBuf = std::min(kMaxLimit, tokenLimitBuf + kStep);
        }
        ImGui::SameLine();
        if (ImGui::Button("--##tokenlimit")) {
            tokenLimitBuf = std::max(kMinLimit, tokenLimitBuf - kStepFast);
        }
        ImGui::SameLine();
        if (ImGui::Button("++##tokenlimit")) {
            tokenLimitBuf = std::min(kMaxLimit, tokenLimitBuf + kStepFast);
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
        ImGui::InputInt("##tooltimeout_input", &executionTimeoutBuf, 0, 0);
        executionTimeoutBuf = std::clamp(executionTimeoutBuf, kMinTimeout, kMaxTimeout);
        ImGui::SameLine();
        if (ImGui::Button("-##tooltimeout")) {
            executionTimeoutBuf = std::max(kMinTimeout, executionTimeoutBuf - kStep);
        }
        ImGui::SameLine();
        if (ImGui::Button("+##tooltimeout")) {
            executionTimeoutBuf = std::min(kMaxTimeout, executionTimeoutBuf + kStep);
        }
        ImGui::SameLine();
        if (ImGui::Button("--##tooltimeout")) {
            executionTimeoutBuf = std::max(kMinTimeout, executionTimeoutBuf - kStepFast);
        }
        ImGui::SameLine();
        if (ImGui::Button("++##tooltimeout")) {
            executionTimeoutBuf = std::min(kMaxTimeout, executionTimeoutBuf + kStepFast);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(%d..%d)", kMinTimeout, kMaxTimeout);
    }

    // ---- Write-permission policy --------------------------------------
    // Off by default: every AI-requested write pops the modal. Turning
    // this on lets the AI execute memory_write / set_breakpoint /
    // remove_breakpoint / open_process without confirmation — use at
    // your own risk. The red warning below the checkbox exists so the
    // state can't be flipped by accident while skimming the panel.
    ImGui::Separator();
    ImGui::TextDisabled("AI Write Permissions");
    ImGui::Checkbox("Auto-approve AI write operations (YOLO mode)",
                    &autoApproveWritesBuf);
    if (autoApproveWritesBuf) {
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
            for (auto& kv : settingsBuffers_) {
                const std::string& pname = kv.first;
                SettingsBuffer& buf = kv.second;
                // Don't clobber an existing stored config with a blank
                // placeholder row. Rows that are fully empty represent
                // "user never filled this provider in"; leaving them
                // untouched preserves any key set in a previous session
                // or via hand-editing ai_config.json.
                if (std::string(buf.apiKey).empty() &&
                    std::string(buf.baseUrl).empty()) {
                    continue;
                }
                ProviderConfig cfg;
                cfg.apiKey = buf.apiKey;
                cfg.baseUrl = buf.baseUrl;
                cfg.model = buf.model;
                cfg.apiVersion = buf.apiVersion;
                keyStore.storeConfig(pname, cfg);
                // Push live so the next sendCompletion uses the new
                // values without a round-trip through loadFromFile (AC 11.2).
                if (AIProvider* p = registry.getProvider(pname)) {
                    p->configure(cfg);
                }
            }
            keyStore.saveToFile("ai_config.json");

            // Apply proxy configuration to the HTTP client singleton.
            ProxyConfig proxy;
            proxy.enabled = proxyEnabled_;
            proxy.host = proxyHost_;
            proxy.port = proxyPort_;
            HttpClient::getInstance().setProxy(proxy);

            // Persist general AI settings to ai_settings.json.
            settings.systemPrompt = systemPromptBuf;
            settings.tokenLimit = tokenLimitBuf;
            settings.executionTimeout = executionTimeoutBuf;
            settings.autoApproveWrites = autoApproveWritesBuf;
            // Guard against an accidentally-cleared prompt: saving an
            // empty string would leave the AI without guidance, so fall
            // back to the shipped default. The user can still explicitly
            // customise it by typing something other than empty.
            if (settings.systemPrompt.empty()) {
                settings.systemPrompt = kDefaultSystemPrompt;
            }
            settings.proxy = proxy;
            AiSettings::getInstance().set(settings);
            // Apply live.
            ToolExecutor::getInstance().setExecutionTimeout(settings.executionTimeout);
            // Push the updated system prompt + token limit into the live
            // ChatSession so the next outgoing request uses them without
            // needing to restart the window. Without this, AiSettings
            // was saved to disk but session_ kept its original values.
            session_.setSystemPrompt(settings.systemPrompt);
            session_.setTokenLimit(settings.tokenLimit);

            Gui::log("[AI Chat] settings saved");
            showSettings_ = false;
            systemPromptLoaded = false;   // reload fresh buffers on next open
            numericSettingsLoaded = false;
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120.0f, 32.0f))) {
        // Drop in-memory edits; next open reloads from ApiKeyStore.
        settingsBuffers_.clear();
        showSettings_ = false;
        systemPromptLoaded = false;
        numericSettingsLoaded = false;
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

void ChatWindow::drawProviderSettings(const std::string& providerName) {
    SettingsBuffer& buf = settingsBuffers_[providerName];

    // ---- API key (masked by default, reveal toggle, AC 10.1) ----------
    ImGui::Text("API Key:");
    ImGui::SameLine();
    ImGui::Checkbox(("Show##" + providerName).c_str(), &buf.showApiKey);
    const ImGuiInputTextFlags keyFlags =
        buf.showApiKey ? 0 : ImGuiInputTextFlags_Password;
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText(("##" + providerName + "_apikey").c_str(),
                     buf.apiKey, sizeof(buf.apiKey), keyFlags);

    // ---- Endpoint URL + reset (AC 11.1, 11.3, 11.4) -------------------
    ImGui::Text("Endpoint URL:");
    ImGui::SetNextItemWidth(-100.0f);
    ImGui::InputText(("##" + providerName + "_url").c_str(),
                     buf.baseUrl, sizeof(buf.baseUrl));
    ImGui::SameLine();
    if (ImGui::Button(("Reset##" + providerName).c_str())) {
        if (AIProvider* p = ProviderRegistry::getInstance().getProvider(providerName)) {
            copyIntoBuf(buf.baseUrl, p->getDefaultBaseUrl());
        }
    }

    // ---- Model name ----------------------------------------------------
    ImGui::Text("Model:");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText(("##" + providerName + "_model").c_str(),
                     buf.model, sizeof(buf.model));

    // ---- API version (Anthropic only) ---------------------------------
    if (providerName == "anthropic" || providerName == "claude") {
        ImGui::Text("API Version:");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText(("##" + providerName + "_ver").c_str(),
                         buf.apiVersion, sizeof(buf.apiVersion));
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
    if (keyEmpty && urlEmpty) {
        // Fully blank → treated as "not configured"; no error.
    } else if (keyEmpty) {
        buf.validationError = "API key is required when an endpoint URL is set.";
    } else if (urlEmpty) {
        buf.validationError = "Endpoint URL is required when an API key is set.";
    } else if (!isHttpsUrl(urlStr)) {
        buf.validationError = "Endpoint URL must begin with https://";
    }

    if (!buf.validationError.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ColorScheme::Error);
        ImGui::TextWrapped("%s", buf.validationError.c_str());
        ImGui::PopStyleColor();
    }
}

bool ChatWindow::validateSettings() {
    bool allOk = true;
    for (auto& kv : settingsBuffers_) {
        const SettingsBuffer& buf = kv.second;
        const std::string apiKeyStr = buf.apiKey;
        const std::string urlStr = buf.baseUrl;
        // Skip rows the user never touched — tabs for other providers
        // shouldn't block a save that only edits general settings. A
        // partially-filled row (one field empty) is still an error.
        const bool keyEmpty = apiKeyStr.empty();
        const bool urlEmpty = urlStr.empty();
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
        }
    }
    return allOk;
}

} // namespace AI

#endif // HAVE_AI_CHAT
