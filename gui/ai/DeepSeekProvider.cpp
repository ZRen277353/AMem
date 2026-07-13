#ifdef HAVE_AI_CHAT

#include "DeepSeekProvider.h"

#include "AiLimits.h"
#include "HttpClient.h"
#include "ProviderResponseLimits.h"
#include "ProviderStreamTerminal.h"
#include "UIMessageQueue.h"

#include "../../third_party/nlohmann/json.hpp"

#include <atomic>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

namespace AI {

using nlohmann::json;

namespace {

constexpr unsigned long long kMaxStreamToolCallIndex = 63ULL;

bool readStreamToolCallIndex(const json& object, size_t& outIndex) {
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
        if (index > kMaxStreamToolCallIndex) {
            return false;
        }
        outIndex = static_cast<size_t>(index);
        return true;
    }

    if (it->is_number_integer()) {
        const auto index = it->get<long long>();
        if (index < 0 || static_cast<unsigned long long>(index) > kMaxStreamToolCallIndex) {
            return false;
        }
        outIndex = static_cast<size_t>(index);
        return true;
    }

    return false;
}

// Map an AI::Role to the OpenAI/DeepSeek role string.
const char* roleToString(Role role) {
    switch (role) {
    case Role::System:    return "system";
    case Role::User:      return "user";
    case Role::Assistant: return "assistant";
    case Role::Tool:      return "tool";
    }
    return "user";
}

// Map an HTTP status code to the appropriate ErrorCategory.
ErrorCategory categorizeHttpStatus(int status) {
    if (status == 401 || status == 403) return ErrorCategory::Authentication;
    if (status == 429)                  return ErrorCategory::RateLimit;
    if (status >= 500)                  return ErrorCategory::Network;
    if (status >= 400)                  return ErrorCategory::InvalidResponse;
    return ErrorCategory::Unknown;
}

// Attempt to parse a provider-side error message from a DeepSeek error body.
// Returns an empty string on failure. DeepSeek/OpenAI format:
//   { "error": { "message": "...", "type": "...", "code": "..." } }
struct ExtractedError {
    std::string message;
    std::string code;
};

ExtractedError extractProviderError(const std::string& body) {
    ExtractedError out;
    try {
        json j = json::parse(body);
        if (j.contains("error") && j["error"].is_object()) {
            const auto& e = j["error"];
            if (e.contains("message") && e["message"].is_string()) {
                out.message = e["message"].get<std::string>();
            }
            if (e.contains("code")) {
                if (e["code"].is_string()) {
                    out.code = e["code"].get<std::string>();
                } else {
                    out.code = e["code"].dump();
                }
            }
        }
    } catch (const std::exception&) {
        // Leave output empty; caller falls back to raw body.
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

void DeepSeekProvider::configure(const ProviderConfig& config) {
    config_ = config;
    if (config_.baseUrl.empty()) {
        config_.baseUrl = getDefaultBaseUrl();
    }
    if (config_.model.empty()) {
        config_.model = "deepseek-chat";
    }
}

const ProviderConfig& DeepSeekProvider::getConfig() const {
    return config_;
}

// ---------------------------------------------------------------------------
// Request body
// ---------------------------------------------------------------------------

std::string DeepSeekProvider::buildRequestBody(const CompletionRequest& request) {
    json body;
    body["model"] = request.model.empty() ? config_.model : request.model;
    body["stream"] = request.stream;

    // messages
    json messages = json::array();
    for (const auto& msg : request.messages) {
        json m;
        m["role"] = roleToString(msg.role);

        // tool role → OpenAI-compatible tool message
        if (msg.role == Role::Tool) {
            m["content"] = msg.content;
            if (!msg.toolCallId.empty()) {
                m["tool_call_id"] = msg.toolCallId;
            }
            if (!msg.name.empty()) {
                m["name"] = msg.name;
            }
            messages.push_back(std::move(m));
            continue;
        }

        // assistant with tool_calls
        if (msg.role == Role::Assistant && !msg.toolCalls.empty()) {
            // content may be empty string when the assistant only emits tool_calls.
            m["content"] = msg.content;
            json tcs = json::array();
            for (const auto& tc : msg.toolCalls) {
                json entry;
                entry["id"] = tc.id;
                entry["type"] = "function";
                json fn;
                fn["name"] = tc.name;
                // OpenAI/DeepSeek expect arguments as a JSON-encoded string.
                fn["arguments"] = tc.arguments;
                entry["function"] = std::move(fn);
                tcs.push_back(std::move(entry));
            }
            m["tool_calls"] = std::move(tcs);
            messages.push_back(std::move(m));
            continue;
        }

        m["content"] = msg.content;
        messages.push_back(std::move(m));
    }
    body["messages"] = std::move(messages);

    // tools
    if (!request.tools.empty()) {
        json tools = json::array();
        for (const auto& td : request.tools) {
            json tool;
            tool["type"] = "function";
            json fn;
            fn["name"] = td.name;
            fn["description"] = td.description;
            // parametersSchema is a JSON Schema string; parse into object.
            if (!td.parametersSchema.empty()) {
                try {
                    fn["parameters"] = json::parse(td.parametersSchema);
                } catch (const std::exception&) {
                    // Fallback: empty object schema rather than malformed JSON.
                    fn["parameters"] = json::object({ {"type", "object"} });
                }
            } else {
                fn["parameters"] = json::object({ {"type", "object"} });
            }
            tool["function"] = std::move(fn);
            tools.push_back(std::move(tool));
        }
        body["tools"] = std::move(tools);
    }

    return body.dump();
}

// ---------------------------------------------------------------------------
// SSE parsing
// ---------------------------------------------------------------------------

void DeepSeekProvider::parseSSEChunk(const std::string& eventData,
                                     const CompletionRequest& request,
                                     ChatMessage& outMessage,
                                     size_t& totalArgumentBytes,
                                     std::string& limitError) {
    if (eventData == "[DONE]") {
        return;
    }

    json chunk;
    try {
        chunk = json::parse(eventData);
    } catch (const std::exception&) {
        // Ignore malformed chunks; the overall request will still produce a
        // final response, and invalid full payloads are caught elsewhere.
        return;
    }

    if (!chunk.contains("choices") || !chunk["choices"].is_array() ||
        chunk["choices"].empty()) {
        return;
    }

    const auto& choice = chunk["choices"][0];

    // Delta content
    if (choice.contains("delta") && choice["delta"].is_object()) {
        const auto& delta = choice["delta"];

        if (delta.contains("content") && delta["content"].is_string()) {
            const std::string& token =
                delta["content"].get_ref<const std::string&>();
            if (!token.empty()) {
                if (!appendAssistantContentWithinLimit(
                        outMessage.content, token, limitError)) {
                    return;
                }
                UIMessage msg;
                msg.type = UIMessageType::Token;
                msg.runId = request.runId;
                msg.data = token;
                UIMessageQueue::getInstance().push(std::move(msg));
                if (request.onToken) {
                    request.onToken(token);
                }
            }
        }

        // Accumulate tool_call fragments.
        //
        // OpenAI-compatible streaming emits tool_calls as a list of partial
        // entries, each identified by `index`. The first chunk for an index
        // carries id + function.name; subsequent chunks append argument
        // fragments. We assemble them into outMessage.toolCalls by index.
        if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
            for (const auto& tcFrag : delta["tool_calls"]) {
                if (!tcFrag.is_object()) continue;

                size_t idx = 0;
                if (!readStreamToolCallIndex(tcFrag, idx)) {
                    continue;
                }
                while (outMessage.toolCalls.size() <= idx) {
                    outMessage.toolCalls.emplace_back();
                }
                ToolCall& target = outMessage.toolCalls[idx];

                if (tcFrag.contains("id") && tcFrag["id"].is_string()) {
                    const std::string& id =
                        tcFrag["id"].get_ref<const std::string&>();
                    if (id.size() > Limits::kMaxToolCallIdBytes) {
                        limitError = "tool call id exceeds 256-byte limit";
                        return;
                    }
                    target.id = id;
                }
                if (tcFrag.contains("function") && tcFrag["function"].is_object()) {
                    const auto& fn = tcFrag["function"];
                    if (fn.contains("name") && fn["name"].is_string()) {
                        const std::string& name =
                            fn["name"].get_ref<const std::string&>();
                        if (name.size() > Limits::kMaxToolCallNameBytes) {
                            limitError =
                                "tool call name exceeds 64-byte limit";
                            return;
                        }
                        target.name = name;
                    }
                    if (fn.contains("arguments") && fn["arguments"].is_string()) {
                        const std::string& fragment =
                            fn["arguments"].get_ref<const std::string&>();
                        if (!appendToolArgumentsWithinLimit(
                                target, fragment, totalArgumentBytes,
                                limitError)) {
                            return;
                        }
                    }
                }
            }
        }
    }

}

// ---------------------------------------------------------------------------
// Full (non-streaming) response parsing
// ---------------------------------------------------------------------------

CompletionResponse DeepSeekProvider::parseFullResponse(const std::string& body) {
    CompletionResponse response;
    response.message.role = Role::Assistant;

    if (body.size() > Limits::kMaxHttpResponseBytes) {
        response.error = providerLimitError(
            "HTTP response exceeds 16 MiB limit");
        return response;
    }

    json j;
    try {
        j = json::parse(body);
    } catch (const std::exception& ex) {
        response.error.category = ErrorCategory::InvalidResponse;
        response.error.message =
            std::string("DeepSeek returned invalid JSON: ") + ex.what();
        return response;
    }

    if (!j.contains("choices") || !j["choices"].is_array() || j["choices"].empty()) {
        response.error.category = ErrorCategory::InvalidResponse;
        response.error.message = "DeepSeek response missing 'choices' array";
        return response;
    }

    const auto& choice = j["choices"][0];
    if (!choice.contains("message") || !choice["message"].is_object()) {
        response.error.category = ErrorCategory::InvalidResponse;
        response.error.message = "DeepSeek response missing 'message' object";
        return response;
    }

    const auto& msg = choice["message"];
    if (msg.contains("content") && msg["content"].is_string()) {
        std::string error;
        if (!appendAssistantContentWithinLimit(
                response.message.content,
                msg["content"].get_ref<const std::string&>(), error)) {
            response.error = providerLimitError(error);
            return response;
        }
    }

    size_t totalArgumentBytes = 0;
    if (msg.contains("tool_calls") && msg["tool_calls"].is_array()) {
        for (const auto& tc : msg["tool_calls"]) {
            if (!tc.is_object()) continue;
            if (response.message.toolCalls.size() >=
                Limits::kMaxToolCallsPerMessage) {
                response.error = providerLimitError(
                    "assistant response exceeds 64 tool calls");
                return response;
            }
            ToolCall call;
            if (tc.contains("id") && tc["id"].is_string()) {
                const std::string& id =
                    tc["id"].get_ref<const std::string&>();
                if (id.size() > Limits::kMaxToolCallIdBytes) {
                    response.error = providerLimitError(
                        "tool call id exceeds 256-byte limit");
                    return response;
                }
                call.id = id;
            }
            if (tc.contains("function") && tc["function"].is_object()) {
                const auto& fn = tc["function"];
                if (fn.contains("name") && fn["name"].is_string()) {
                    const std::string& name =
                        fn["name"].get_ref<const std::string&>();
                    if (name.size() > Limits::kMaxToolCallNameBytes) {
                        response.error = providerLimitError(
                            "tool call name exceeds 64-byte limit");
                        return response;
                    }
                    call.name = name;
                }
                if (fn.contains("arguments")) {
                    std::string arguments;
                    if (fn["arguments"].is_string()) {
                        arguments = fn["arguments"].get<std::string>();
                    } else {
                        // Some servers may embed arguments as a JSON object;
                        // preserve the original encoding as a string.
                        arguments = fn["arguments"].dump();
                    }
                    std::string error;
                    if (!appendToolArgumentsWithinLimit(
                            call, arguments, totalArgumentBytes, error)) {
                        response.error = providerLimitError(error);
                        return response;
                    }
                }
            }
            response.message.toolCalls.push_back(std::move(call));
        }
    }

    // Requirement 4.5: validate tool_call arguments are well-formed JSON.
    validateToolCallArguments(response.message, response);

    if (!response.error) {
        std::string limitError;
        if (!validateProviderMessageLimits(response.message, limitError)) {
            response.error = providerLimitError(limitError);
            response.message.content.clear();
            response.message.toolCalls.clear();
        }
    }
    return response;
}

// ---------------------------------------------------------------------------
// Tool-call argument validation (Req 4.5)
// ---------------------------------------------------------------------------

bool DeepSeekProvider::validateToolCallArguments(const ChatMessage& message,
                                                 CompletionResponse& response) {
    for (const auto& tc : message.toolCalls) {
        // Arguments may legitimately be empty for tools with no parameters;
        // in that case we skip parsing.
        if (tc.arguments.empty()) continue;

        try {
            (void)json::parse(tc.arguments);
        } catch (const std::exception& ex) {
            std::ostringstream oss;
            oss << "DeepSeek returned invalid JSON in tool_call arguments: "
                << tc.name << " (parse error: " << ex.what() << ")";
            response.error.category = ErrorCategory::InvalidResponse;
            response.error.message = oss.str();
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// sendCompletion
// ---------------------------------------------------------------------------

void DeepSeekProvider::sendCompletion(const CompletionRequest& request,
                                      CancellationToken cancelToken) {
    // Build URL: {baseUrl}/chat/completions (no /v1 prefix for DeepSeek)
    std::string baseUrl = config_.baseUrl.empty() ? getDefaultBaseUrl() : config_.baseUrl;
    // Trim trailing slash to avoid double-slash in the final URL.
    while (!baseUrl.empty() && baseUrl.back() == '/') {
        baseUrl.pop_back();
    }
    const std::string url = baseUrl + "/chat/completions";

    std::map<std::string, std::string> headers;
    headers["Authorization"] = "Bearer " + config_.apiKey;
    headers["Content-Type"] = "application/json";
    if (request.stream) {
        headers["Accept"] = "text/event-stream";
    }

    const std::string body = buildRequestBody(request);

    // Shared state captured by both SSE and completion callbacks. The
    // HttpClient invokes callbacks on its background thread; the state lives
    // on the heap via shared_ptr so both callbacks can observe it safely
    // throughout the request's lifetime.
    struct StreamState {
        ChatMessage assembled;
        StreamTerminalTracker terminal;
        ProviderError limitError;
        size_t totalArgumentBytes = 0;
    };
    auto state = std::make_shared<StreamState>();
    state->assembled.role = Role::Assistant;

    const bool streaming = request.stream;

    SSECallback onSSE;
    if (streaming) {
        onSSE = [this, state, request](const std::string& eventData) {
            state->terminal.observe(
                InspectOpenAICompatibleStreamEvent(eventData, "DeepSeek"));
            std::string error;
            parseSSEChunk(eventData, request, state->assembled,
                          state->totalArgumentBytes, error);
            if (!error.empty()) {
                state->limitError = providerLimitError(error);
                state->assembled.content.clear();
                state->assembled.toolCalls.clear();
                throw std::runtime_error(error);
            }
        };
    }

    HttpCompletionCallback onComplete =
        [this, state, request, streaming](HttpResponse http) {
            CompletionResponse resp;
            resp.message.role = Role::Assistant;
            if (streaming) {
                resp.message = state->assembled;
            }

            if (http.cancelled) {
                resp.error.category = ErrorCategory::Cancelled;
                resp.error.message = "request cancelled";
            } else if (state->limitError) {
                resp.error = state->limitError;
            } else if (http.limitExceeded) {
                resp.error = providerLimitError(http.errorMessage.empty()
                    ? "provider response exceeds configured limit"
                    : http.errorMessage);
            } else if (http.timedOut) {
                resp.error.category = ErrorCategory::Timeout;
                resp.error.message = http.errorMessage.empty()
                    ? "request timed out"
                    : http.errorMessage;
            } else if (http.statusCode == 0) {
                resp.error.category = ErrorCategory::Network;
                resp.error.message = http.errorMessage.empty()
                    ? "network error"
                    : http.errorMessage;
            } else if (http.statusCode < 200 || http.statusCode >= 300) {
                resp.error.category = categorizeHttpStatus(http.statusCode);
                resp.error.httpStatusCode = http.statusCode;
                ExtractedError ex = extractProviderError(http.body);
                std::ostringstream oss;
                oss << "DeepSeek HTTP " << http.statusCode;
                if (!ex.message.empty()) {
                    oss << ": " << ex.message;
                } else if (!http.body.empty()) {
                    oss << ": " << http.body;
                }
                resp.error.message = oss.str();
                resp.error.providerErrorCode = ex.code;
            } else if (streaming) {
                if (state->terminal.applyValidation("DeepSeek", resp)) {
                    validateToolCallArguments(resp.message, resp);
                }
            } else {
                // Success + non-streaming: parse the full JSON body.
                resp = parseFullResponse(http.body);
            }

            UIMessage msg;
            msg.type = UIMessageType::Completion;
            msg.runId = request.runId;
            msg.response = resp;
            UIMessageQueue::getInstance().push(std::move(msg));

            if (request.onComplete) {
                request.onComplete(std::move(resp));
            }
        };

    // Fire the request. The cancellation token is shared with HttpClient so
    // it can outlive the UI object that started the request.
    (void)HttpClient::getInstance().postAsync(url,
                                              headers,
                                              body,
                                              std::move(onSSE),
                                              std::move(onComplete),
                                              std::move(cancelToken));
}

} // namespace AI

#endif // HAVE_AI_CHAT
