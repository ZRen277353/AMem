#ifdef HAVE_AI_CHAT

#include "SessionManager.h"

#include "../../third_party/nlohmann/json.hpp"
#include "../../utils/AtomicFileWrite.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <system_error>
#include <unordered_set>

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

long long fileTimestamp(const std::filesystem::path& path) {
    std::error_code ec;
    const auto fileTime = std::filesystem::last_write_time(path, ec);
    if (ec) {
        return nowUnixSeconds();
    }
    const auto systemTime =
        std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            fileTime - std::filesystem::file_time_type::clock::now() +
            std::chrono::system_clock::now());
    return std::chrono::duration_cast<std::chrono::seconds>(
        systemTime.time_since_epoch()).count();
}

bool recoverSessionInfo(const std::filesystem::path& path,
                        SessionInfo& info) {
    const std::string id = path.stem().string();
    if (id.empty() || sanitizeId(id) != id) {
        return false;
    }

    JsonDocumentLoadResult document = loadJsonDocument(path);
    if (document.result.status != PersistenceLoadStatus::Loaded ||
        !document.document.is_object()) {
        return false;
    }

    const auto messages = document.document.find("messages");
    if (messages != document.document.end() && !messages->is_array()) {
        return false;
    }

    std::string firstUserMessage;
    size_t messageCount = 0;
    if (messages != document.document.end()) {
        messageCount = messages->size();
        for (const auto& message : *messages) {
            if (!message.is_object()) {
                return false;
            }
            const auto role = message.find("role");
            const auto content = message.find("content");
            if ((role != message.end() && !role->is_string()) ||
                (content != message.end() && !content->is_string())) {
                return false;
            }
            if (firstUserMessage.empty() && role != message.end() &&
                role->get<std::string>() == "user" &&
                content != message.end()) {
                firstUserMessage = content->get<std::string>();
            }
        }
    }

    info.id = id;
    info.title = firstUserMessage.empty()
        ? std::string("Recovered session")
        : SessionManager::deriveTitle(firstUserMessage);
    info.createdAt = fileTimestamp(path);
    info.updatedAt = info.createdAt;
    info.messageCount = static_cast<int>(std::min<size_t>(
        messageCount,
        static_cast<size_t>(std::numeric_limits<int>::max())));
    return true;
}

} // namespace

PersistenceLoadResult SessionManager::init(
    const std::string& sessionsDir,
    const std::string& legacySessionFile) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) return lastLoadResult_;

    dir_       = sessionsDir;
    indexPath_ = dir_ + "/index.json";

    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    if (ec) {
        indexWritesEnabled_ = false;
        lastLoadResult_ = {
            PersistenceLoadStatus::IoError,
            "could not create sessions directory: " + ec.message()};
        initialized_ = true;
        return lastLoadResult_;
    }

    lastLoadResult_ = loadIndexUnlocked();
    if (lastLoadResult_.status == PersistenceLoadStatus::Invalid ||
        lastLoadResult_.status == PersistenceLoadStatus::Missing) {
        lastLoadResult_ = recoverIndexUnlocked(lastLoadResult_);
    } else if (lastLoadResult_.status == PersistenceLoadStatus::IoError) {
        indexWritesEnabled_ = false;
    }

    // Legacy migration: promote `ai_session.json` into the new layout exactly
    // once, tracked by the persisted `legacyMigrated_` flag rather than by an
    // empty session list — otherwise a leftover legacy file (e.g. one whose
    // removal failed below) would be re-imported every time the user empties
    // their session list.
    if (indexWritesEnabled_ && !legacyMigrated_ &&
        !legacySessionFile.empty()) {
        std::error_code fec;
        if (std::filesystem::exists(legacySessionFile, fec)) {
            SessionInfo info;
            info.id        = allocIdUnlocked();
            info.title     = "Imported session";
            info.createdAt = nowUnixSeconds();
            info.updatedAt = info.createdAt;

            // Try to peek at the message count so the sidebar shows a
            // meaningful badge immediately; non-fatal on parse failure.
            JsonDocumentLoadResult legacyDocument =
                loadJsonDocument(legacySessionFile);
            if (legacyDocument.result.status == PersistenceLoadStatus::Loaded &&
                legacyDocument.document.is_object() &&
                legacyDocument.document.contains("messages") &&
                legacyDocument.document["messages"].is_array()) {
                const size_t count =
                    legacyDocument.document["messages"].size();
                info.messageCount = static_cast<int>(std::min(
                    count,
                    static_cast<size_t>(std::numeric_limits<int>::max())));
            }

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
                // Record success so a failed removal (below) or a later restore
                // of the legacy file can't trigger a duplicate re-import.
                legacyMigrated_ = true;
                saveIndexUnlocked();
            }
            // On failure leave legacyMigrated_ false so the next launch retries.
        } else {
            // No legacy file present; persist the migrated decision so one that
            // appears later isn't imported into an existing install.
            legacyMigrated_ = true;
            saveIndexUnlocked();
        }
    }

    initialized_ = true;
    return lastLoadResult_;
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

