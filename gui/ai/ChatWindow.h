#pragma once
#ifdef HAVE_AI_CHAT

#include "../Window.h"
#include "AgentController.h"
#include "AgentRunner.h"
#include "AIProvider.h"
#include "ChatSession.h"

#include <atomic>
#include <map>
#include <string>
#include <vector>

namespace AI {

// Main AI chat window. Inherits from the project's Window base class so it
// integrates with the standard Gui::getOrCreate<>() / main-loop draw pipeline.
//
// The class body lives in ChatWindow.cpp. To keep individual translation
// units manageable, other ChatWindow.* files in this directory contribute
// additional method definitions:
//   - ChatWindowSettings.cpp (task 8.4) — drawSettingsPanel /
//     drawProviderSettings / validateSettings
// Members marked "settings-panel state" below are public because they are
// consumed by that sibling .cpp.
class ChatWindow : public Window {
public:
    ChatWindow();
    ~ChatWindow() override;

    ChatWindow(const ChatWindow&) = delete;
    ChatWindow& operator=(const ChatWindow&) = delete;

    void onDraw() override;
    unsigned int getWindowFlags() const override;

    // ---- settings-panel state (shared with ChatWindowSettings.cpp) ------
    // Temporary editable buffer for one provider's settings. Populated on
    // demand when the settings panel opens and written back to the
    // ApiKeyStore via validateSettings() on save.
    struct SettingsBuffer {
        char apiKey[512] = {};
        char baseUrl[2049] = {};
        char model[256] = {};
        char apiVersion[64] = {};
        bool showApiKey = false;
        bool dirty = false;
        std::string validationError;
    };

    bool showSettings_ = false;
    std::map<std::string, SettingsBuffer> settingsBuffers_;

    // Proxy configuration mirrored by the settings panel. Applied to
    // HttpClient when the user saves the panel.
    char proxyHost_[256] = {};
    int proxyPort_ = 0;
    bool proxyEnabled_ = false;

    // Entry points owned by ChatWindowSettings.cpp (task 8.4). Declared
    // here so ChatWindow.cpp's onDraw() can call into them without an
    // extra forwarding header.
    void drawSettingsPanel();
    void drawProviderSettings(const std::string& providerName);
    bool validateSettings();

private:
    // ---- per-frame UI drawing ------------------------------------------
    void drawToolbar();
    void drawMessageArea();
    void drawAgentActivityPanel();
    void drawInputArea();
    void drawToolConfirmationModal();

    // ---- message rendering (task 8.2 replaces these bodies) ------------
    void renderMessage(const ChatMessage& msg, int index);
    void renderStreamingMessage();
    void renderLoadingIndicator();

    // ---- actions -------------------------------------------------------
    void sendMessage();
    void cancelRequest();
    void clearHistory();

    // ---- tool-execution flow (task 8.3 fleshes these out) --------------
    void processToolCalls(const std::vector<ToolCall>& calls);
    void handleAgentOutcome(AgentRunner::Outcome outcome);
    void sendFollowUpAfterTools();
    bool dispatchAgentRequest(const std::vector<ChatMessage>& messages,
                              const char* failureDetail);
    AgentRunner::Config makeAgentConfig() const;
    void addAgentTraceEvent(AgentTraceType type,
                            const std::string& detail = {},
                            const std::string& tool = {},
                            long long durationMs = 0);
    void appendAgentTraceEvents(std::vector<AgentTraceEvent> events);
    void clearAgentTrace();

    // ---- session switching --------------------------------------------
    // Load the session identified by `id` into session_, replacing the
    // current in-memory conversation. Empty id → start with an empty
    // conversation bound to the currently-active session path.
    void switchToSession(const std::string& id);
    // Create a new session, switch to it, and persist an empty file.
    void createNewSession();
    // Delete a session by id. If it's the active one, the next-most-
    // recent session becomes active; when no sessions remain a fresh
    // one is auto-created so the window always has something to show.
    void deleteSession(const std::string& id);
    // Draw the session picker + New/Delete buttons in the toolbar.
    void drawSessionControls();
    // Called by message-pump handlers after any append to session_ so
    // the SessionManager can update updatedAt / title / messageCount.
    void touchActiveSession();

