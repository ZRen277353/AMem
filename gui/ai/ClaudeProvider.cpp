#ifdef HAVE_AI_CHAT

#include "ClaudeProvider.h"

#include "HttpClient.h"
#include "UIMessageQueue.h"

#include "../../third_party/nlohmann/json.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace AI {

namespace {

using json = nlohmann::json;

// Default API version header value. Users may override via config_.apiVersion.
constexpr const char* kDefaultApiVersion = "2023-06-01";

// Default max_tokens when the caller does not configure one. The Anthropic
// Messages API requires this field to be present.
constexpr int kDefaultMaxTokens = 4096;

constexpr unsigned long long kMaxStreamContentBlockIndex = 63ULL;

// Role → Anthropic role string mapping (only user/assistant are valid inside
// the "messages" array; System is hoisted to the top-level "system" param,
// Tool is converted to a user-role tool_result message).
const char* roleToAnthropic(Role r) {
    switch (r) {
    case Role::User:      return "user";
    case Role::Assistant: return "assistant";
    case Role::System:    return "system"; // not emitted directly
    case Role::Tool:      return "user";   // tool results are user messages
    }
    return "user";
}

std::string stripTrailingSlash(const std::string& url) {
    std::size_t end = url.size();
    while (end > 0 && url[end - 1] == '/') {
        --end;
    }
    return url.substr(0, end);
}

// Try to parse a string as JSON; on failure return the supplied fallback.
// Used for both tool argument payloads (may be partial/invalid) and tool
// input_schema strings supplied by callers.
json parseJsonOr(const std::string& text, const json& fallback) {
    if (text.empty()) {
        return fallback;
    }
    try {
        return json::parse(text);
    } catch (...) {
        return fallback;
    }
}

const json* objectMember(const json& object, const char* key) {
    if (!object.is_object()) {
        return nullptr;
    }
    const auto it = object.find(key);
    if (it == object.end() || !it->is_object()) {
        return nullptr;
    }
    return &*it;
}

std::string stringMemberOr(const json& object,
                           const char* key,
                           const std::string& fallback = {}) {
    if (!object.is_object()) {
        return fallback;
    }
    const auto it = object.find(key);
    if (it == object.end() || !it->is_string()) {
        return fallback;
    }
    return it->get<std::string>();
}

bool readStreamContentBlockIndex(const json& object, int& outIndex) {
    outIndex = 0;
    if (!object.is_object()) {
        return false;
    }

    const auto it = object.find("index");
    if (it == object.end()) {
        return true;
    }

    if (it->is_number_unsigned()) {
        const auto index = it->get<unsigned long long>();
        if (index > kMaxStreamContentBlockIndex) {
            return false;
        }
        outIndex = static_cast<int>(index);
        return true;
    }

    if (it->is_number_integer()) {
        const auto index = it->get<long long>();
        if (index < 0 || static_cast<unsigned long long>(index) > kMaxStreamContentBlockIndex) {
            return false;
        }
        outIndex = static_cast<int>(index);
        return true;
    }

    return false;
}

json toolResultBlock(const ChatMessage& msg) {
    json block = json::object();
    block["type"]        = "tool_result";
    block["tool_use_id"] = msg.toolCallId;
    block["content"]     = msg.content;
    return block;
}

// Map HTTP + network errors to a ProviderError. Mirrors the requirements in
// §12 and the general mapping documented in task 3.2.
ProviderError mapError(const HttpResponse& http, const std::string& body) {
    ProviderError err;
    if (http.cancelled) {
        err.category = ErrorCategory::Cancelled;
        err.message  = http.errorMessage.empty() ? "request cancelled"
                                                 : http.errorMessage;
        err.httpStatusCode = http.statusCode;
        return err;
    }
    if (http.timedOut) {
        err.category = ErrorCategory::Timeout;
        err.message  = http.errorMessage.empty() ? "request timed out"
                                                 : http.errorMessage;
        err.httpStatusCode = http.statusCode;
        return err;
    }
    if (http.statusCode == 0) {
        err.category = ErrorCategory::Network;
        err.message  = http.errorMessage.empty() ? "network error"
                                                 : http.errorMessage;
        return err;
    }
    if (http.statusCode >= 200 && http.statusCode < 300) {
        // Non-error status; caller checks err.category
        return err;
    }

    err.httpStatusCode = http.statusCode;

    // Try to extract Anthropic's error envelope:
    //   { "type": "error", "error": { "type": "...", "message": "..." } }
    std::string detail;
    try {
        json j = json::parse(body);
        if (j.is_object() && j.contains("error") && j["error"].is_object()) {
            const auto& e = j["error"];
            if (e.contains("type") && e["type"].is_string()) {
                err.providerErrorCode = e["type"].get<std::string>();
            }
            if (e.contains("message") && e["message"].is_string()) {
                detail = e["message"].get<std::string>();
            }
        }
    } catch (...) {
        // Fall through; use http.errorMessage below.
    }

    switch (http.statusCode) {
    case 401:
    case 403:
        err.category = ErrorCategory::Authentication;
        break;
    case 429:
        err.category = ErrorCategory::RateLimit;
        break;
    default:
        if (http.statusCode >= 500) {
            err.category = ErrorCategory::Unknown;
        } else if (http.statusCode >= 400) {
            err.category = ErrorCategory::InvalidResponse;
        } else {
            err.category = ErrorCategory::Unknown;
        }
        break;
    }

    if (!detail.empty()) {
        err.message = detail;
    } else if (!http.errorMessage.empty()) {
        err.message = http.errorMessage;
    } else {
        err.message = "HTTP " + std::to_string(http.statusCode);
    }
    return err;
}

// In-flight streaming context. A new instance lives for the duration of a
// single request; SSE callbacks update it from the HTTP worker thread.
struct StreamState {
    // Per content-block accumulators, indexed by "index" field sent by the
    // server. content_block_start decides whether a block is "text" or
    // "tool_use"; deltas and stops reference the same index.
    struct Block {
        std::string type;      // "text" | "tool_use"
        std::string text;      // for text blocks
        std::string toolId;    // for tool_use blocks
        std::string toolName;  // for tool_use blocks
        std::string toolArgs;  // accumulated input_json_delta fragments
    };

