#ifdef HAVE_AI_CHAT

#include "SessionManager.h"

#include "../../third_party/nlohmann/json.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace AI {

namespace {

constexpr int kIndexVersion = 1;

long long nowUnixSeconds() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

long long nowUnixMillis() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

// Very small subset of "safe id" enforcement: strip anything that isn't
// ASCII alphanumeric, '-', or '_'. Used only when an id somehow arrives
// from an older index file; freshly allocated ids from allocIdUnlocked()
// already fit the safe pattern.
std::string sanitizeId(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        const bool safe = std::isalnum(u) || c == '-' || c == '_';
        if (safe) out.push_back(c);
    }
    return out;
}

} // namespace

void SessionManager::init(const std::string& sessionsDir,
                          const std::string& legacySessionFile) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) return;

    dir_       = sessionsDir;
    indexPath_ = dir_ + "/index.json";

    std::error_code ec;
    std::filesystem::create_directories(dir_, ec); // non-fatal if exists

    // Load index (if any).
    loadIndexUnlocked();

    // Legacy migration: promote `ai_session.json` into the new layout
    // exactly once. We only trigger this when the index has no sessions
    // yet, so users who've already migrated don't see the old file
    // resurface in the list.
    if (sessions_.empty() && !legacySessionFile.empty()) {
        std::error_code fec;
        if (std::filesystem::exists(legacySessionFile, fec)) {
            SessionInfo info;
            info.id        = allocIdUnlocked();
            info.title     = "Imported session";
            info.createdAt = nowUnixSeconds();
            info.updatedAt = info.createdAt;

            // Try to peek at the message count so the sidebar shows a
            // meaningful badge immediately; non-fatal on parse failure.
            try {
                std::ifstream in(legacySessionFile, std::ios::binary);
                if (in.is_open()) {
                    nlohmann::json j;
                    in >> j;
                    if (j.is_object() && j.contains("messages") &&
                        j["messages"].is_array()) {
                        info.messageCount = static_cast<int>(j["messages"].size());
                    }
                }
            } catch (...) { /* ignore */ }

            const std::string destPath = pathForUnlocked(info.id);
            std::error_code mec;
            std::filesystem::rename(legacySessionFile, destPath, mec);
            if (mec) {
                // Fall back to copy+remove if rename across drives/fails.
                std::filesystem::copy_file(
                    legacySessionFile, destPath,
                    std::filesystem::copy_options::overwrite_existing, mec);
                if (!mec) {
                    std::error_code rm;
                    std::filesystem::remove(legacySessionFile, rm);
                }
            }
            if (!mec) {
                sessions_.push_back(std::move(info));
                activeId_ = sessions_.back().id;
                saveIndexUnlocked();
            }
        }
    }

    initialized_ = true;
}

std::vector<SessionInfo> SessionManager::list() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SessionInfo> copy = sessions_;
    // Most-recently-used first so the sidebar order feels natural.
    std::sort(copy.begin(), copy.end(),
              [](const SessionInfo& a, const SessionInfo& b) {
                  return a.updatedAt > b.updatedAt;
              });
    return copy;
}

std::string SessionManager::activeId() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return activeId_;
}

void SessionManager::setActiveId(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    activeId_ = id;
    saveIndexUnlocked();
}

std::string SessionManager::create(const std::string& title) {
    std::lock_guard<std::mutex> lock(mutex_);

    SessionInfo info;
    info.id        = allocIdUnlocked();
    info.title     = title.empty() ? std::string("New chat") : title;
    info.createdAt = nowUnixSeconds();
    info.updatedAt = info.createdAt;
    info.messageCount = 0;

    sessions_.push_back(info);
    activeId_ = info.id;
    saveIndexUnlocked();
    return info.id;
}

bool SessionManager::remove(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = std::find_if(sessions_.begin(), sessions_.end(),
                           [&](const SessionInfo& s) { return s.id == id; });
    if (it == sessions_.end()) return false;

    const std::string path = pathForUnlocked(id);
    std::error_code ec;
    std::filesystem::remove(path, ec); // best-effort; missing file is fine

    sessions_.erase(it);

    // If the active session was removed, fall back to the newest
    // remaining one so the window always has something to show. An empty
    // list is legal — the caller is responsible for calling create().
    if (activeId_ == id) {
        activeId_.clear();
        long long newest = 0;
        for (const auto& s : sessions_) {
            if (s.updatedAt >= newest) {
                newest = s.updatedAt;
                activeId_ = s.id;
            }
        }
    }

    saveIndexUnlocked();
    return true;
}

bool SessionManager::rename(const std::string& id, const std::string& newTitle) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& s : sessions_) {
        if (s.id == id) {
            s.title = newTitle;
            saveIndexUnlocked();
            return true;
        }
    }
    return false;
}

void SessionManager::touch(const std::string& id,
                           int messageCount,
                           const std::string& firstUserMessage) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& s : sessions_) {
        if (s.id != id) continue;
        s.updatedAt    = nowUnixSeconds();
        s.messageCount = messageCount;
        // Only auto-fill the title when the user hasn't customised it
        // yet — we detect "hasn't customised" heuristically via the
        // default "New chat" / "Imported session" labels.
        if ((s.title == "New chat" || s.title == "Imported session" || s.title.empty()) &&
            !firstUserMessage.empty()) {
            s.title = deriveTitle(firstUserMessage);
        }
        saveIndexUnlocked();
        return;
    }
}

