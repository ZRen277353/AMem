#include "gui/ai/AiLimits.h"
#include "gui/ai/ProviderStreamTerminal.h"
#include "gui/ai/ProviderResponseLimits.h"
#include "gui/ai/SSEParser.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using AI::CompletionResponse;
using AI::ErrorCategory;
using AI::InspectClaudeStreamEvent;
using AI::InspectOpenAICompatibleStreamEvent;
using AI::SSEParser;
using AI::StreamTerminalTracker;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void expectInvalid(const AI::ProviderError& error,
                   const std::string& expectedMessage) {
    expect(error.category == ErrorCategory::InvalidResponse,
           "expected InvalidResponse");
    expect(error.message.find(expectedMessage) != std::string::npos,
           "unexpected error: " + error.message);
}

void testSseFragmentationAndMultilineData() {
    std::vector<std::string> events;
    SSEParser parser([&](const std::string& event) {
        events.push_back(event);
    });

    const std::string first = "data: {\"type\":\"message_";
    const std::string second = "start\"}\r\n\r\ndata: line one\r\ndata: line two\r\n\r\n";
    parser.feed(first.data(), first.size());
    parser.feed(second.data(), second.size());
    parser.finish();

    expect(events.size() == 2, "fragmented SSE should emit two events");
    expect(events[0] == R"({"type":"message_start"})",
           "fragmented JSON event should be reconstructed");
    expect(events[1] == "line one\nline two",
           "multiple data fields should be joined with a newline");
}

void testDoneIsVisibleAndEofFlushes() {
    std::vector<std::string> events;
    SSEParser parser([&](const std::string& event) {
        events.push_back(event);
    });

    const std::string input = "data: [DONE]\n\ndata: tail";
    parser.feed(input.data(), input.size());
    parser.finish();

    expect(events.size() == 2 && events[0] == "[DONE]" &&
               events[1] == "tail",
           "[DONE] and an unterminated final event must reach the callback");
}

void testSseCallbackExceptionStopsParsing() {
    int calls = 0;
    SSEParser parser([&](const std::string&) {
        ++calls;
        throw std::runtime_error("callback exploded");
    });

    const std::string input = "data: first\n\ndata: second\n\n";
    parser.feed(input.data(), input.size());
    parser.finish();

    expect(calls == 1, "parser must stop dispatching after callback failure");
    expect(parser.failed(), "callback failure should be observable");
    expect(parser.error() == "callback exploded",
           "callback error should preserve the exception message");
}

void testSseLineLimit() {
    SSEParser parser(nullptr);
    std::string input = "data: ";
    input.append(AI::Limits::kMaxSseLineBytes, 'x');
    parser.feed(input.data(), input.size());

    expect(parser.failed() && parser.limitExceeded(),
           "oversized SSE line should fail at the parser boundary");
    expect(parser.error().find("1 MiB") != std::string::npos,
           "SSE line error should name its limit");
}

void testSseEventLimit() {
    SSEParser parser(nullptr);
    const std::string fragment(800u * AI::Limits::kKiB, 'x');
    const std::string line = "data: " + fragment + "\n";
    parser.feed(line.data(), line.size());
    parser.feed(line.data(), line.size());
    parser.feed(line.data(), line.size());

    expect(parser.failed() && parser.limitExceeded(),
           "oversized multiline SSE event should fail before dispatch");
    expect(parser.error().find("2 MiB") != std::string::npos,
           "SSE event error should name its limit");
}