    std::map<int, Block> blocks;
    std::string accumulatedText;       // concatenation of all text blocks
    std::vector<ToolCall> toolCalls;   // finalized on content_block_stop

    bool completed = false;            // set on message_stop / errors
    bool errorSeen = false;
    ProviderError streamError;

    std::string runId;
    StreamCallback onToken;
};

// Handle one SSE event payload (the JSON blob from a "data:" line). Updates
// `state` and pushes UI messages / invokes token callbacks as needed.
void handleStreamEvent(const std::string& eventData, StreamState& state) {
    if (eventData.empty()) return;

    json ev;
    try {
        ev = json::parse(eventData);
    } catch (...) {
        // Anthropic sometimes sends ping events with empty payload; safely ignore.
        return;
    }
    if (!ev.is_object() || !ev.contains("type") || !ev["type"].is_string()) {
        return;
    }
    const std::string type = ev["type"].get<std::string>();

    if (type == "message_start") {
        // No-op for now; metadata only.
        return;
    }

    if (type == "content_block_start") {
        int idx = 0;
        if (!readStreamContentBlockIndex(ev, idx)) {
            return;
        }
        const json* cb = objectMember(ev, "content_block");
        if (!cb) {
            return;
        }
        StreamState::Block block;
        block.type = stringMemberOr(*cb, "type");
        if (block.type == "tool_use") {
            block.toolId   = stringMemberOr(*cb, "id");
            block.toolName = stringMemberOr(*cb, "name");
            // input may be {} initially; deltas stream the real value as JSON
            // fragments via input_json_delta.
        }
        state.blocks[idx] = std::move(block);
        return;
    }

    if (type == "content_block_delta") {
        int idx = 0;
        if (!readStreamContentBlockIndex(ev, idx)) {
            return;
        }
        auto it = state.blocks.find(idx);
        if (it == state.blocks.end()) {
            return;
        }
        const json* delta = objectMember(ev, "delta");
        if (!delta) {
            return;
        }
        const std::string deltaType = stringMemberOr(*delta, "type");
        if (deltaType == "text_delta") {
            const std::string chunk = stringMemberOr(*delta, "text");
            if (chunk.empty()) return;
            it->second.text += chunk;
            state.accumulatedText += chunk;
            if (state.onToken) {
                state.onToken(chunk);
            }
            UIMessage m;
            m.type = UIMessageType::Token;
            m.runId = state.runId;
            m.data = chunk;
            UIMessageQueue::getInstance().push(std::move(m));
        } else if (deltaType == "input_json_delta") {
            // Accumulate raw JSON fragments for the current tool_use block.
            const std::string chunk = stringMemberOr(*delta, "partial_json");
            it->second.toolArgs += chunk;
        }
        return;
    }

    if (type == "content_block_stop") {
        int idx = 0;
        if (!readStreamContentBlockIndex(ev, idx)) {
            return;
        }
        auto it = state.blocks.find(idx);
        if (it == state.blocks.end()) {
            return;
        }
        if (it->second.type == "tool_use") {
            ToolCall tc;
            tc.id        = it->second.toolId;
            tc.name      = it->second.toolName;
            // If no fragments arrived, keep an empty JSON object so the
            // downstream executor can parse without special-casing.
            tc.arguments = it->second.toolArgs.empty() ? std::string("{}")
                                                       : it->second.toolArgs;
            state.toolCalls.push_back(std::move(tc));
        }
        return;
    }

    if (type == "message_delta") {
        // Carries stop_reason / stop_sequence / usage; nothing to surface
        // during streaming.
        return;
    }

    if (type == "message_stop") {
        state.completed = true;
        return;
    }

    if (type == "error") {
        state.errorSeen = true;
        state.completed = true;
        ProviderError err;
        err.category = ErrorCategory::InvalidResponse;
        if (const json* e = objectMember(ev, "error")) {
            err.providerErrorCode = stringMemberOr(*e, "type");
            err.message           = stringMemberOr(*e, "message", "stream error");
        } else {
            err.message = "stream error";
        }
        state.streamError = std::move(err);
        return;
    }

    // "ping" and any unknown event types: ignore.
}

} // namespace

