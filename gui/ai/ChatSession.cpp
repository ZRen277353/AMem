#ifdef HAVE_AI_CHAT

#include "ChatSession.h"
#include "ToolCallSecurity.h"

#include "../Gui.h"
#include "../../third_party/nlohmann/json.hpp"
#include "../../utils/AtomicFileWrite.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <unordered_set>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace AI {

namespace {

// ---- Role <-> string helpers -----------------------------------------------

const char* roleToString(Role r) {
    switch (r) {
        case Role::System:    return "system";
        case Role::User:      return "user";
        case Role::Assistant: return "assistant";
        case Role::Tool:      return "tool";
    }
    return "user";
}

Role stringToRole(const std::string& s, Role fallback = Role::User) {
    if (s == "system")    return Role::System;
    if (s == "user")      return Role::User;
    if (s == "assistant") return Role::Assistant;
    if (s == "tool")      return Role::Tool;
    return fallback;
}

bool isRetiredToolName(const std::string& name) {
    static const std::unordered_set<std::string> retired = {
        "get_status", "get_server_version", "get_architecture",
        "init_driver", "read_memory", "read_value", "write_bytes",
        "write_value", "scan_set_range", "scan_value", "scan_next",
        "scan_fuzzy", "scan_hex", "get_scan_count", "get_scan_results",
        "clear_scan", "get_module_list", "list_modules", "get_module_base",
        "get_process_list", "list_processes", "open_process",
        "resolve_offset_chain", "read_disassembly", "set_breakpoint",
        "remove_breakpoint", "read_breakpoint_info", "suspend_breakpoint",
        "resume_breakpoint", "resolve_symbol", "symbol_init", "symbol_find",
        "execute_lua",
    };
    return retired.find(name) != retired.end();
}

bool hasRetiredToolCall(const std::vector<ToolCall>& calls) {
    return std::any_of(calls.begin(), calls.end(),
        [](const ToolCall& call) { return isRetiredToolName(call.name); });
}

ChatMessage historicalToolGroup(
    const ChatMessage& assistant,
    const std::vector<ChatMessage>& toolResults) {
    ChatMessage history;
    history.role = Role::Assistant;
    history.timestamp = assistant.timestamp;
    history.durationMs = assistant.durationMs;

    std::ostringstream text;
    if (!assistant.content.empty()) {
        text << assistant.content << "\n\n";
    }
    text << "Historical tool execution (retired tools are not available for new calls):";
    for (const auto& call : assistant.toolCalls) {
        text << "\n\nTool: " << call.name;
        const std::string& arguments = toolCallArgumentsForDisplay(call);
        if (!arguments.empty()) {
            text << "\nArguments: " << arguments;
        }
        const auto result = std::find_if(
            toolResults.begin(), toolResults.end(),
            [&call](const ChatMessage& message) {
                return message.toolCallId == call.id;
            });
        if (result != toolResults.end()) {
            text << "\nResult: " << result->content;
        } else {
            text << "\nResult: [not recorded]";
        }
    }
    history.content = text.str();
    return history;
}

// ---- JSON (de)serialization of a single message ----------------------------

nlohmann::json messageToJson(const ChatMessage& msg) {
    nlohmann::json j;
    j["role"]    = roleToString(msg.role);
    j["content"] = msg.content;
    if (!msg.toolCalls.empty()) {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& tc : msg.toolCalls) {
            nlohmann::json tj;
            tj["id"]        = tc.id;
            tj["name"]      = tc.name;
            tj["arguments"] = toolCallArgumentsForDisplay(tc);
            arr.push_back(std::move(tj));
        }
        j["toolCalls"] = std::move(arr);
    }
    if (!msg.toolCallId.empty()) j["toolCallId"] = msg.toolCallId;
    if (!msg.name.empty())       j["name"]       = msg.name;
    if (msg.timestamp != 0)      j["timestamp"]  = msg.timestamp;
    if (msg.durationMs != 0)     j["durationMs"] = msg.durationMs;
    return j;
}

