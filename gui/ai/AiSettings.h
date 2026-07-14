#pragma once
#ifdef HAVE_AI_CHAT

#include "HttpClient.h"
#include "Persistence.h"

#include <mutex>
#include <string>

namespace AI {

// Human-editable (non-secret) settings for the AI chat module.
//
// Serialised to `ai_settings.json` alongside the DPAPI-encrypted
// `ai_config.json` managed by ApiKeyStore. This file is plain JSON so the
// user can hand-edit it without going through the settings panel — useful
// for scripting, dotfile dumps, or tweaking the system prompt.
//
// Schema (version 1):
//     {
//       "version": 1,
//       "activeProvider": "openai",
//       "executionTimeout": 30,
//       "maxAgentSteps": 12,
//       "maxToolCallsPerTurn": 16,
//       "tokenLimit": 16000,
//       "systemPrompt": "...",
//       "autoApproveWrites": false,
//       "autoApproveLuaExecution": true,
//       "proxy": { "enabled": false, "host": "", "port": 0 }
//     }
struct AiSettingsData {
    std::string activeProvider = "openai";
    int executionTimeout = 30;   // seconds, clamped to [1, 300]
    int maxAgentSteps = 12;      // model/tool loop iterations, clamped to [1, 64]
    int maxToolCallsPerTurn = 16;// tool calls in one assistant turn, clamped to [1, 64]
    int tokenLimit = 16000;      // clamped to [1000, 1000000]
    std::string systemPrompt;    // prepended to every request
    ProxyConfig proxy;           // host / port / enabled

    // YOLO mode for non-Lua writes. Lua has a separate permission so users
    // can keep ordinary target mutations gated independently.
    bool autoApproveWrites = false;

    // Shared by the in-app Agent and Native IPC. When true, lua_execute gets
    // a policy approval without opening the per-request confirmation UI.
    bool autoApproveLuaExecution = true;
};

class AiSettings {
public:
    static AiSettings& getInstance() {
        static AiSettings instance;
        return instance;
    }

    // Default filename sits next to ai_config.json so users know the two
    // belong together. Both default paths can be overridden by callers.
    PersistenceLoadResult loadFromFile(
        const std::string& filepath = "ai_settings.json");
    bool saveToFile(const std::string& filepath = "ai_settings.json");

    // Thread-safe snapshot / replace. Callers should avoid holding onto
    // const references returned by get() across mutations.
    AiSettingsData get() const;
    void set(const AiSettingsData& data);

    // Field-level mutators so the settings panel can update one field at a
    // time without clobbering the others. Mutators clamp numeric fields
    // and persist to disk if a non-empty path was supplied on the last
    // loadFromFile() / saveToFile() call.
    void setActiveProvider(const std::string& name);
    void setExecutionTimeout(int seconds);
    void setTokenLimit(int limit);
    void setSystemPrompt(const std::string& prompt);
    void setProxy(const ProxyConfig& proxy);
    void setAutoApproveLuaExecution(bool enabled);

    // Initialize from disk if present. Only a genuinely missing file is
    // created from defaults; invalid or unreadable files are preserved.
    PersistenceLoadResult loadOrDefault(
        const std::string& filepath = "ai_settings.json");

private:
    AiSettings() = default;
    ~AiSettings() = default;
    AiSettings(const AiSettings&) = delete;
    AiSettings& operator=(const AiSettings&) = delete;

    mutable std::mutex mutex_;
    AiSettingsData data_;
    void persistLastPath();

    std::string lastPath_;
};

} // namespace AI

#endif // HAVE_AI_CHAT
