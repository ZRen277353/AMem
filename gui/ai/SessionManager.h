#pragma once
#ifdef HAVE_AI_CHAT

#include "Persistence.h"

#include <mutex>
#include <string>
#include <vector>

namespace AI {

// Metadata for a single persisted chat session. The full message history
// lives in `ai_sessions/<id>.json`; this struct is what appears in the
// sidebar / session picker so we don't need to load every file on
// startup just to render the list.
struct SessionInfo {
    // Opaque stable identifier (milliseconds-since-epoch string). Used as
    // both the filename stem on disk and the key the UI passes back in to
    // open/delete operations.
    std::string id;

    // Human-readable title. Auto-derived from the first user message when
    // not explicitly set — see SessionManager::deriveTitle().
    std::string title;

    // Unix timestamps (seconds) for sorting and showing "last used". The
    // UI orders sessions by updatedAt descending so the most recent
    // conversation floats to the top.
    long long createdAt = 0;
    long long updatedAt = 0;

    // Cached message count from the last save. Shown as a subtle badge in
    // the session list; a best-effort estimate — ChatSession itself
    // remains the source of truth for the currently loaded session.
    int messageCount = 0;
};

// Singleton that owns the sessions directory and its index file.
//
// Layout on disk (relative to the working directory, i.e. next to the
// executable when run via `bin/ImGuiProject.exe`):
//
//   ai_sessions/
//     index.json            ← metadata list + activeId
//     <id>.json             ← one per session, matches ChatSession format
//
// Legacy migration: if `ai_session.json` exists at startup but no
// `ai_sessions/` directory, it gets moved in under a new id so users
// never lose their history.
class SessionManager {
public:
    static SessionManager& getInstance() {
        static SessionManager instance;
        return instance;
    }

    // Load the index from disk (or initialise empty). Performs legacy
    // migration exactly once. Idempotent — safe to call from every
    // ChatWindow constructor.
    PersistenceLoadResult init(
        const std::string& sessionsDir = "ai_sessions",
        const std::string& legacySessionFile = "ai_session.json");

    // All known sessions, ordered by updatedAt descending. A fresh copy
    // is returned so callers can iterate without holding the mutex.
    std::vector<SessionInfo> list() const;

    // Active session id (empty only on a brand-new install before the
    // first create()). Auto-persisted in index.json.
    std::string activeId() const;
    void setActiveId(const std::string& id);

    // Create a fresh session, persist it in the index, and make it
    // active. Returns the new id. The on-disk file is NOT written here —
    // ChatSession::save() will create it on the first addMessage().
    std::string create(const std::string& title = {});

    // Remove the session file from disk and drop it from the index. If
    // the removed session was active, the next-most-recent session (if
    // any) becomes active. Returns true when the id was known.
    bool remove(const std::string& id);

    // Rename a session in the index (persists immediately).
    bool rename(const std::string& id, const std::string& newTitle);

    // Update updatedAt / messageCount / (optional) auto-derived title
    // when the current conversation changes. Called by ChatWindow after
    // every addMessage so the sidebar stays fresh.
    void touch(const std::string& id,
               int messageCount,
               const std::string& firstUserMessage);

    // Absolute path of the session file for a given id. Returns empty
    // when the id is unknown.
    std::string pathFor(const std::string& id) const;

    // Directory in use; exposed for filesystem-level callers (e.g.
    // ChatSession).
    std::string sessionsDir() const;

    // Utility: derive a reasonable title from the first user message.
    // Trims whitespace, truncates to ~40 chars, replaces newlines with
    // spaces. Used by touch() and create().
    static std::string deriveTitle(const std::string& firstUserMessage);

private:
    SessionManager() = default;
    ~SessionManager() = default;
    SessionManager(const SessionManager&) = delete;
    SessionManager& operator=(const SessionManager&) = delete;

    // Unlocked helpers; callers must hold mutex_.
    PersistenceLoadResult loadIndexUnlocked();
    PersistenceLoadResult recoverIndexUnlocked(
        const PersistenceLoadResult& failure);
    bool saveIndexUnlocked() const;
    std::string allocIdUnlocked() const;
    std::string pathForUnlocked(const std::string& id) const;

    mutable std::mutex mutex_;
    std::string dir_ = "ai_sessions";
    std::string indexPath_; // dir_ + "/index.json"
    std::string activeId_;
    std::vector<SessionInfo> sessions_;
    bool initialized_ = false;
    bool indexWritesEnabled_ = true;
    PersistenceLoadResult lastLoadResult_;
    // True once the one-time ai_session.json → ai_sessions/ migration has been
    // handled. Persisted in index.json so a leftover legacy file can't be
    // re-imported on a later launch (e.g. after the user deletes all sessions).
    bool legacyMigrated_ = false;
};

} // namespace AI

#endif // HAVE_AI_CHAT
