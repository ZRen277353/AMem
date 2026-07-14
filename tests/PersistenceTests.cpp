#include "gui/Gui.h"
#include "gui/ai/AiLimits.h"
#include "gui/ai/AiSettings.h"
#include "gui/ai/ApiKeyStore.h"
#include "gui/ai/ChatSettingsDraft.h"
#include "gui/ai/ChatSession.h"
#include "gui/ai/ProtectedPersistence.h"
#include "gui/ai/SessionManager.h"
#include "utils/AtomicFileWrite.h"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

class Window {};

namespace Gui {
std::list<std::unique_ptr<Window>> windows;
std::list<std::pair<std::string, int>> logs;
std::mutex logsMutex;
} // namespace Gui

namespace {

using AI::PersistenceLoadStatus;
using nlohmann::json;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class TempDirectory {
public:
    explicit TempDirectory(const std::string& label) {
        const auto serial = std::chrono::steady_clock::now()
            .time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            ("amem-" + label + "-" + std::to_string(serial));
        std::error_code ec;
        std::filesystem::create_directories(path_, ec);
        expect(!ec, "could not create test directory: " + ec.message());
    }

    ~TempDirectory() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

void writeText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    expect(output.is_open(), "could not open test file for writing");
    output << text;
    output.flush();
    expect(output.good(), "could not write test file");
}

void writeSparseFile(const std::filesystem::path& path, size_t size) {
    expect(size > 0, "sparse test file must not be empty");
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    expect(output.is_open(), "could not open sparse test file for writing");
    output.seekp(static_cast<std::streamoff>(size - 1));
    output.put('x');
    output.flush();
    expect(output.good(), "could not write sparse test file");
}

std::string readText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    expect(input.is_open(), "could not open test file for reading");
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

json readProtectedJson(
    const std::filesystem::path& path,
    AI::ProtectedPersistenceKind kind) {
    const AI::ProtectedJsonDocumentLoadResult loaded =
        AI::loadProtectedJsonDocument(path, kind);
    expect(loaded.result.usable() &&
               loaded.result.status != PersistenceLoadStatus::Missing,
           "could not load protected JSON: " + loaded.result.message);
    return loaded.document;
}

json sessionDocument(const std::string& firstUser) {
    return {
        {"version", 1},
        {"systemPrompt", "prompt"},
        {"tokenLimit", 16000},
        {"messages", json::array({
            {{"role", "user"}, {"content", firstUser}},
            {{"role", "assistant"}, {"content", "answer"}},
        })},
    };
}

AI::ChatMessage chatMessage(AI::Role role, const std::string& content) {
    AI::ChatMessage message;
    message.role = role;
    message.content = content;
    return message;
}

class ScriptedAtomicFileOps {
public:
    explicit ScriptedAtomicFileOps(std::vector<bool> renameResults)
        : renameResults_(std::move(renameResults)) {}

    bool rename(const std::filesystem::path& from,
                const std::filesystem::path& to) {
        const bool allowed = renameIndex_ < renameResults_.size()
            ? renameResults_[renameIndex_++]
            : true;
        if (!allowed || files_.find(from.string()) == files_.end()) {
            return false;
        }
        files_[to.string()] = files_[from.string()];
        files_.erase(from.string());
        return true;
    }

    bool exists(const std::filesystem::path& path) const {
        return files_.find(path.string()) != files_.end();
    }

    void remove(const std::filesystem::path& path) {
        files_.erase(path.string());
    }

    void put(const std::filesystem::path& path, std::string content) {
        files_[path.string()] = std::move(content);
    }

