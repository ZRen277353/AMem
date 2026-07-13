#include "gui/ai/ContextBudget.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

AI::ChatMessage message(AI::Role role, std::string content) {
    AI::ChatMessage value;
    value.role = role;
    value.content = std::move(content);
    return value;
}

AI::ContextBudgetConfig budgetConfig(int userLimit,
                                     int providerLimit = 64000) {
    AI::ContextBudgetConfig config;
    config.userTokenLimit = userLimit;
    config.providerContextTokens = providerLimit;
    config.providerMaxOutputTokens = 4096;
    return config;
}

void testConservativeTextEstimator() {
    expect(AI::estimateTextTokensConservative("abcdefghijkl") == 4,
           "ASCII estimation should use a conservative three-byte ratio");
    const std::string fourCjk =
        "\xE5\x86\x85\xE5\xAD\x98\xE5\xB7\xA5\xE5\x85\xB7";
    expect(AI::estimateTextTokensConservative(fourCjk) == 4,
           "three-byte UTF-8 code points should count individually");
    const std::string emoji = "\xF0\x9F\x94\xA7";
    expect(AI::estimateTextTokensConservative(emoji) == 2,
           "four-byte UTF-8 code points should reserve two tokens");
}

void testProviderAndConfiguredLimits() {
    std::vector<AI::ChatMessage> messages = {
        message(AI::Role::User, "latest"),
    };
    AI::ContextBudgetResult providerLimited = AI::prepareContextBudget(
        messages, {}, budgetConfig(100000));
    expect(providerLimited.success &&
               providerLimited.contextWindowTokens == 64000 &&
               providerLimited.outputTokenReserve == 4096 &&
               providerLimited.inputBudgetTokens == 59904,
           "provider capability must cap the global user limit");

    AI::ContextBudgetConfig configured = budgetConfig(100000);
    configured.configuredContextTokens = 32000;
    AI::ContextBudgetResult endpointLimited = AI::prepareContextBudget(
        messages, {}, configured);
    expect(endpointLimited.success &&
               endpointLimited.contextWindowTokens == 32000,
           "explicit endpoint/model context must replace the provider default");

    configured.configuredContextTokens = 128000;
    AI::ContextBudgetResult providerStillLimited = AI::prepareContextBudget(
        messages, {}, configured);
    expect(providerStillLimited.success &&
               providerStillLimited.contextWindowTokens == 64000,
           "built-in endpoints must not exceed the provider capability");

    configured.configuredContextMayExceedProvider = true;
    AI::ContextBudgetResult customEndpoint = AI::prepareContextBudget(
        messages, {}, configured);
    expect(customEndpoint.success &&
               customEndpoint.contextWindowTokens == 100000,
           "a custom endpoint override may raise context but remains user-capped");
}

void testToolSchemasAndOutputReserveAreCharged() {
    AI::ToolDefinition tool;
    tool.name = "large_schema";
    tool.description = std::string(300, 'd');
    tool.parametersSchema = std::string(3000, 's');
    const std::vector<AI::ToolDefinition> tools = {tool};
    const AI::ContextBudgetResult result = AI::prepareContextBudget(
        {message(AI::Role::User, "latest")},
        tools,
        budgetConfig(8000));
    expect(result.success &&
               result.toolDefinitionTokens > 1000 &&
               result.outputTokenReserve == 2000 &&
               result.estimatedInputTokens > result.toolDefinitionTokens,
           "request budget must charge tools, framing, messages, and output");
}

void testOldCompleteGroupsAreTrimmed() {
    AI::ChatMessage assistant = message(
        AI::Role::Assistant, std::string(1800, 'a'));
    AI::ToolCall call;
    call.id = "call-old";
    call.name = "memory_read";
    call.arguments = "{}";
    assistant.toolCalls.push_back(call);

    AI::ChatMessage toolResult = message(AI::Role::Tool, "old result");
    toolResult.toolCallId = call.id;
    toolResult.name = call.name;

    std::vector<AI::ChatMessage> messages = {
        message(AI::Role::System, "system"),
        message(AI::Role::User, std::string(3600, 'u')),
        std::move(assistant),
        std::move(toolResult),
        message(AI::Role::User, "latest question"),
    };
    const AI::ContextBudgetResult result = AI::prepareContextBudget(
        std::move(messages), {}, budgetConfig(2000));
    expect(result.success && result.droppedMessages == 3 &&
               result.messages.size() == 2 &&
               result.messages[0].role == AI::Role::System &&
               result.messages[1].role == AI::Role::User &&
               result.messages[1].content == "latest question",
           "budget trimming must remove an old tool group atomically");
}

void testOversizedLatestGroupFailsClosed() {
    const AI::ContextBudgetResult messageFailure = AI::prepareContextBudget(
        {message(AI::Role::System, "system"),
         message(AI::Role::User, std::string(3000, 'x'))},
        {},
        budgetConfig(1000));
    expect(!messageFailure.success &&
               messageFailure.error.find("latest conversation") !=
                   std::string::npos,
           "an oversized latest turn must be rejected instead of truncated");

    AI::ToolDefinition oversizedTool;
    oversizedTool.name = "oversized";
    oversizedTool.parametersSchema = std::string(6000, 's');
    const AI::ContextBudgetResult toolFailure = AI::prepareContextBudget(
        {message(AI::Role::User, "latest")},
        {oversizedTool},
        budgetConfig(1000));
    expect(!toolFailure.success && toolFailure.toolDefinitionTokens > 1900,
           "untrimmable tool schemas must fail before provider dispatch");
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, void (*)()>> tests = {
        {"conservative UTF-8 estimator", testConservativeTextEstimator},
        {"provider and configured limits", testProviderAndConfiguredLimits},
        {"tool schema and output reserve", testToolSchemasAndOutputReserveAreCharged},
        {"complete group trimming", testOldCompleteGroupsAreTrimmed},
        {"oversized latest group rejection", testOversizedLatestGroupFailsClosed},
    };

    size_t passed = 0;
    for (const auto& test : tests) {
        try {
            test.second();
            ++passed;
            std::cout << "[PASS] " << test.first << '\n';
        } catch (const std::exception& error) {
            std::cerr << "[FAIL] " << test.first << ": "
                      << error.what() << '\n';
            return 1;
        }
    }
    std::cout << passed << " context budget test groups passed\n";
    return 0;
}