void testProviderAccumulationLimits() {
    std::string error;
    std::string content(AI::Limits::kMaxAssistantContentBytes, 'a');
    expect(AI::appendAssistantContentWithinLimit(content, "", error),
           "assistant content at the limit should be accepted");
    expect(!AI::appendAssistantContentWithinLimit(content, "x", error) &&
               content.size() == AI::Limits::kMaxAssistantContentBytes,
           "assistant content must reject the first byte beyond its limit");

    AI::ToolCall call;
    size_t totalArguments = 0;
    const std::string maxArguments(
        AI::Limits::kMaxToolArgumentsPerCallBytes, 'b');
    error.clear();
    expect(AI::appendToolArgumentsWithinLimit(
               call, maxArguments, totalArguments, error),
           "per-call arguments at the limit should be accepted");
    expect(!AI::appendToolArgumentsWithinLimit(
               call, "x", totalArguments, error) &&
               call.arguments.size() ==
                   AI::Limits::kMaxToolArgumentsPerCallBytes,
           "per-call arguments must not grow after limit rejection");

    totalArguments = AI::Limits::kMaxToolArgumentsPerMessageBytes;
    AI::ToolCall another;
    error.clear();
    expect(!AI::appendToolArgumentsWithinLimit(
               another, "x", totalArguments, error) &&
               another.arguments.empty(),
           "message-level argument budget must reject additional bytes");
}

void testProviderMessageValidationLimits() {
    AI::ChatMessage message;
    message.role = AI::Role::Assistant;
    message.content.assign(AI::Limits::kMaxAssistantContentBytes, 'a');
    message.toolCalls.resize(AI::Limits::kMaxToolCallsPerMessage);

    std::string error;
    expect(AI::validateProviderMessageLimits(message, error),
           "provider message exactly at structural limits should validate");

    message.toolCalls.emplace_back();
    expect(!AI::validateProviderMessageLimits(message, error) &&
               error.find("64 tool calls") != std::string::npos,
           "provider message should reject a 65th tool call");
}

void testClaudeCompleteStream() {
    StreamTerminalTracker tracker;
    tracker.observe(InspectClaudeStreamEvent(R"({"type":"message_start"})"));
    tracker.observe(InspectClaudeStreamEvent(R"({"type":"ping"})"));
    tracker.observe(InspectClaudeStreamEvent(R"({"type":"message_stop"})"));
    tracker.observe(InspectClaudeStreamEvent(R"({"type":"message_stop"})"));

    expect(tracker.started() && tracker.terminal(),
           "Claude start and repeated stop should be accepted");
    expect(!tracker.validationError("Claude"),
           "complete Claude stream should validate");
}

void testClaudeMissingTerminal() {
    StreamTerminalTracker tracker;
    tracker.observe(InspectClaudeStreamEvent(R"({"type":"message_start"})"));
    expectInvalid(tracker.validationError("Claude"), "without a terminal event");
}

void testClaudeMalformedEventWins() {
    StreamTerminalTracker tracker;
    tracker.observe(InspectClaudeStreamEvent(R"({"type":"message_start"})"));
    tracker.observe(InspectClaudeStreamEvent("{not-json"));
    tracker.observe(InspectClaudeStreamEvent(R"({"type":"message_stop"})"));
    expectInvalid(tracker.validationError("Claude"), "invalid JSON");
}

void testOpenAiFinishReasonTerminal() {
    StreamTerminalTracker tracker;
    tracker.observe(InspectOpenAICompatibleStreamEvent(
        R"({"choices":[{"delta":{"content":"ok"},"finish_reason":null}]})",
        "OpenAI"));
    tracker.observe(InspectOpenAICompatibleStreamEvent(
        R"({"choices":[{"delta":{},"finish_reason":"stop"}]})",
        "OpenAI"));
    expect(!tracker.validationError("OpenAI"),
           "finish_reason should terminate an OpenAI stream");
}

void testOpenAiDoneTerminal() {
    StreamTerminalTracker tracker;
    tracker.observe(InspectOpenAICompatibleStreamEvent(
        R"({"choices":[{"delta":{"role":"assistant"}}]})", "OpenAI"));
    tracker.observe(InspectOpenAICompatibleStreamEvent("[DONE]", "OpenAI"));
    expect(!tracker.validationError("OpenAI"),
           "[DONE] should terminate a started OpenAI stream");
}