// ---------------------------------------------------------------------------
// ClaudeProvider implementation
// ---------------------------------------------------------------------------

void ClaudeProvider::configure(const ProviderConfig& config) {
    config_ = config;
    if (config_.baseUrl.empty()) {
        config_.baseUrl = getDefaultBaseUrl();
    }
    if (config_.model.empty()) {
        config_.model = "claude-sonnet-4-5";
    }
    if (config_.apiVersion.empty()) {
        config_.apiVersion = kDefaultApiVersion;
    }
}

const ProviderConfig& ClaudeProvider::getConfig() const {
    return config_;
}

std::string ClaudeProvider::buildRequestBody(const CompletionRequest& request) {
    json root = json::object();
    root["model"]      = request.model.empty() ? config_.model : request.model;
    root["max_tokens"] = kDefaultMaxTokens;
    root["stream"]     = request.stream;

    // 1. Hoist every System-role message into the top-level "system" field.
    //    Multiple system messages are concatenated with double newlines to
    //    preserve the authoring order without injecting ambiguous separators.
    std::string systemText;
    for (const auto& msg : request.messages) {
        if (msg.role == Role::System && !msg.content.empty()) {
            if (!systemText.empty()) systemText += "\n\n";
            systemText += msg.content;
        }
    }
    if (!systemText.empty()) {
        root["system"] = systemText;
    }

    // 2. Build the Messages array from user/assistant/tool messages.
    json messages = json::array();
    for (std::size_t i = 0; i < request.messages.size(); ++i) {
        const auto& msg = request.messages[i];
        if (msg.role == Role::System) continue;

        json m = json::object();
        m["role"] = roleToAnthropic(msg.role);

        if (msg.role == Role::Tool) {
            // Merge adjacent stored tool results into one Anthropic user
            // message so a multi-tool turn stays protocol-valid.
            json blocks = json::array();
            blocks.push_back(toolResultBlock(msg));
            while (i + 1 < request.messages.size() &&
                   request.messages[i + 1].role == Role::Tool) {
                ++i;
                blocks.push_back(toolResultBlock(request.messages[i]));
            }
            m["content"] = std::move(blocks);
            messages.push_back(std::move(m));
            continue;
        }

        if (msg.role == Role::Assistant && !msg.toolCalls.empty()) {
            // Assistant with tool calls → array content: optional text block
            // followed by one tool_use block per ToolCall.
            json blocks = json::array();
            if (!msg.content.empty()) {
                blocks.push_back({ {"type", "text"}, {"text", msg.content} });
            }
            for (const auto& tc : msg.toolCalls) {
                json b = json::object();
                b["type"]  = "tool_use";
                b["id"]    = tc.id;
                b["name"]  = tc.name;
                // tc.arguments is a JSON string; fall back to empty object
                // if the server ever handed us an unparseable fragment.
                b["input"] = parseJsonOr(tc.arguments, json::object());
                blocks.push_back(std::move(b));
            }
            m["content"] = std::move(blocks);
            messages.push_back(std::move(m));
            continue;
        }

        // Plain user or assistant text message.
        m["content"] = msg.content;
        messages.push_back(std::move(m));
    }
    root["messages"] = std::move(messages);

    // 3. Tool definitions use Anthropic's shape: {name, description,
    //    input_schema}. Callers supply input_schema as a JSON string; we
    //    reparse so it is emitted as a real object rather than a string.
    if (!request.tools.empty()) {
        json tools = json::array();
        for (const auto& t : request.tools) {
            json tool = json::object();
            tool["name"]         = t.name;
            tool["description"]  = t.description;
            tool["input_schema"] = parseJsonOr(t.parametersSchema, json::object());
            tools.push_back(std::move(tool));
        }
        root["tools"] = std::move(tools);
    }

    return root.dump();
}

