#pragma once
#ifdef HAVE_AI_CHAT

#include "AIProvider.h"
#include "Persistence.h"

#include <mutex>
#include <string>
#include <vector>

namespace AI {

// ChatSession owns the ordered conversation history for a single chat window.
// It is responsible for message retention (max 1000), token-limit truncation,
// and Windows-user-protected JSON persistence so sessions survive restarts
// without leaving message/tool payloads readable at rest.
//
// The class is lightly thread-safe: all public mutating/observing operations
// take an internal mutex so background completion threads can append tokens
// while the UI thread reads history. All complex work is delegated to
// `*Unlocked` helpers so methods that already hold the lock can reuse them
// without recursive locking.
class ChatSession {
public:
    ChatSession();
    ~ChatSession() = default;

    ChatSession(const ChatSession&) = delete;
    ChatSession& operator=(const ChatSession&) = delete;

    // Message management
    bool addMessage(ChatMessage msg);
    const std::vector<ChatMessage>& getMessages() const;
    void clearHistory();

    // Live copy of the global AiSettings prompt. It is never loaded from or
    // saved to a session file (format v2).
    // Maximum 4000 chars, truncated silently per AC 8.2.
    void setSystemPrompt(const std::string& prompt);
    std::string getSystemPrompt() const;

    // Live copy of the global AiSettings token budget. It is never loaded
    // from or saved to a session file (format v2).
    // Default 800000, clamped to [1000, 1000000] per AC 8.3.
    void setTokenLimit(int limit);
    int getTokenLimit() const;
    int estimateTokenCount() const;
    void truncateIfNeeded();

    // Persistence. On success, the filepath is remembered and subsequent
    // addMessage() calls auto-persist to it.
    bool save(const std::string& filepath);
    bool saveBound();
    PersistenceLoadResult load(const std::string& filepath);

    // Clear in-memory messages WITHOUT touching the persisted file or the
    // bound sessionFilePath_. Used when switching to a different chat
    // session where the current on-disk content must survive. The system
    // prompt and token limit are preserved because AiSettings owns them
    // globally; they are not per-session content.
    void resetInMemory();

    // Change the persisted file path (e.g. when switching sessions). The
    // in-memory state is left untouched; callers typically pair this
    // with resetInMemory() + load() on the new path.
    void setSessionFilePath(const std::string& filepath);

    // Build the outgoing message list for an AI request. If a system prompt
    // is set it is inserted as the first element with Role::System. Persisted
    // Role::System messages are UI/runtime notices and are not sent.
    std::vector<ChatMessage> getMessagesForRequest() const;

    // Maximum system prompt length (characters) per AC 8.2.
    static constexpr int kMaxSystemPromptChars = 4000;
    // Maximum retained messages per AC 8.1.
    static constexpr int kMaxMessages = 1000;
    // Token-limit bounds. The ceiling is intentionally generous (1M) to
    // accommodate the 1M-context models (GPT-4.1/5-tier, Claude Sonnet
    // extended-context, Gemini long-context) surfaced by the configured
    // providers. Truncation still applies once the limit is reached —
    // this just raises the knob ceiling.
    static constexpr int kMinTokenLimit = 1000;
    static constexpr int kMaxTokenLimit = 1000000;
    static constexpr int kDefaultTokenLimit = 800000;

private:
    // Unlocked helpers: callers must hold mutex_.
    int estimateTokenCountUnlocked() const;
    void truncateIfNeededUnlocked();
    bool saveUnlocked(const std::string& filepath) const;
    PersistenceLoadResult loadUnlocked(const std::string& filepath);
    void clearHistoryUnlocked();

    std::vector<ChatMessage> messages_;
    std::string systemPrompt_;
    int tokenLimit_ = kDefaultTokenLimit;
    std::string sessionFilePath_;

    mutable std::mutex mutex_;
};

} // namespace AI

#endif // HAVE_AI_CHAT