    // ---- message pump --------------------------------------------------
    void pollMessages();

    // ---- error routing (task 9.1) --------------------------------------
    // Translates a ProviderError into a system-role chat message and resets
    // the UI to Idle (AC 12.1–12.4, 12.6–12.8). For authentication failures
    // it also opens the settings panel so the user can update the key.
    void displayErrorForCategory(const ProviderError& err);

    // ---- state ---------------------------------------------------------
    enum class State { Idle, WaitingResponse, ToolConfirmation, ToolExecuting };
    State state_ = State::Idle;

    // Input buffer for the composer. 2001 chars = 2000-char message cap
    // (AC 9.3) + null terminator.
    char inputBuf_[2001] = {};

    // Accumulates tokens delivered via UIMessageQueue::Token while a
    // streaming response is in flight. Cleared when the Completion arrives.
    std::string streamingContent_;

    // Currently selected provider / model as shown in the toolbar.
    std::string currentProvider_;
    std::string currentModel_;

    ChatSession session_;

    // Shared with the background HTTP worker: setting this to true aborts
    // the in-flight request. Reset to false before every new send.
    std::atomic<bool> cancelFlag_{false};

    bool autoScroll_ = true;
    bool userScrolledUp_ = false;
    float scrollY_ = 0.0f;
    float maxScrollY_ = 0.0f;
    int animFrame_ = 0; // loading-indicator animation counter

    // Snapshot of session_.getMessages().size() at the end of the previous
    // drawMessageArea(). Used to detect "new content since last frame" so
    // auto-scroll can re-pin to the bottom only when the history grew
    // (AC 9.8). Tracks streaming content as well to handle token arrivals
    // within a single in-flight response.
    size_t lastRenderedMessageCount_ = 0;
    size_t lastStreamingContentLen_ = 0;

    AgentRunner agentRunner_;
    AgentController agentController_;
    std::vector<AgentTraceEvent> agentTrace_;
    int maxAgentSteps_ = 12;
    int maxToolCallsPerTurn_ = 16;

    // Request-timing state. `requestStartMs_` is a steady_clock epoch in
    // milliseconds captured when a request is dispatched. The completion
    // handler uses it to
    // compute durationMs on the assistant message. 0 means "no request
    // in flight" so the live "thinking for Xs" indicator can hide.
    long long requestStartMs_ = 0;

    // Set by sendMessage() to signal drawInputArea() that it should
    // re-focus the composer on the next frame (after the buffer clear
    // has propagated through ImGui's internal widget state).
    bool refocusInput_ = false;

    // Bumped by sendMessage() to force the InputTextMultiline widget to
    // regenerate — its ImGui id embeds this counter, so incrementing
    // the value makes ImGui treat it as a fresh widget and drop the
    // cached edit buffer that would otherwise resurrect the sent text.
    unsigned int inputGeneration_ = 0;

    // ---- multi-session state -------------------------------------------
    // Id of the currently loaded session. Empty only during the very
    // first frame of a fresh install, before createNewSession() runs.
    std::string activeSessionId_;
    // Pending id used by a two-step delete confirmation popup so the
    // user can back out without losing data.
    std::string deleteConfirmId_;
    bool showDeleteConfirm_ = false;
    // Buffer + visibility flag for the rename popup.
    bool showRenameDialog_ = false;
    char renameBuf_[128] = {};
    std::string renameTargetId_;

    static constexpr const char* kSessionFile = "ai_session.json"; // legacy — migrated by SessionManager
    static constexpr const char* kConfigFile = "ai_config.json";
    static constexpr const char* kSettingsFile = "ai_settings.json";
    static constexpr const char* kLegacyConfigFile = "ai_config.dat";
    static constexpr const char* kSessionsDir = "ai_sessions";
};

} // namespace AI

#endif // HAVE_AI_CHAT