CompletionResponse ClaudeProvider::parseFullResponse(const std::string& body) {
    CompletionResponse out;
    out.message.role = Role::Assistant;

    json j;
    try {
        j = json::parse(body);
    } catch (...) {
        out.error.category = ErrorCategory::InvalidResponse;
        out.error.message  = "failed to parse response JSON";
        return out;
    }

    if (!j.is_object()) {
        out.error.category = ErrorCategory::InvalidResponse;
        out.error.message  = "response JSON is not an object";
        return out;
    }

    // Error envelope: { "type": "error", "error": { ... } }
    if (stringMemberOr(j, "type") == "error") {
        out.error.category          = ErrorCategory::InvalidResponse;
        out.error.message           = "unknown error";
        if (const json* e = objectMember(j, "error")) {
            out.error.providerErrorCode = stringMemberOr(*e, "type");
            out.error.message           = stringMemberOr(*e, "message", "unknown error");
        }
        return out;
    }

    // Successful response: content is an array of text / tool_use blocks.
    if (j.contains("content") && j["content"].is_array()) {
        std::string text;
        for (const auto& block : j["content"]) {
            if (!block.is_object()) continue;
            const std::string bt = stringMemberOr(block, "type");
            if (bt == "text") {
                text += stringMemberOr(block, "text");
            } else if (bt == "tool_use") {
                ToolCall tc;
                tc.id        = stringMemberOr(block, "id");
                tc.name      = stringMemberOr(block, "name");
                // input is an object; re-serialize as a string so the rest of
                // the pipeline can treat arguments uniformly.
                const json* input = objectMember(block, "input");
                tc.arguments = input ? input->dump() : std::string("{}");
                out.message.toolCalls.push_back(std::move(tc));
            }
        }
        out.message.content = std::move(text);
    }

    return out;
}