std::string SessionManager::pathFor(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (id.empty()) return {};
    return pathForUnlocked(id);
}

std::string SessionManager::sessionsDir() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dir_;
}

std::string SessionManager::deriveTitle(const std::string& firstUserMessage) {
    // Single-line, collapsed whitespace, capped at ~40 chars so the tab
    // label fits without overflowing the selector.
    std::string out;
    out.reserve(std::min<size_t>(firstUserMessage.size(), 64));
    bool lastSpace = false;
    for (char c : firstUserMessage) {
        if (c == '\r' || c == '\n' || c == '\t') c = ' ';
        if (c == ' ') {
            if (lastSpace) continue;
            lastSpace = true;
        } else {
            lastSpace = false;
        }
        out.push_back(c);
        if (out.size() >= 40) break;
    }
    // Trim trailing whitespace left by the truncation.
    while (!out.empty() && out.back() == ' ') out.pop_back();
    if (out.empty()) return "New chat";
    if (firstUserMessage.size() > out.size()) out += "…";
    return out;
}

// ---------------------------------------------------------------------------
// Private helpers — mutex held by callers
// ---------------------------------------------------------------------------

bool SessionManager::loadIndexUnlocked() {
    sessions_.clear();
    activeId_.clear();

    std::error_code ec;
    if (!std::filesystem::exists(indexPath_, ec)) {
        return false; // fresh install
    }

    std::ifstream in(indexPath_, std::ios::binary);
    if (!in.is_open()) return false;

    nlohmann::json root;
    try {
        in >> root;
    } catch (const nlohmann::json::exception&) {
        return false;
    }
    if (!root.is_object()) return false;

    activeId_ = sanitizeId(root.value("activeId", std::string{}));
    if (root.contains("sessions") && root["sessions"].is_array()) {
        for (const auto& entry : root["sessions"]) {
            if (!entry.is_object()) continue;
            SessionInfo info;
            info.id = sanitizeId(entry.value("id", std::string{}));
            if (info.id.empty()) continue;
            info.title        = entry.value("title", std::string{});
            info.createdAt    = entry.value("createdAt", 0ll);
            info.updatedAt    = entry.value("updatedAt", 0ll);
            info.messageCount = entry.value("messageCount", 0);
            sessions_.push_back(std::move(info));
        }
    }

    // If the index referenced an active id that no longer exists, clear
    // it so the caller can pick a sane default.
    if (!activeId_.empty()) {
        auto it = std::find_if(sessions_.begin(), sessions_.end(),
                               [&](const SessionInfo& s) { return s.id == activeId_; });
        if (it == sessions_.end()) activeId_.clear();
    }
    return true;
}

bool SessionManager::saveIndexUnlocked() const {
    nlohmann::json root = nlohmann::json::object();
    root["version"]  = kIndexVersion;
    root["activeId"] = activeId_;

    nlohmann::json arr = nlohmann::json::array();
    for (const auto& s : sessions_) {
        nlohmann::json e;
        e["id"]           = s.id;
        e["title"]        = s.title;
        e["createdAt"]    = s.createdAt;
        e["updatedAt"]    = s.updatedAt;
        e["messageCount"] = s.messageCount;
        arr.push_back(std::move(e));
    }
    root["sessions"] = std::move(arr);

    // Atomic write via .tmp rename so a crash mid-save can't truncate the
    // index — same pattern used by ApiKeyStore / AiSettings.
    std::filesystem::path target(indexPath_);
    std::filesystem::path tmp = target;
    tmp += ".tmp";

    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return false;
        try {
            out << root.dump(2);
        } catch (const nlohmann::json::exception&) {
            return false;
        }
        out.flush();
        if (!out.good()) return false;
    }

    std::error_code ec;
    std::filesystem::rename(tmp, target, ec);
    if (ec) {
        std::filesystem::remove(target, ec);
        ec.clear();
        std::filesystem::rename(tmp, target, ec);
        if (ec) {
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    return true;
}

std::string SessionManager::allocIdUnlocked() const {
    // Milliseconds-since-epoch is unique in practice and sorts nicely by
    // creation order if the sidebar ever wants to fall back to id order.
    // Collision defence: if the candidate is already present (clock
    // regression, concurrent create from another window instance), we
    // append a disambiguation suffix.
    long long ms = nowUnixMillis();
    for (int attempt = 0; attempt < 1000; ++attempt) {
        std::ostringstream oss;
        oss << (ms + attempt);
        const std::string candidate = oss.str();
        auto it = std::find_if(sessions_.begin(), sessions_.end(),
                               [&](const SessionInfo& s) { return s.id == candidate; });
        if (it == sessions_.end()) return candidate;
    }
    // Extraordinarily unlikely, but fall back to a random-ish stem.
    std::ostringstream oss;
    oss << ms << "-x" << std::rand();
    return oss.str();
}

std::string SessionManager::pathForUnlocked(const std::string& id) const {
    return dir_ + "/" + id + ".json";
}

} // namespace AI

#endif // HAVE_AI_CHAT