void testOpenAiFinishThenDoneIsIdempotent() {
    StreamTerminalTracker tracker;
    tracker.observe(InspectOpenAICompatibleStreamEvent(
        R"({"choices":[{"delta":{},"finish_reason":"tool_calls"}]})",
        "OpenAI"));
    tracker.observe(InspectOpenAICompatibleStreamEvent("[DONE]", "OpenAI"));
    expect(tracker.started() && tracker.terminal() &&
               !tracker.validationError("OpenAI"),
           "finish_reason followed by [DONE] should remain valid");
}

void testEmptyFinishReasonIsNotTerminal() {
    StreamTerminalTracker tracker;
    tracker.observe(InspectOpenAICompatibleStreamEvent(
        R"({"choices":[{"delta":{},"finish_reason":""}]})", "OpenAI"));
    expectInvalid(tracker.validationError("OpenAI"), "without a terminal event");
}

void testDeepSeekMalformedEvent() {
    StreamTerminalTracker tracker;
    tracker.observe(InspectOpenAICompatibleStreamEvent(
        R"({"choices":"invalid"})", "DeepSeek"));
    expectInvalid(tracker.validationError("DeepSeek"), "invalid 'choices'");
}

void testTerminalWithoutStart() {
    StreamTerminalTracker tracker;
    tracker.observe(InspectOpenAICompatibleStreamEvent("[DONE]", "OpenAI"));
    expectInvalid(tracker.validationError("OpenAI"), "without a valid start event");
}

void testNonSseBodyDoesNotValidate() {
    StreamTerminalTracker tracker;
    expectInvalid(tracker.validationError("OpenAI"), "without a valid start event");
}

void testPartialToolCallIsPreservedOnValidationFailure() {
    StreamTerminalTracker tracker;
    tracker.observe(InspectOpenAICompatibleStreamEvent(
        R"({"choices":[{"delta":{"tool_calls":[{"index":0}]}}]})",
        "OpenAI"));

    CompletionResponse response;
    response.message.content = "partial";
    response.message.toolCalls.push_back(
        {"call_1", "memory_read", R"({"address":"0x1234")", {}});

    expect(!tracker.applyValidation("OpenAI", response),
           "unterminated stream should fail validation");
    expectInvalid(response.error, "without a terminal event");
    expect(response.message.content == "partial" &&
               response.message.toolCalls.size() == 1 &&
               response.message.toolCalls[0].name == "memory_read",
           "validation failure must retain partial content and tool calls");
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, void (*)()>> tests = {
        {"SSE fragmentation and multiline data", testSseFragmentationAndMultilineData},
        {"SSE DONE visibility and EOF flush", testDoneIsVisibleAndEofFlushes},
        {"SSE callback exception", testSseCallbackExceptionStopsParsing},
        {"SSE line limit", testSseLineLimit},
        {"SSE event limit", testSseEventLimit},
        {"provider accumulation limits", testProviderAccumulationLimits},
        {"provider message validation limits",
         testProviderMessageValidationLimits},
        {"Claude complete stream", testClaudeCompleteStream},
        {"Claude missing terminal", testClaudeMissingTerminal},
        {"Claude malformed event", testClaudeMalformedEventWins},
        {"OpenAI finish_reason", testOpenAiFinishReasonTerminal},
        {"OpenAI DONE", testOpenAiDoneTerminal},
        {"OpenAI repeated terminal", testOpenAiFinishThenDoneIsIdempotent},
        {"OpenAI empty finish_reason", testEmptyFinishReasonIsNotTerminal},
        {"DeepSeek malformed event", testDeepSeekMalformedEvent},
        {"terminal without start", testTerminalWithoutStart},
        {"non-SSE body", testNonSseBodyDoesNotValidate},
        {"partial tool call retention", testPartialToolCallIsPreservedOnValidationFailure},
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

    std::cout << passed << " provider stream test groups passed\n";
    return 0;
}