ChatMessage messageFromJson(const nlohmann::json& j) {
    ChatMessage msg;
    if (j.contains("role") && j["role"].is_string()) {
        msg.role = stringToRole(j["role"].get<std::string>());
    }
    if (j.contains("content") && j["content"].is_string()) {
        msg.content = j["content"].get<std::string>();
    }
    if (j.contains("toolCalls") && j["toolCalls"].is_array()) {
        for (const auto& tj : j["toolCalls"]) {
            ToolCall tc;
            if (tj.contains("id") && tj["id"].is_string())
                tc.id = tj["id"].get<std::string>();
            if (tj.contains("name") && tj["name"].is_string())
                tc.name = tj["name"].get<std::string>();
            if (tj.contains("arguments") && tj["arguments"].is_string())
                tc.arguments = tj["arguments"].get<std::string>();
            msg.toolCalls.push_back(std::move(tc));
        }
    }
    if (j.contains("toolCallId") && j["toolCallId"].is_string())
        msg.toolCallId = j["toolCallId"].get<std::string>();
    if (j.contains("name") && j["name"].is_string())
        msg.name = j["name"].get<std::string>();
    if (j.contains("timestamp") && j["timestamp"].is_number_integer())
        msg.timestamp = j["timestamp"].get<long long>();
    if (j.contains("durationMs") && j["durationMs"].is_number_integer())
        msg.durationMs = j["durationMs"].get<long long>();
    return msg;
}

bool validateMessageJson(const nlohmann::json& message,
                         std::string& error) {
    if (!message.is_object()) {
        error = "message entries must be objects";
        return false;
    }

    const auto role = message.find("role");
    const auto content = message.find("content");
    const auto toolCallId = message.find("toolCallId");
    const auto name = message.find("name");
    const auto timestamp = message.find("timestamp");
    const auto duration = message.find("durationMs");
    if ((role != message.end() && !role->is_string()) ||
        (content != message.end() && !content->is_string()) ||
        (toolCallId != message.end() && !toolCallId->is_string()) ||
        (name != message.end() && !name->is_string()) ||
        (timestamp != message.end() && !timestamp->is_number_integer()) ||
        (duration != message.end() && !duration->is_number_integer())) {
        error = "message fields have invalid types";
        return false;
    }

    if (role != message.end()) {
        const std::string value = role->get<std::string>();
        if (value != "system" && value != "user" &&
            value != "assistant" && value != "tool") {
            error = "message has an unknown role";
            return false;
        }
    }

    const auto toolCalls = message.find("toolCalls");
    if (toolCalls == message.end()) {
        return true;
    }
    if (!toolCalls->is_array()) {
        error = "message 'toolCalls' must be an array";
        return false;
    }
    for (const auto& call : *toolCalls) {
        if (!call.is_object()) {
            error = "tool call entries must be objects";
            return false;
        }
        for (const char* fieldName : {"id", "name", "arguments"}) {
            const auto field = call.find(fieldName);
            if (field != call.end() && !field->is_string()) {
                error = "tool call fields must be strings";
                return false;
            }
        }
    }
    return true;
}

bool eraseOldestConversationGroup(std::vector<ChatMessage>& messages) {
    if (messages.size() <= 1) {
        return false;
    }

    // Prefer trimming at user-turn boundaries. A user turn owns every
    // assistant/tool/system message until the next user message, so removing
    // the whole group keeps assistant tool_calls adjacent to their tool
    // results instead of leaving orphan tool messages in the request.
    std::size_t eraseEnd = messages.size();
    const bool startsWithUser = messages.front().role == Role::User;
    for (std::size_t i = 1; i < messages.size(); ++i) {
        if (messages[i].role == Role::User) {
            eraseEnd = i;
            break;
        }
    }

    if (startsWithUser && eraseEnd == messages.size()) {
        // Only the newest user turn remains. Keep it even if it is still
        // over budget, matching the existing "preserve latest user input"
        // contract.
        return false;
    }

    if (!startsWithUser && eraseEnd == messages.size()) {
        // Legacy/corrupt histories may have no user boundary. Drop the
        // oldest standalone message as a best-effort fallback.
        eraseEnd = 1;
    }

    if (eraseEnd == 0 || eraseEnd > messages.size()) {
        return false;
    }

    messages.erase(messages.begin(),
                   messages.begin() + static_cast<std::ptrdiff_t>(eraseEnd));
    return true;
}

} // namespace

// ---- ChatSession -----------------------------------------------------------

ChatSession::ChatSession() = default;

void ChatSession::addMessage(ChatMessage msg) {
    std::string persistPath;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        messages_.push_back(std::move(msg));

        // Enforce hard message cap (AC 8.1) by dropping complete oldest
        // conversation groups. This avoids splitting assistant tool_calls
        // from their tool result messages.
        while (messages_.size() > static_cast<size_t>(kMaxMessages)) {
            if (!eraseOldestConversationGroup(messages_)) {
                break;
            }
        }

        // Enforce token-limit truncation (AC 8.3).
        truncateIfNeededUnlocked();

        persistPath = sessionFilePath_;
    }

    // Auto-persist outside the lock to avoid holding it during file I/O.
    // We grab the path under the lock, then re-lock inside save() for the
    // actual serialization snapshot.
    if (!persistPath.empty()) {
        save(persistPath);
    }
}