PersistenceLoadResult SessionManager::loadIndexUnlocked() {
    JsonDocumentLoadResult document = loadJsonDocument(indexPath_);
    if (document.result.status != PersistenceLoadStatus::Loaded) {
        return document.result;
    }

    std::vector<SessionInfo> loadedSessions;
    std::string loadedActiveId;
    bool loadedLegacyMigrated = false;
    try {
        const nlohmann::json& root = document.document;
        if (!root.is_object()) {
            return {PersistenceLoadStatus::Invalid,
                    "session index root must be an object"};
        }
        if (root.contains("version") && !root["version"].is_number_integer()) {
            return {PersistenceLoadStatus::Invalid,
                    "session index 'version' must be an integer"};
        }

        const auto active = root.find("activeId");
        if (active != root.end()) {
            if (!active->is_string()) {
                return {PersistenceLoadStatus::Invalid,
                        "session index 'activeId' must be a string"};
            }
            loadedActiveId = active->get<std::string>();
            if (sanitizeId(loadedActiveId) != loadedActiveId) {
                return {PersistenceLoadStatus::Invalid,
                        "session index contains an unsafe active id"};
            }
        }

        const auto sessions = root.find("sessions");
        if (sessions != root.end()) {
            if (!sessions->is_array()) {
                return {PersistenceLoadStatus::Invalid,
                        "session index 'sessions' must be an array"};
            }

            std::unordered_set<std::string> ids;
            for (const auto& entry : *sessions) {
                if (!entry.is_object()) {
                    return {PersistenceLoadStatus::Invalid,
                            "session index entries must be objects"};
                }

                SessionInfo info;
                const auto id = entry.find("id");
                if (id == entry.end() || !id->is_string()) {
                    return {PersistenceLoadStatus::Invalid,
                            "session index entry is missing a string id"};
                }
                info.id = id->get<std::string>();
                if (info.id.empty() || sanitizeId(info.id) != info.id ||
                    !ids.insert(info.id).second) {
                    return {PersistenceLoadStatus::Invalid,
                            "session index contains an unsafe or duplicate id"};
                }

                const auto title = entry.find("title");
                const auto created = entry.find("createdAt");
                const auto updated = entry.find("updatedAt");
                const auto count = entry.find("messageCount");
                if ((title != entry.end() && !title->is_string()) ||
                    (created != entry.end() && !created->is_number_integer()) ||
                    (updated != entry.end() && !updated->is_number_integer()) ||
                    (count != entry.end() && !count->is_number_integer())) {
                    return {PersistenceLoadStatus::Invalid,
                            "session index entry fields have invalid types"};
                }
                if (title != entry.end()) info.title = title->get<std::string>();
                if (created != entry.end()) info.createdAt = created->get<long long>();
                if (updated != entry.end()) info.updatedAt = updated->get<long long>();
                if (count != entry.end()) {
                    info.messageCount = std::max(0, count->get<int>());
                }
                loadedSessions.push_back(std::move(info));
            }
        }

        const auto migrated = root.find("legacyMigrated");
        if (migrated != root.end()) {
            if (!migrated->is_boolean()) {
                return {PersistenceLoadStatus::Invalid,
                        "session index 'legacyMigrated' must be boolean"};
            }
            loadedLegacyMigrated = migrated->get<bool>();
        } else {
            loadedLegacyMigrated = !loadedSessions.empty();
        }
    } catch (const nlohmann::json::exception& error) {
        return {PersistenceLoadStatus::Invalid,
                std::string("invalid session index: ") + error.what()};
    }

    if (!loadedActiveId.empty()) {
        const auto active = std::find_if(
            loadedSessions.begin(), loadedSessions.end(),
            [&](const SessionInfo& info) { return info.id == loadedActiveId; });
        if (active == loadedSessions.end()) {
            loadedActiveId.clear();
        }
    }

    sessions_ = std::move(loadedSessions);
    activeId_ = std::move(loadedActiveId);
    legacyMigrated_ = loadedLegacyMigrated;
    return {PersistenceLoadStatus::Loaded, {}};
}

