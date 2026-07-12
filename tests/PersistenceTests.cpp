#include "gui/Gui.h"
#include "gui/ai/AiSettings.h"
#include "gui/ai/ApiKeyStore.h"
#include "gui/ai/ChatSession.h"
#include "gui/ai/SessionManager.h"

#include "nlohmann/json.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <list>
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

std::string readText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    expect(input.is_open(), "could not open test file for reading");
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
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

    AI::SessionManager& manager = AI::SessionManager::getInstance();
    const auto result = manager.init(temp.path().string(), {});
    expect(result.status == PersistenceLoadStatus::Recovered,
           "corrupt index should be recovered from session files");

    const auto sessions = manager.list();
    expect(sessions.size() == 2,
           "recovery should retain only two valid session files");
    expect(!manager.activeId().empty(),
           "recovery should select an active session");
    expect(readText(index.string() + ".corrupt") == corrupt,
           "corrupt index must be preserved before replacement");

    const json rebuilt = json::parse(readText(index));
    expect(rebuilt["sessions"].is_array() &&
               rebuilt["sessions"].size() == 2,
           "rebuilt index should persist recovered metadata");
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
    settings.set(expected);

    const auto corrupt = temp.path() / "corrupt.json";
    writeText(corrupt, R"({"tokenLimit":"wrong"})");
    const std::string original = readText(corrupt);
    const auto result = settings.loadFromFile(corrupt.string());
    const AI::AiSettingsData retained = settings.get();
    expect(result.status == PersistenceLoadStatus::Invalid &&
               retained.activeProvider == expected.activeProvider &&
               retained.executionTimeout == expected.executionTimeout &&
               retained.systemPrompt == expected.systemPrompt,
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
           "an initially corrupt session must remain unbound and preserved");

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
           "invalid session path must not become the auto-save target");
    const json updatedCurrent = json::parse(readText(current));
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

} // namespace

int main() {
    const std::vector<std::pair<const char*, void (*)()>> tests = {
        {"corrupt index recovery", testCorruptIndexRecovery},
        {"API key transactional load", testApiKeyLoadIsTransactional},
        {"settings transactional load", testSettingsLoadIsTransactional},
        {"chat session transactional load", testChatSessionLoadAndInstallAreTransactional},
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