    std::string get(const std::filesystem::path& path) const {
        const auto found = files_.find(path.string());
        return found == files_.end() ? std::string{} : found->second;
    }

private:
    std::map<std::string, std::string> files_;
    std::vector<bool> renameResults_;
    size_t renameIndex_ = 0;
};

void testCorruptIndexRecovery() {
    TempDirectory temp("index-recovery");
    const auto index = temp.path() / "index.json";
    const std::string corrupt = R"({"sessions":"wrong-type"})";
    writeText(index, corrupt);
    writeText(temp.path() / "session_one.json",
              sessionDocument("first recovered chat").dump());
    writeText(temp.path() / "session_two.json",
              sessionDocument("second recovered chat").dump());
    writeText(temp.path() / "broken.json", "{not-json");
    const std::string staleBackupSecret = "stale plaintext session backup";
    writeText(temp.path() / "orphan.json.bak", staleBackupSecret);

    AI::SessionManager& manager = AI::SessionManager::getInstance();
    const auto result = manager.init(temp.path().string(), {});
    expect(result.status == PersistenceLoadStatus::Recovered,
           "corrupt index should be recovered from session files");

    const auto sessions = manager.list();
    expect(sessions.size() == 2,
           "recovery should retain only two valid session files (got " +
               std::to_string(sessions.size()) + ")");
    expect(!manager.activeId().empty(),
           "recovery should select an active session");
    const auto corruptBackup = index.string() + ".corrupt";
    expect(readText(corruptBackup) == corrupt,
           "invalid index bytes must be preserved before replacement");

    expect(AI::isProtectedPersistenceFile(
               index, AI::ProtectedPersistenceKind::SessionIndex) &&
               AI::isProtectedPersistenceFile(
                   temp.path() / "session_one.json",
                   AI::ProtectedPersistenceKind::Session) &&
               AI::isProtectedPersistenceFile(
                   temp.path() / "session_two.json",
                   AI::ProtectedPersistenceKind::Session),
           "all valid session artifacts must be protected at startup");
    expect(readText(temp.path() / "broken.json") == "{not-json" &&
               readText(temp.path() / "orphan.json.bak") ==
                   staleBackupSecret,
           "invalid legacy artifacts must remain byte-for-byte unchanged");
    const json rebuilt = readProtectedJson(
        index, AI::ProtectedPersistenceKind::SessionIndex);
    expect(rebuilt["sessions"].is_array() &&
               rebuilt["sessions"].size() == 2,
           "rebuilt index should persist recovered metadata");
    expect(readText(index).find("first recovered chat") == std::string::npos,
           "session titles must not remain plaintext in the index");
}

void testApiKeyLoadIsTransactional() {
    TempDirectory temp("api-config");
    AI::ApiKeyStore& store = AI::ApiKeyStore::getInstance();

    AI::ProviderConfig config;
    config.apiKey = "test-secret";
    config.baseUrl = "https://example.invalid/v1";
    config.model = "test-model";
    expect(store.storeConfig("openai", config),
           "DPAPI should encrypt a test provider key");

    const auto malformed = temp.path() / "malformed.json";
    writeText(malformed, "{not-json");
    const auto malformedResult = store.loadFromFile(malformed.string());
    expect(malformedResult.status == PersistenceLoadStatus::Invalid &&
               store.hasConfig("openai") &&
               readText(malformed) == "{not-json",
           "malformed config must preserve memory and disk state");

    const auto wrongType = temp.path() / "wrong-type.json";
    writeText(wrongType,
              R"({"providers":{"openai":{"apiKey":[]}}})");
    const auto typeResult = store.loadFromFile(wrongType.string());
    expect(typeResult.status == PersistenceLoadStatus::Invalid &&
               store.hasConfig("openai"),
           "wrong provider field types must not escape or clear the store");

    const auto invalidContext = temp.path() / "invalid-context.json";
    writeText(invalidContext,
              R"({"version":2,"providers":{"openai":{"apiKey":"blob","contextWindowTokens":2000001}}})");
    const auto contextResult = store.loadFromFile(invalidContext.string());
    expect(contextResult.status == PersistenceLoadStatus::Invalid &&
               store.hasConfig("openai"),
           "invalid context override must preserve the prior provider store");
}

void testAtomicInstallFaultRecovery() {
    const std::filesystem::path target = "state.json";
    const std::filesystem::path temporary = "state.json.tmp";
    const std::filesystem::path backup = "state.json.bak";

    ScriptedAtomicFileOps rejected({false, true, false, true});
    rejected.put(target, "original");
    rejected.put(temporary, "replacement");
    expect(!utils::detail::installTempFileWithOps(
               temporary, target, rejected) &&
               rejected.get(target) == "original" &&
               !rejected.exists(temporary) &&
               !rejected.exists(backup),
           "failed fallback install must restore the only good target");

    ScriptedAtomicFileOps accepted({false, true, true});
    accepted.put(target, "original");
    accepted.put(temporary, "replacement");
    accepted.put(backup, "stale backup");
    expect(utils::detail::installTempFileWithOps(
               temporary, target, accepted) &&
               accepted.get(target) == "replacement" &&
               !accepted.exists(temporary) &&
               !accepted.exists(backup),
           "successful fallback install must commit new data and remove backup");
}

void testProviderConfigChangesAreAtomic() {
    TempDirectory temp("provider-config-changes");
    AI::ApiKeyStore& store = AI::ApiKeyStore::getInstance();
    const auto resetPath = temp.path() / "missing-reset.json";
    expect(store.loadFromFile(resetPath.string()).status ==
               PersistenceLoadStatus::Missing,
           "missing config fixture should reset the singleton store");

    AI::ProviderConfig openai;
    openai.apiKey = "old-openai-secret";
    openai.baseUrl = "https://old.example.invalid/v1";
    openai.model = "old-model";
    AI::ProviderConfig anthropic;
    anthropic.apiKey = "anthropic-secret";
    anthropic.baseUrl = "https://api.anthropic.com/v1";
    anthropic.model = "claude-test";
    expect(store.storeConfig("openai", openai) &&
               store.storeConfig("anthropic", anthropic),
           "could not seed provider change fixture");

    const auto configPath = temp.path() / "ai_config.json";
    expect(store.saveToFile(configPath.string()),
           "could not persist provider change baseline");

    std::vector<AI::ProviderConfigChange> changes;
    AI::ProviderConfigChange update;
    update.providerName = "openai";
    update.config.apiKey = "new-openai-secret";
    update.config.baseUrl = "https://new.example.invalid/v1";
    update.config.model = "new-model";
    update.config.trustedBaseUrl = "https://new.example.invalid/v1";
    update.config.contextWindowTokens = 32768;
    changes.push_back(std::move(update));
    AI::ProviderConfigChange remove;
    remove.providerName = "anthropic";
    remove.remove = true;
    changes.push_back(std::move(remove));

    expect(store.applyChangesAndSave(changes, configPath.string()),
           "provider update/removal should commit as one snapshot");
    AI::ProviderConfig loaded;
    expect(store.loadConfig("openai", loaded) &&
               loaded.apiKey == "new-openai-secret" &&
               loaded.baseUrl == "https://new.example.invalid/v1" &&
               loaded.model == "new-model" &&
               loaded.trustedBaseUrl ==
                   "https://new.example.invalid/v1" &&
               loaded.contextWindowTokens == 32768 &&
               !store.hasConfig("anthropic"),
           "committed provider snapshot does not match requested changes");
    const json persisted = json::parse(readText(configPath));
    expect(persisted["version"] == 3 &&
               persisted["providers"].contains("openai") &&
               persisted["providers"]["openai"]["trustedBaseUrl"] ==
                   "https://new.example.invalid/v1" &&
               persisted["providers"]["openai"]["contextWindowTokens"] ==
                   32768 &&
               !persisted["providers"].contains("anthropic"),
           "forgotten provider must be removed from encrypted persistence");

    std::vector<AI::ProviderConfigChange> rejectedChanges;
    AI::ProviderConfigChange rejected;
    rejected.providerName = "openai";
    rejected.config.apiKey = "must-not-commit";
    rejected.config.baseUrl = "https://rejected.example.invalid/v1";
    rejected.config.model = "rejected-model";
    rejected.config.trustedBaseUrl =
        "https://rejected.example.invalid/v1";
    rejected.config.contextWindowTokens = 100000;
    rejectedChanges.push_back(std::move(rejected));
    const auto unavailablePath =
        temp.path() / "missing-parent" / "ai_config.json";
    expect(!store.applyChangesAndSave(rejectedChanges,
                                      unavailablePath.string()),
           "provider commit should fail when the temp file cannot be created");
    loaded = {};
    expect(store.loadConfig("openai", loaded) &&
               loaded.apiKey == "new-openai-secret" &&
               loaded.model == "new-model" &&
               loaded.trustedBaseUrl ==
                   "https://new.example.invalid/v1" &&
               loaded.contextWindowTokens == 32768,
           "failed provider persistence must preserve the live snapshot");
}

void testSettingsDraftClearsSensitiveBuffers() {
    AI::ProviderSettingsDraft provider;
    std::memcpy(provider.apiKey, "plaintext-key", 14);
    std::memcpy(provider.baseUrl, "https://example.invalid", 24);
    provider.validationError = "sensitive diagnostic";
    provider.showApiKey = true;
    provider.dirty = true;
    provider.wasConfigured = true;
    provider.forgetRequested = true;
    provider.clear();

    const auto allZero = [](const auto& buffer) {
        return std::all_of(std::begin(buffer), std::end(buffer),
                           [](char value) { return value == '\0'; });
    };
    expect(allZero(provider.apiKey) &&
               allZero(provider.baseUrl) &&
               provider.validationError.empty() &&
               !provider.showApiKey &&
               !provider.dirty &&
               !provider.wasConfigured &&
               !provider.forgetRequested,
           "provider draft clear must wipe plaintext and reset metadata");

    AI::ChatSettingsDraft draft;
    std::memcpy(draft.systemPrompt, "temporary prompt", 17);
    std::memcpy(draft.proxyHost, "proxy.invalid", 14);
    std::memcpy(draft.provider("openai").apiKey, "another-key", 12);
    draft.tokenLimit = 64000;
    draft.autoApproveWrites = true;
    draft.autoApproveLuaExecution = false;
    draft.loaded = true;
    draft.clear();
    expect(draft.providers.empty() &&
               allZero(draft.systemPrompt) &&
               allZero(draft.proxyHost) &&
               draft.tokenLimit == 0 &&
               !draft.autoApproveWrites &&
               draft.autoApproveLuaExecution &&
               !draft.loaded,
           "discarding the settings draft must wipe the complete edit snapshot");
}

void testSettingsLoadIsTransactional() {
    TempDirectory temp("settings");
    AI::AiSettings& settings = AI::AiSettings::getInstance();
    const auto good = temp.path() / "good.json";
    expect(settings.saveToFile(good.string()),
           "initial settings save should bind a safe path");

    AI::AiSettingsData expected = settings.get();
    expected.activeProvider = "deepseek";
    expected.executionTimeout = 77;
    expected.tokenLimit = 42000;
    expected.systemPrompt = "transactional prompt";
    expected.autoApproveLuaExecution = false;
    settings.set(expected);

    const auto roundTrip = settings.loadFromFile(good.string());
    const auto roundTrippedSettings = settings.get();
    const json persistedSettings = json::parse(readText(good));
    expect(roundTrip.status == PersistenceLoadStatus::Loaded &&
               !roundTrippedSettings.autoApproveLuaExecution &&
               persistedSettings.at("autoApproveLuaExecution") == false,
           "disabled Lua auto-approval must round-trip through settings persistence");

    const auto legacy = temp.path() / "legacy.json";
    writeText(legacy, R"({"version":1,"autoApproveWrites":false})");
    const auto legacyResult = settings.loadFromFile(legacy.string());
    expect(legacyResult.status == PersistenceLoadStatus::Loaded &&
               settings.get().autoApproveLuaExecution,
           "settings without the Lua permission field must retain the default allow policy");
    expect(settings.loadFromFile(good.string()).status ==
                   PersistenceLoadStatus::Loaded &&
               !settings.get().autoApproveLuaExecution,
           "explicitly disabled Lua permission must remain loadable after legacy defaults");

    const auto corrupt = temp.path() / "corrupt.json";
    writeText(corrupt, R"({"tokenLimit":"wrong"})");
    const std::string original = readText(corrupt);
    const auto result = settings.loadFromFile(corrupt.string());
    const AI::AiSettingsData retained = settings.get();
    expect(result.status == PersistenceLoadStatus::Invalid &&
               retained.activeProvider == expected.activeProvider &&
               retained.executionTimeout == expected.executionTimeout &&
               retained.systemPrompt == expected.systemPrompt &&
               !retained.autoApproveLuaExecution,
           "invalid settings must preserve the prior snapshot");

    settings.setTokenLimit(43000);
    expect(readText(corrupt) == original,
           "field mutators must not overwrite an invalid load target");

    const auto missing = temp.path() / "new-settings.json";
    const auto missingResult = settings.loadOrDefault(missing.string());
    expect(missingResult.status == PersistenceLoadStatus::Missing &&
               std::filesystem::exists(missing),
           "only a missing settings file should be created from defaults");
}

void testChatSessionLoadAndInstallAreTransactional() {
    TempDirectory temp("chat-session");
    const auto current = temp.path() / "current.json";
    const auto corrupt = temp.path() / "corrupt.json";

    AI::ChatSession initiallyUnbound;
    writeText(corrupt, R"({"messages":"wrong"})");
    const std::string originalCorrupt = readText(corrupt);
    const auto initialInvalid = initiallyUnbound.load(corrupt.string());
    initiallyUnbound.addMessage(chatMessage(AI::Role::User, "not persisted"));
    expect(initialInvalid.status == PersistenceLoadStatus::Invalid &&
               !initiallyUnbound.saveBound() &&
               readText(corrupt) == originalCorrupt,
           "an invalid legacy session must remain unbound and unchanged");

    AI::ChatSession session;
    session.setSystemPrompt("current prompt");
    session.setTokenLimit(32000);
    session.addMessage(chatMessage(AI::Role::User, "first"));
    expect(session.save(current.string()),
           "initial session save should succeed");

    const auto invalid = session.load(corrupt.string());
    expect(invalid.status == PersistenceLoadStatus::Invalid &&
               session.getMessages().size() == 1 &&
               session.getSystemPrompt() == "current prompt" &&
               session.getTokenLimit() == 32000,
           "invalid session must preserve messages, prompt, and token limit");

    session.addMessage(chatMessage(AI::Role::Assistant, "second"));
    expect(readText(corrupt) == originalCorrupt,
           "invalid session path must remain unchanged and unbound");
    const json updatedCurrent = readProtectedJson(
        current, AI::ProtectedPersistenceKind::Session);
    expect(updatedCurrent["messages"].size() == 2,
           "auto-save should remain bound to the prior valid session");

    expect(session.save(current.string()),
           "replacing an existing session should succeed");
    expect(!std::filesystem::exists(current.string() + ".tmp") &&
               !std::filesystem::exists(current.string() + ".bak"),
           "atomic replacement should leave no temp or backup file");

    const auto missing = temp.path() / "new-session.json";
    const auto missingResult = session.load(missing.string());
    expect(missingResult.status == PersistenceLoadStatus::Missing &&
               session.getMessages().empty(),
           "missing session should commit a fresh empty binding");
    session.addMessage(chatMessage(AI::Role::User, "new session"));
    expect(std::filesystem::exists(missing),
           "first message should persist to the missing session path");
}

void testSessionSettingsRemainGlobal() {
    TempDirectory temp("global-session-settings");
    const auto legacy = temp.path() / "legacy-v1.json";
    const auto legacyBudget = temp.path() / "legacy-v1-budget.json";
    const auto migrated = temp.path() / "migrated-v2.json";

    json legacyDocument = sessionDocument("legacy message");
    legacyDocument["systemPrompt"] = json::array({"not", "a", "prompt"});
    legacyDocument["tokenLimit"] = "not-a-token-limit";
    writeText(legacy, legacyDocument.dump());

    AI::ChatSession session;
    session.setSystemPrompt("global prompt");
    session.setTokenLimit(42000);
    const auto loaded = session.load(legacy.string());
    expect(loaded.usable() &&
               session.getSystemPrompt() == "global prompt" &&
               session.getTokenLimit() == 42000 &&
               session.getMessages().size() == 2,
           "legacy session settings must be ignored without blocking message migration");
    expect(loaded.status == PersistenceLoadStatus::Recovered &&
               AI::isProtectedPersistenceFile(
                   legacy, AI::ProtectedPersistenceKind::Session) &&
               readText(legacy).find("legacy message") == std::string::npos,
           "valid legacy plaintext sessions must migrate before use");

    expect(session.save(migrated.string()),
           "migrated session should save in the current format");
    const json saved = readProtectedJson(
        migrated, AI::ProtectedPersistenceKind::Session);
    expect(saved.at("version") == 2 &&
               !saved.contains("systemPrompt") &&
               !saved.contains("tokenLimit"),
           "session format v2 must not persist global prompt/token settings");

    AI::ChatSession reloaded;
    reloaded.setSystemPrompt("new global prompt");
    reloaded.setTokenLimit(64000);
    expect(reloaded.load(migrated.string()).usable() &&
               reloaded.getSystemPrompt() == "new global prompt" &&
               reloaded.getTokenLimit() == 64000,
           "switching to a v2 session must preserve the current global settings");

    json budgetDocument = sessionDocument("unused");
    budgetDocument["tokenLimit"] = AI::ChatSession::kMaxTokenLimit;
    budgetDocument["messages"] = json::array({
        {{"role", "user"}, {"content", std::string(3000, 'o')}},
        {{"role", "assistant"}, {"content", std::string(3000, 'a')}},
        {{"role", "user"}, {"content", "latest"}},
    });
    writeText(legacyBudget, budgetDocument.dump());

    AI::ChatSession budgeted;
    budgeted.setTokenLimit(AI::ChatSession::kMinTokenLimit);
    expect(budgeted.load(legacyBudget.string()).usable() &&
               budgeted.getMessages().size() == 1 &&
               budgeted.getMessages().front().content == "latest",
           "session load must apply the live global token budget, not the legacy session field");
}

void testPersistenceFileLimit() {
    TempDirectory temp("file-limit");
    const auto oversized = temp.path() / "oversized.json";
    writeSparseFile(oversized, AI::Limits::kMaxPersistenceFileBytes + 1u);

    const AI::JsonDocumentLoadResult loaded =
        AI::loadJsonDocument(oversized);
    expect(loaded.result.status == PersistenceLoadStatus::Invalid &&
               loaded.result.message.find("32 MiB") != std::string::npos,
           "persistence loader must reject a 32 MiB + 1 file before parsing");

    const auto saveTarget = temp.path() / "escaped-session.json";
    writeText(saveTarget, "preserve me");
    AI::ChatSession escaped;
    expect(escaped.addMessage(chatMessage(
               AI::Role::User,
               std::string(6u * AI::Limits::kMiB, '\0'))),
           "escaped payload fixture should fit the in-memory session budget");
    expect(!escaped.save(saveTarget.string()) &&
               readText(saveTarget) == "preserve me" &&
               !std::filesystem::exists(saveTarget.string() + ".tmp"),
           "JSON escaping beyond 32 MiB must not replace the prior file");
}

void testPersistenceJsonComplexityLimits() {
    TempDirectory temp("json-complexity");

    const auto validateRejected = [](const std::string& serialized,
                                     const std::string& marker) {
        std::string error;
        nlohmann::json document;
        expect(!AI::parseBoundedJson(serialized, document, error) &&
                   document.is_null() &&
                   error.find(marker) != std::string::npos,
               "bounded JSON parser should reject " + marker +
                   " before DOM construction (error: " + error + ")");
    };

    std::string tooDeep(AI::Limits::kMaxPersistenceJsonDepth + 1u, '[');
    tooDeep += "null";
    tooDeep.append(AI::Limits::kMaxPersistenceJsonDepth + 1u, ']');
    validateRejected(tooDeep, "nesting");

    std::string tooManyItems;
    tooManyItems.reserve(
        2u + (AI::Limits::kMaxPersistenceContainerItems + 1u) * 5u);
    tooManyItems.push_back('[');
    for (size_t i = 0;
         i < AI::Limits::kMaxPersistenceContainerItems + 1u; ++i) {
        if (i != 0) {
            tooManyItems.push_back(',');
        }
        tooManyItems += "null";
    }
    tooManyItems.push_back(']');
    validateRejected(tooManyItems, "20000 item");

    std::string tooManyNodes;
    constexpr size_t kChildArrays = 13u;
    tooManyNodes.reserve(
        kChildArrays *
        (2u + AI::Limits::kMaxPersistenceContainerItems * 5u));
    tooManyNodes.push_back('[');
    for (size_t child = 0; child < kChildArrays; ++child) {
        if (child != 0) {
            tooManyNodes.push_back(',');
        }
        tooManyNodes.push_back('[');
        for (size_t item = 0;
             item < AI::Limits::kMaxPersistenceContainerItems; ++item) {
            if (item != 0) {
                tooManyNodes.push_back(',');
            }
            tooManyNodes += "null";
        }
        tooManyNodes.push_back(']');
    }
    tooManyNodes.push_back(']');
    expect(tooManyNodes.size() < AI::Limits::kMaxPersistenceFileBytes,
           "node-amplification fixture must remain below the file limit");
    validateRejected(tooManyNodes, "250000 node");

    std::string tooManyStringBytes;
    tooManyStringBytes.reserve(
        AI::Limits::kMaxPersistenceTotalStringBytes + 64u);
    tooManyStringBytes.push_back('[');
    for (size_t i = 0; i < 3u; ++i) {
        if (i != 0) {
            tooManyStringBytes.push_back(',');
        }
        tooManyStringBytes.push_back('"');
        tooManyStringBytes.append(AI::Limits::kMaxPersistenceStringBytes,
                                  static_cast<char>('a' + i));
        tooManyStringBytes.push_back('"');
    }
    tooManyStringBytes += ",\"x\"";
    tooManyStringBytes.push_back(']');
    expect(tooManyStringBytes.size() < AI::Limits::kMaxPersistenceFileBytes,
           "string-amplification fixture must remain below the file limit");
    validateRejected(tooManyStringBytes, "cumulative limit");

    const auto plainConfig = temp.path() / "complex-config.json";
    writeText(plainConfig, tooManyItems);
    const AI::JsonDocumentLoadResult configResult =
        AI::loadJsonDocument(plainConfig);
    expect(configResult.result.status == PersistenceLoadStatus::Invalid &&
               configResult.result.message.find("20000 item") !=
                   std::string::npos &&
               readText(plainConfig) == tooManyItems,
           "ordinary persistence must reject complex JSON without modifying its bytes");

    const auto current = temp.path() / "current.json";
    const auto complexSession = temp.path() / "complex-session.json";
    AI::ChatSession session;
    expect(session.addMessage(chatMessage(AI::Role::User, "retained")) &&
               session.save(current.string()),
           "could not create complexity transactional baseline");
    writeText(complexSession, tooDeep);
    const auto protectedResult = session.load(complexSession.string());
    expect(protectedResult.status == PersistenceLoadStatus::Invalid &&
               protectedResult.message.find("nesting") != std::string::npos &&
               session.getMessages().size() == 1 &&
               session.getMessages().front().content == "retained" &&
               readText(complexSession) == tooDeep,
           "protected-session loader must reject structural amplification transactionally");

    session.addMessage(chatMessage(AI::Role::Assistant, "still bound"));
    const json persisted = readProtectedJson(
        current, AI::ProtectedPersistenceKind::Session);
    expect(persisted.at("messages").size() == 2 &&
               persisted.at("messages").at(1).at("content") ==
                   "still bound",
           "complexity rejection must preserve the prior protected auto-save binding");
}

void testChatSessionLoadLimitsAreTransactional() {
    TempDirectory temp("session-limits");
    const auto current = temp.path() / "current.json";
    const auto oversizedMessage = temp.path() / "oversized-message.json";
    const auto tooManyMessages = temp.path() / "too-many-messages.json";
    const auto oversizedPayload = temp.path() / "oversized-payload.json";

    AI::ChatSession session;
    session.setSystemPrompt("retained prompt");
    expect(session.addMessage(chatMessage(AI::Role::User, "retained")) &&
               session.save(current.string()),
           "could not create the retained session baseline");

    {
        json document = sessionDocument("unused");
        document["messages"] = json::array({
            {{"role", "user"},
             {"content", std::string(
                 AI::Limits::kMaxMessageContentBytes + 1u, 'x')}},
        });
        writeText(oversizedMessage, document.dump());
    }
    const auto messageResult = session.load(oversizedMessage.string());
    expect(messageResult.status == PersistenceLoadStatus::Invalid &&
               messageResult.message.find("JSON string exceeds") !=
                   std::string::npos &&
               session.getMessages().size() == 1 &&
               session.getMessages().front().content == "retained" &&
               session.getSystemPrompt() == "retained prompt",
           "oversized message load must preserve the prior session snapshot");

    {
        json document = sessionDocument("unused");
        document["messages"] = json::array();
        for (size_t i = 0;
             i < AI::Limits::kMaxSessionMessagesOnDisk + 1u; ++i) {
            document["messages"].push_back(
                {{"role", "user"}, {"content", "x"}});
        }
        writeText(tooManyMessages, document.dump());
    }
    const auto countResult = session.load(tooManyMessages.string());
    expect(countResult.status == PersistenceLoadStatus::Invalid &&
               countResult.message.find("10000") != std::string::npos &&
               session.getMessages().size() == 1,
           "session loader must reject a 10001st on-disk message transactionally");

    {
        json document = sessionDocument("unused");
        document["messages"] = json::array({
            {{"role", "user"},
             {"content", std::string(
                 AI::Limits::kMaxMessageContentBytes, 'a')}},
            {{"role", "assistant"},
             {"content", std::string(
                 AI::Limits::kMaxMessageContentBytes, 'b')},
             {"name", "x"}},
        });
        writeText(oversizedPayload, document.dump());
    }
    const auto payloadResult = session.load(oversizedPayload.string());
    expect(payloadResult.status == PersistenceLoadStatus::Invalid &&
               payloadResult.message.find("16 MiB") != std::string::npos &&
               session.getMessages().size() == 1,
           "retained session payload must reject the first byte beyond 16 MiB");

    expect(session.addMessage(chatMessage(AI::Role::Assistant, "still bound")),
           "valid message should still append after rejected session loads");
    const json persisted = readProtectedJson(
        current, AI::ProtectedPersistenceKind::Session);
    expect(persisted["messages"].size() == 2 &&
               persisted["messages"][1]["content"] == "still bound",
           "invalid load targets must not replace the prior auto-save binding");
}

void testRuntimeSessionPayloadLimit() {
    AI::ChatSession rejected;
    expect(rejected.addMessage(chatMessage(
               AI::Role::User,
               std::string(AI::Limits::kMaxMessageContentBytes, 'u'))) &&
               rejected.addMessage(chatMessage(
                   AI::Role::Assistant,
                   std::string(AI::Limits::kMaxMessageContentBytes, 'a'))),
           "runtime session should accept exactly 16 MiB of retained payload");
    expect(!rejected.addMessage(chatMessage(AI::Role::Tool, "x")) &&
               rejected.getMessages().size() == 2 &&
               rejected.getMessages().back().role == AI::Role::Assistant,
           "runtime session must reject and roll back the first byte beyond the latest turn budget");

    TempDirectory temp("session-group-eviction");
    const auto source = temp.path() / "source.json";
    const auto rejectedSource = temp.path() / "rejected-source.json";

    {
        json document = sessionDocument("unused");
        document["tokenLimit"] = AI::ChatSession::kMaxTokenLimit;
        document["messages"] = json::array({
            {{"role", "user"}, {"content", "old"}},
            {{"role", "system"},
             {"content", std::string(
                 2u * AI::Limits::kMiB - 64u, 'o')}},
            {{"role", "user"}, {"content", "new"}},
            {{"role", "system"},
             {"content", std::string(7u * AI::Limits::kMiB, 'n')}},
            {{"role", "system"},
             {"content", std::string(7u * AI::Limits::kMiB, 'a')}},
        });
        writeText(rejectedSource, document.dump());
    }
    AI::ChatSession transactional;
    expect(transactional.load(rejectedSource.string()).usable(),
           "could not load the rejection rollback fixture");
    expect(!transactional.addMessage(chatMessage(
               AI::Role::Assistant,
               std::string(3u * AI::Limits::kMiB, 'x'))) &&
               transactional.getMessages().size() == 5 &&
               transactional.getMessages().front().content == "old",
           "rejected latest-turn growth must preserve older conversation groups");

    {
        json document = sessionDocument("unused");
        document["tokenLimit"] = AI::ChatSession::kMaxTokenLimit;
        document["messages"] = json::array({
            {{"role", "user"}, {"content", "old"}},
            {{"role", "system"},
             {"content", std::string(7u * AI::Limits::kMiB, 'o')}},
            {{"role", "assistant"}, {"content", "old answer"}},
            {{"role", "user"}, {"content", "new"}},
            {{"role", "system"},
             {"content", std::string(7u * AI::Limits::kMiB, 'n')}},
        });
        writeText(source, document.dump());
    }

    AI::ChatSession evicted;
    expect(evicted.load(source.string()).usable(),
           "could not load the near-budget session fixture");
    expect(evicted.addMessage(chatMessage(
               AI::Role::Assistant,
               std::string(3u * AI::Limits::kMiB, 'r'))),
           "a new message should fit after evicting an old conversation group");
    const auto& retained = evicted.getMessages();
    expect(retained.size() == 3 &&
               retained[0].role == AI::Role::User &&
               retained[0].content == "new" &&
               retained[1].role == AI::Role::System &&
               retained[1].content.front() == 'n' &&
               retained[2].role == AI::Role::Assistant &&
               retained[2].content.front() == 'r',
           "session payload eviction must remove the complete oldest conversation group");
}

void testSessionProtectionAndCiphertextTamper() {
    TempDirectory temp("session-protection");
    const auto sessionPath = temp.path() / "protected.json";
    const std::string secret =
        "lua_execute memory 0x12345678 register=x0 raw=DEADBEEF";

    AI::ChatSession session;
    expect(session.addMessage(chatMessage(AI::Role::User, secret)) &&
               session.save(sessionPath.string()),
           "protected session fixture should save");
    const std::string encrypted = readText(sessionPath);
    expect(AI::isProtectedPersistenceFile(
               sessionPath, AI::ProtectedPersistenceKind::Session) &&
               encrypted.find(secret) == std::string::npos &&
               encrypted.find("messages") == std::string::npos,
           "session history and schema must not be readable at rest");

    AI::ChatSession reloaded;
    expect(reloaded.load(sessionPath.string()).status ==
               PersistenceLoadStatus::Loaded &&
               reloaded.getMessages().size() == 1 &&
               reloaded.getMessages().front().content == secret,
           "same-user DPAPI round trip must preserve the full history");

    std::string tampered = encrypted;
    expect(tampered.size() > 32, "protected envelope fixture is too small");
    tampered.back() = static_cast<char>(tampered.back() ^ 0x5a);
    writeText(sessionPath, tampered);
    const auto rejected = reloaded.load(sessionPath.string());
    expect(rejected.status == PersistenceLoadStatus::Invalid &&
               reloaded.getMessages().size() == 1 &&
               reloaded.getMessages().front().content == secret,
           "tampered DPAPI ciphertext must fail without replacing live history");

    const auto wrongKind = AI::loadProtectedJsonDocument(
        sessionPath, AI::ProtectedPersistenceKind::SessionIndex);
    expect(wrongKind.result.status == PersistenceLoadStatus::Invalid,
           "a session envelope must not be accepted as an index");
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, void (*)()>> tests = {
        {"corrupt index recovery", testCorruptIndexRecovery},
        {"API key transactional load", testApiKeyLoadIsTransactional},
        {"atomic install fault recovery", testAtomicInstallFaultRecovery},
        {"provider config transactional changes", testProviderConfigChangesAreAtomic},
        {"settings draft secure clear", testSettingsDraftClearsSensitiveBuffers},
        {"settings transactional load", testSettingsLoadIsTransactional},
        {"chat session transactional load", testChatSessionLoadAndInstallAreTransactional},
        {"global session settings ownership", testSessionSettingsRemainGlobal},
        {"persistence file limit", testPersistenceFileLimit},
        {"persistence JSON complexity", testPersistenceJsonComplexityLimits},
        {"chat session load limits", testChatSessionLoadLimitsAreTransactional},
        {"runtime session payload limit", testRuntimeSessionPayloadLimit},
        {"DPAPI session protection", testSessionProtectionAndCiphertextTamper},
    };

    size_t passed = 0;
    for (const auto& test : tests) {
        try {
            test.second();
            ++passed;
            std::cout << "[PASS] " << test.first << '\n';
        } catch (const std::exception& error) {
            std::cerr << "[FAIL] " << test.first << ": " << error.what() << '\n';
            return 1;
        }
    }

    std::cout << passed << " persistence test groups passed\n";
    return 0;
}