const std::vector<ChatMessage>& ChatSession::getMessages() const {
    // NOTE: returns a reference; callers are expected to read from the UI
    // thread only. The mutex exists for robustness against background writes
    // but cannot protect lifetime of the returned reference. This matches
    // the design's "ChatWindow owns ChatSession on the main thread" model.
    return messages_;
}

void ChatSession::clearHistory() {
    std::lock_guard<std::mutex> lock(mutex_);
    clearHistoryUnlocked();
}

void ChatSession::resetInMemory() {
    // Drop in-memory content but leave both the persisted file and the
    // sessionFilePath_ binding alone — the caller is about to load a
    // different session's file and we must not clobber the previous
    // session's on-disk history.
    std::lock_guard<std::mutex> lock(mutex_);
    messages_.clear();
}

void ChatSession::setSessionFilePath(const std::string& filepath) {
    std::lock_guard<std::mutex> lock(mutex_);
    sessionFilePath_ = filepath;
}

void ChatSession::clearHistoryUnlocked() {
    messages_.clear();

    // Delete the persisted session file so history truly goes away (AC 8.4).
    if (!sessionFilePath_.empty()) {
        std::error_code ec;
        std::filesystem::path p(sessionFilePath_);
        if (std::filesystem::exists(p, ec)) {
            std::filesystem::remove(p, ec);
            if (ec) {
                Gui::log("[ChatSession] failed to delete session file '%s': %s",
                         sessionFilePath_.c_str(), ec.message().c_str());
            }
        }
    }
}

void ChatSession::setSystemPrompt(const std::string& prompt) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Silently clamp to kMaxSystemPromptChars per AC 8.2 ("maximum 4000 chars").
    if (prompt.size() > static_cast<size_t>(kMaxSystemPromptChars)) {
        systemPrompt_ = prompt.substr(0, static_cast<size_t>(kMaxSystemPromptChars));
    } else {
        systemPrompt_ = prompt;
    }
}

std::string ChatSession::getSystemPrompt() const {
    // Return by value under the lock: a reference into systemPrompt_ could
    // dangle if setSystemPrompt() reassigns the string concurrently.
    std::lock_guard<std::mutex> lock(mutex_);
    return systemPrompt_;
}

void ChatSession::setTokenLimit(int limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Clamp to [kMinTokenLimit, kMaxTokenLimit]. Ceiling was raised to
    // 1,000,000 so the UI knob can target long-context models; the
    // original 200,000 cap predated the 1M-token providers.
    if (limit < kMinTokenLimit)      limit = kMinTokenLimit;
    else if (limit > kMaxTokenLimit) limit = kMaxTokenLimit;
    tokenLimit_ = limit;
    truncateIfNeededUnlocked();
}

int ChatSession::getTokenLimit() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tokenLimit_;
}

int ChatSession::estimateTokenCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return estimateTokenCountUnlocked();
}

int ChatSession::estimateTokenCountUnlocked() const {
    // AC 8.8: total character count / 4.
    // We include the system prompt because it is prepended to every request
    // and therefore consumes context tokens, and include tool_call fields
    // because they are serialized into the outgoing payload.
    std::size_t chars = systemPrompt_.size();
    for (const auto& m : messages_) {
        if (m.role == Role::System) {
            continue;
        }
        chars += m.content.size();
        chars += m.toolCallId.size();
        chars += m.name.size();
        for (const auto& tc : m.toolCalls) {
            chars += tc.id.size();
            chars += tc.name.size();
            chars += tc.arguments.size();
        }
    }
    const std::size_t tokens = chars / 4;
    if (tokens > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return std::numeric_limits<int>::max();
    }
    return static_cast<int>(tokens);
}

void ChatSession::truncateIfNeeded() {
    std::lock_guard<std::mutex> lock(mutex_);
    truncateIfNeededUnlocked();
}

void ChatSession::truncateIfNeededUnlocked() {
    // AC 8.3: remove oldest complete conversation groups until within the
    // token limit, preserving the most recent user turn. The system prompt
    // is stored outside messages_ so it is inherently preserved.
    if (messages_.empty()) return;

    while (messages_.size() > 1 && estimateTokenCountUnlocked() > tokenLimit_) {
        if (!eraseOldestConversationGroup(messages_)) {
            break;
        }
    }
}