PersistenceLoadResult SessionManager::recoverIndexUnlocked(
    const PersistenceLoadResult& failure) {
    std::filesystem::path backupPath;
    if (failure.status == PersistenceLoadStatus::Invalid) {
        std::string backupError;
        if (!preserveInvalidFile(indexPath_, backupPath, backupError)) {
            indexWritesEnabled_ = false;
            return {PersistenceLoadStatus::Invalid,
                    failure.message + "; could not preserve index: " +
                        backupError};
        }
    }

    std::vector<SessionInfo> recovered;
    std::error_code ec;
    std::filesystem::directory_iterator iterator(dir_, ec);
    if (ec) {
        indexWritesEnabled_ = false;
        return {PersistenceLoadStatus::IoError,
                "could not scan sessions directory: " + ec.message()};
    }

    for (const auto& entry : iterator) {
        ec.clear();
        if (!entry.is_regular_file(ec) || ec ||
            entry.path().extension() != ".json" ||
            entry.path().filename() ==
                std::filesystem::path(indexPath_).filename()) {
            continue;
        }

        SessionInfo info;
        if (recoverSessionInfo(entry.path(), info)) {
            recovered.push_back(std::move(info));
        }
    }

    sessions_ = std::move(recovered);
    activeId_.clear();
    long long newest = std::numeric_limits<long long>::min();
    for (const auto& session : sessions_) {
        if (session.updatedAt >= newest) {
            newest = session.updatedAt;
            activeId_ = session.id;
        }
    }
    legacyMigrated_ = !sessions_.empty();

    if (failure.status == PersistenceLoadStatus::Missing && sessions_.empty()) {
        return failure;
    }

    std::string message = "rebuilt session index from " +
        std::to_string(sessions_.size()) + " session file(s)";
    if (!backupPath.empty()) {
        message += "; corrupt index preserved as " + backupPath.string();
    }
    if (!saveIndexUnlocked()) {
        message += "; rebuilt index could not be persisted";
    }
    return {PersistenceLoadStatus::Recovered, std::move(message)};
}

bool SessionManager::saveIndexUnlocked() const {
    if (!indexWritesEnabled_ || indexPath_.empty()) {
        return false;
    }
    nlohmann::json root = nlohmann::json::object();
    root["version"]        = kIndexVersion;
    root["activeId"]       = activeId_;
    root["legacyMigrated"] = legacyMigrated_;

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

    // Install the temp over the index without risking the only good copy —
    // a held-open index.json no longer leads to the whole session list being
    // deleted (see utils::installTempFile).
    return utils::installTempFile(tmp, target);
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