void ClaudeProvider::sendCompletion(const CompletionRequest& request,
                                    CancellationToken cancelToken) {
    // Fail fast when required config is missing so callers get a clear error
    // message instead of a 401 from the upstream API.
    if (config_.apiKey.empty()) {
        CompletionResponse r;
        r.error.category = ErrorCategory::Authentication;
        r.error.message  = "Anthropic API key is not configured";
        UIMessage m;
        m.type     = UIMessageType::Error;
        m.runId    = request.runId;
        m.data     = r.error.message;
        m.response = r;
        UIMessageQueue::getInstance().push(std::move(m));
        if (request.onComplete) request.onComplete(std::move(r));
        return;
    }

    const std::string baseUrl = stripTrailingSlash(
        config_.baseUrl.empty() ? getDefaultBaseUrl() : config_.baseUrl);
    const std::string apiVer  = config_.apiVersion.empty() ? kDefaultApiVersion
                                                           : config_.apiVersion;
    const std::string url     = baseUrl + "/messages";

    std::map<std::string, std::string> headers;
    headers["x-api-key"]         = config_.apiKey;
    headers["anthropic-version"] = apiVer;
    headers["Content-Type"]      = "application/json";
    if (request.stream) {
        headers["Accept"] = "text/event-stream";
    }

    const std::string body = buildRequestBody(request);

    // Streaming state is shared between the SSE callback and the completion
    // callback (same HTTP worker thread invokes both, but shared_ptr keeps
    // the object alive across the callback transition regardless).
    auto state = std::make_shared<StreamState>();
    state->runId = request.runId;
    state->onToken = request.onToken;

    // Snapshot the completion callback so we can hand it through to the
    // completion lambda by value.
    CompletionCallback onComplete = request.onComplete;
    const bool streaming          = request.stream;

    SSECallback sseCb;
    if (streaming) {
        sseCb = [state](const std::string& evt) {
            handleStreamEvent(evt, *state);
        };
    }

    HttpCompletionCallback completeCb =
        [state, onComplete, streaming](HttpResponse http) {
        CompletionResponse final;
        final.message.role = Role::Assistant;

        if (http.cancelled) {
            final.error = mapError(http, http.body);
            UIMessage m;
            m.type     = UIMessageType::Error;
            m.runId    = state->runId;
            m.data     = final.error.message;
            m.response = final;
            UIMessageQueue::getInstance().push(std::move(m));
            if (onComplete) onComplete(std::move(final));
            return;
        }

        if (http.statusCode == 0 || http.statusCode < 200 || http.statusCode >= 300) {
            final.error = mapError(http, http.body);
            // Preserve any partial streaming content so the UI can show what
            // was received before the stream broke (Req 12.7).
            if (streaming) {
                final.message.content   = std::move(state->accumulatedText);
                final.message.toolCalls = std::move(state->toolCalls);
            }
            UIMessage m;
            m.type     = UIMessageType::Error;
            m.runId    = state->runId;
            m.data     = final.error.message;
            m.response = final;
            UIMessageQueue::getInstance().push(std::move(m));
            if (onComplete) onComplete(std::move(final));
            return;
        }

        if (streaming) {
            if (state->errorSeen) {
                final.error           = state->streamError;
                final.message.content = std::move(state->accumulatedText);
                final.message.toolCalls = std::move(state->toolCalls);
                UIMessage m;
                m.type     = UIMessageType::Error;
                m.runId    = state->runId;
                m.data     = final.error.message;
                m.response = final;
                UIMessageQueue::getInstance().push(std::move(m));
                if (onComplete) onComplete(std::move(final));
                return;
            }
            final.message.content   = std::move(state->accumulatedText);
            final.message.toolCalls = std::move(state->toolCalls);
        } else {
            // Non-streaming: parse the full JSON body returned by the API.
            final = ClaudeProvider::parseFullResponse(http.body);
        }

        UIMessage m;
        m.type     = UIMessageType::Completion;
        m.runId    = state->runId;
        m.response = final;
        UIMessageQueue::getInstance().push(std::move(m));
        if (onComplete) onComplete(std::move(final));
    };

    HttpClient::getInstance().postAsync(url,
                                        headers,
                                        body,
                                        std::move(sseCb),
                                        std::move(completeCb),
                                        std::move(cancelToken));
}

} // namespace AI

#endif // HAVE_AI_CHAT