std::vector<ChatMessage> ChatSession::getMessagesForRequest() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ChatMessage> out;
    out.reserve(messages_.size() + (systemPrompt_.empty() ? 0 : 1));

    if (!systemPrompt_.empty()) {
        ChatMessage sys;
        sys.role = Role::System;
        sys.content = systemPrompt_;
        out.push_back(std::move(sys));
    }
    for (std::size_t i = 0; i < messages_.size(); ++i) {
        const ChatMessage& m = messages_[i];
        // Persisted Role::System messages are UI/runtime notices such as
        // cancellation and provider errors. The configured systemPrompt_ is
        // the only system instruction that should be sent to the model.
        if (m.role == Role::System) {
            continue;
        }
        if (m.role == Role::Assistant && !m.toolCalls.empty()) {
            ChatMessage assistant = m;
            assistant.toolCalls.erase(
                std::remove_if(assistant.toolCalls.begin(),
                               assistant.toolCalls.end(),
                               [](const ToolCall& tc) {
                                   return tc.id.empty() || tc.name.empty();
                               }),
                assistant.toolCalls.end());
            if (assistant.toolCalls.empty()) {
                assistant.toolCalls.clear();
                if (!assistant.content.empty()) {
                    out.push_back(std::move(assistant));
                }
                continue;
            }

            std::unordered_set<std::string> requiredToolIds;
            for (const auto& tc : assistant.toolCalls) {
                requiredToolIds.insert(tc.id);
            }
            if (requiredToolIds.size() != assistant.toolCalls.size()) {
                assistant.toolCalls.clear();
                if (!assistant.content.empty()) {
                    out.push_back(std::move(assistant));
                }
                continue;
            }

            std::vector<ChatMessage> toolResults;
            std::size_t nextIndex = i + 1;
            bool completeToolGroup = false;
            for (; nextIndex < messages_.size(); ++nextIndex) {
                const ChatMessage& next = messages_[nextIndex];
                if (next.role == Role::System) {
                    continue;
                }
                if (next.role != Role::Tool) {
                    break;
                }
                if (next.toolCallId.empty()) {
                    continue;
                }
                const auto erased = requiredToolIds.erase(next.toolCallId);
                if (erased == 0) {
                    continue;
                }
                toolResults.push_back(next);
                if (requiredToolIds.empty()) {
                    completeToolGroup = true;
                    ++nextIndex;
                    break;
                }
            }

            if (hasRetiredToolCall(assistant.toolCalls)) {
                out.push_back(historicalToolGroup(assistant, toolResults));
                if (completeToolGroup) {
                    i = nextIndex - 1;
                }
            } else if (completeToolGroup) {
                out.push_back(std::move(assistant));
                out.insert(out.end(), toolResults.begin(), toolResults.end());
                i = nextIndex - 1;
            } else {
                // Historical interrupted/corrupt runs may contain an assistant
                // tool_call without all corresponding tool results. Sending
                // that group would violate chat-completions protocol, so keep
                // only the text portion when available.
                assistant.toolCalls.clear();
                if (!assistant.content.empty()) {
                    out.push_back(std::move(assistant));
                }
            }
            continue;
        }
        if (m.role == Role::Tool) {
            continue;
        }
        out.push_back(m);
    }
    return out;
}

// ---- Persistence -----------------------------------------------------------

bool ChatSession::save(const std::string& filepath) {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool ok = saveUnlocked(filepath);
    if (ok) {
        sessionFilePath_ = filepath;
    }
    return ok;
}

bool ChatSession::saveBound() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (sessionFilePath_.empty()) {
        return false;
    }
    return saveUnlocked(sessionFilePath_);
}

bool ChatSession::saveUnlocked(const std::string& filepath) const {
    nlohmann::json root;
    root["version"]      = 1;
    root["systemPrompt"] = systemPrompt_;
    root["tokenLimit"]   = tokenLimit_;

    nlohmann::json arr = nlohmann::json::array();
    for (const auto& m : messages_) {
        arr.push_back(messageToJson(m));
    }
    root["messages"] = std::move(arr);

    // Write atomically: serialize to `<filepath>.tmp`, then rename over the
    // target. This prevents a crash mid-write from leaving a truncated JSON
    // file that would later be rejected by load().
    const std::filesystem::path targetPath(filepath);
    std::filesystem::path tmpPath = targetPath;
    tmpPath += ".tmp";

    try {
        std::error_code mkec;
        if (targetPath.has_parent_path()) {
            std::filesystem::create_directories(targetPath.parent_path(), mkec);
            // create_directories failing is non-fatal here; the subsequent
            // ofstream open will report the real error if the dir is missing.
        }

        {
            std::ofstream ofs(tmpPath, std::ios::binary | std::ios::trunc);
            if (!ofs.is_open()) {
                Gui::log("[ChatSession] failed to open '%s' for writing",
                         tmpPath.string().c_str());
                return false;
            }
            ofs << root.dump(2);
            if (!ofs.good()) {
                Gui::log("[ChatSession] write failed for '%s'",
                         tmpPath.string().c_str());
                return false;
            }
        }

        if (!utils::installTempFile(tmpPath, targetPath)) {
            Gui::log("[ChatSession] failed to atomically install '%s'",
                     targetPath.string().c_str());
            return false;
        }
    } catch (const std::exception& e) {
        Gui::log("[ChatSession] exception while saving '%s': %s",
                 filepath.c_str(), e.what());
        std::error_code rmec;
        std::filesystem::remove(tmpPath, rmec);
        return false;
    }

    return true;
}

PersistenceLoadResult ChatSession::load(const std::string& filepath) {
    std::lock_guard<std::mutex> lock(mutex_);
    return loadUnlocked(filepath);
}

PersistenceLoadResult ChatSession::loadUnlocked(const std::string& filepath) {
    JsonDocumentLoadResult document = loadJsonDocument(filepath);
    if (document.result.status == PersistenceLoadStatus::Missing) {
        messages_.clear();
        sessionFilePath_ = filepath;
        return document.result;
    }
    if (document.result.failed()) {
        Gui::log("[ChatSession] %s session '%s': %s",
                 persistenceLoadStatusName(document.result.status),
                 filepath.c_str(), document.result.message.c_str());
        return document.result;
    }

    std::vector<ChatMessage> loadedMessages;
    std::string loadedPrompt = systemPrompt_;
    int loadedTokenLimit = tokenLimit_;
    try {
        const nlohmann::json& root = document.document;
        if (!root.is_object()) {
            throw std::runtime_error("root is not a JSON object");
        }

        const auto prompt = root.find("systemPrompt");
        if (prompt != root.end()) {
            if (!prompt->is_string()) {
                throw std::runtime_error("systemPrompt must be a string");
            }
            loadedPrompt = prompt->get<std::string>();
            if (loadedPrompt.size() >
                static_cast<size_t>(kMaxSystemPromptChars)) {
                loadedPrompt.resize(static_cast<size_t>(kMaxSystemPromptChars));
            }
        }

        const auto tokenLimit = root.find("tokenLimit");
        if (tokenLimit != root.end()) {
            if (!tokenLimit->is_number_integer()) {
                throw std::runtime_error("tokenLimit must be an integer");
            }
            loadedTokenLimit = tokenLimit->get<int>();
            if (loadedTokenLimit < kMinTokenLimit) {
                loadedTokenLimit = kMinTokenLimit;
            } else if (loadedTokenLimit > kMaxTokenLimit) {
                loadedTokenLimit = kMaxTokenLimit;
            }
        }

        const auto messages = root.find("messages");
        if (messages != root.end()) {
            if (!messages->is_array()) {
                throw std::runtime_error("messages must be an array");
            }
            const auto& arr = *messages;
            loadedMessages.reserve(
                std::min(arr.size(), static_cast<size_t>(kMaxMessages)));
            for (const auto& mj : arr) {
                std::string validationError;
                if (!validateMessageJson(mj, validationError)) {
                    throw std::runtime_error(validationError);
                }
                loadedMessages.push_back(messageFromJson(mj));
            }
            while (loadedMessages.size() >
                   static_cast<size_t>(kMaxMessages)) {
                if (!eraseOldestConversationGroup(loadedMessages)) {
                    break;
                }
            }
        }
    } catch (const std::exception& e) {
        Gui::log("[ChatSession] malformed session in '%s' (%s); preserving current state",
                 filepath.c_str(), e.what());
        return {PersistenceLoadStatus::Invalid, e.what()};
    }

    messages_ = std::move(loadedMessages);
    systemPrompt_ = std::move(loadedPrompt);
    tokenLimit_ = loadedTokenLimit;
    sessionFilePath_ = filepath;
    return {PersistenceLoadStatus::Loaded, {}};
}

} // namespace AI

#endif // HAVE_AI_CHAT
