#ifdef HAVE_AI_CHAT

#include "OpenAIProvider.h"

#include "HttpClient.h"
#include "UIMessageQueue.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace AI {

namespace {

using json = nlohmann::json;

// Map OpenAI role enum → wire string.
const char* roleToString(Role role) {
    switch (role) {
    case Role::System:    return "system";
    case Role::User:      return "user";
    case Role::Assistant: return "assistant";
    case Role::Tool:      return "tool";
    }
    return "user";
}

// Trim trailing '/' characters off a base URL so we can append a path cleanly.
std::string stripTrailingSlash(const std::string& url) {
    size_t end = url.size();
    while (end > 0 && url[end - 1] == '/') {
        --end;
    }
    return url.substr(0, end);
}

// Classify an HTTP response into a ProviderError. Pulls any human-readable
// message out of the error body ("error.message" / top-level "message")
// when the body is valid JSON.
ProviderError classifyHttpError(const HttpResponse& resp) {
    ProviderError err;
    err.httpStatusCode = resp.statusCode;

    if (resp.cancelled) {
        err.category = ErrorCategory::Cancelled;
        err.message = resp.errorMessage.empty() ? "request cancelled" : resp.errorMessage;
        return err;
    }
    if (resp.timedOut) {
        err.category = ErrorCategory::Timeout;
        err.message = resp.errorMessage.empty() ? "request timed out" : resp.errorMessage;
        return err;
    }
    if (resp.statusCode == 0) {
        err.category = ErrorCategory::Network;
        err.message = resp.errorMessage.empty() ? "network error" : resp.errorMessage;
        return err;
    }
    if (resp.statusCode >= 200 && resp.statusCode < 300) {
        err.category = ErrorCategory::None;
        return err;
    }

    // Try to surface the provider's own error message.
    std::string providerMessage;
    std::string providerCode;
    if (!resp.body.empty()) {
        try {
            auto j = json::parse(resp.body);
            if (j.is_object()) {
                if (j.contains("error") && j["error"].is_object()) {
                    const auto& e = j["error"];
                    if (e.contains("message") && e["message"].is_string()) {
                        providerMessage = e["message"].get<std::string>();
                    }
                    if (e.contains("code")) {
                        if (e["code"].is_string()) {
                            providerCode = e["code"].get<std::string>();
                        } else if (e["code"].is_number_integer()) {
                            providerCode = std::to_string(e["code"].get<long long>());
                        }
                    }
                } else if (j.contains("message") && j["message"].is_string()) {
                    providerMessage = j["message"].get<std::string>();
                }
            }
        } catch (const json::exception&) {
            // Fall through; keep raw status code message.
        }
    }

    switch (resp.statusCode) {
    case 401:
    case 403:
        err.category = ErrorCategory::Authentication;
        break;
    case 429:
        err.category = ErrorCategory::RateLimit;
        break;
    default:
        if (resp.statusCode >= 500 && resp.statusCode < 600) {
            err.category = ErrorCategory::Unknown;
        } else {
            err.category = ErrorCategory::Unknown;
        }
        break;
    }

    err.providerErrorCode = std::move(providerCode);
    if (!providerMessage.empty()) {
        err.message = "HTTP " + std::to_string(resp.statusCode) + ": " + providerMessage;
    } else if (!resp.errorMessage.empty()) {
        err.message = resp.errorMessage;
    } else {
        err.message = "HTTP " + std::to_string(resp.statusCode);
    }
    return err;
}

// Push a Completion message carrying `response` onto the UI queue and also
// invoke the caller-provided onComplete callback.
void deliverCompletion(const CompletionRequest& request, CompletionResponse response) {
    UIMessage msg;
    msg.type = UIMessageType::Completion;
    msg.runId = request.runId;
    msg.response = response;
    UIMessageQueue::getInstance().push(std::move(msg));
    if (request.onComplete) {
        request.onComplete(std::move(response));
    }
}

// Push a Token message onto the UI queue and invoke the caller-provided
// onToken callback.
void deliverToken(const CompletionRequest& request, const std::string& token) {
    if (token.empty()) {
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

// Serialize a single ChatMessage into an OpenAI-format message object.
json messageToJson(const ChatMessage& m) {
    json obj = json::object();
    obj["role"] = roleToString(m.role);

    if (m.role == Role::Tool) {
        obj["tool_call_id"] = m.toolCallId;
        obj["content"] = m.content;
        if (!m.name.empty()) {
            obj["name"] = m.name;
        }
        return obj;
    }

    // Assistant messages may carry tool_calls in addition to (possibly empty) content.
    obj["content"] = m.content;
    if (m.role == Role::Assistant && !m.toolCalls.empty()) {
        json calls = json::array();
        for (const auto& tc : m.toolCalls) {
            json call = json::object();
            call["id"] = tc.id;
            call["type"] = "function";
            json fn = json::object();
            fn["name"] = tc.name;
            // Arguments are a JSON-encoded string per OpenAI spec.
            fn["arguments"] = tc.arguments;
            call["function"] = std::move(fn);
            calls.push_back(std::move(call));
        }
        obj["tool_calls"] = std::move(calls);
    }
    return obj;
}

} // namespace

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

void OpenAIProvider::configure(const ProviderConfig& config) {
    config_ = config;
    if (config_.baseUrl.empty()) {
        config_.baseUrl = getDefaultBaseUrl();
    }
    if (config_.model.empty()) {
        config_.model = "gpt-5.4";
    }
}

const ProviderConfig& OpenAIProvider::getConfig() const {
    return config_;
}

// ---------------------------------------------------------------------------
// Request building
// ---------------------------------------------------------------------------

std::string OpenAIProvider::buildRequestBody(const CompletionRequest& request) const {
    json body = json::object();

    // Prefer request model, fall back to provider-configured model.
    body["model"] = !request.model.empty() ? request.model : config_.model;
    body["stream"] = request.stream;

    json messages = json::array();
    for (const auto& m : request.messages) {
        messages.push_back(messageToJson(m));
    }
    body["messages"] = std::move(messages);

    if (!request.tools.empty()) {
        json tools = json::array();
        for (const auto& t : request.tools) {
            json tool = json::object();
            tool["type"] = "function";
            json fn = json::object();
            fn["name"] = t.name;
            fn["description"] = t.description;

            // parametersSchema is stored as a JSON-encoded string. Embed the
            // parsed object so the API sees a real JSON schema, not a string.
            json params;
            if (!t.parametersSchema.empty()) {
                try {
                    params = json::parse(t.parametersSchema);
                } catch (const json::exception&) {
                    // Fall back to a permissive empty object schema.
                    params = json::object();
                    params["type"] = "object";
                    params["properties"] = json::object();
                }
            } else {
                params = json::object();
                params["type"] = "object";
                params["properties"] = json::object();
            }
            fn["parameters"] = std::move(params);

            tool["function"] = std::move(fn);
            tools.push_back(std::move(tool));
        }
        body["tools"] = std::move(tools);
    }

    return body.dump();
}

// ---------------------------------------------------------------------------
// SSE chunk parsing
// ---------------------------------------------------------------------------
//
// Returns a CompletionResponse describing the *delta* carried by a single SSE
// event. The caller (the streaming SSE callback in sendCompletion) is
// responsible for merging the delta into an accumulated state:
//
//   - response.message.content holds the content delta for this chunk.
//   - response.message.toolCalls is a vector positioned by OpenAI's
//     `index` field: the entry at position i corresponds to the tool_call
//     with index = i in the chunk. Entries not referenced by this chunk are
//     left empty (all fields ""). Each entry carries the *fragments* delivered
//     in this chunk only (id/name/arguments may each be empty strings when
//     absent from the chunk).
//   - response.error is set when the chunk could not be parsed as JSON or
//     had an unexpected structure.

CompletionResponse OpenAIProvider::parseSSEChunk(const std::string& chunk) const {
    CompletionResponse out;
    out.message.role = Role::Assistant;

    try {
        auto j = json::parse(chunk);
        if (!j.is_object() || !j.contains("choices") || !j["choices"].is_array() ||
            j["choices"].empty()) {
            // Non-fatal: some keep-alive / status-only chunks have no choices.
            return out;
        }

        const auto& choice = j["choices"][0];
        if (!choice.is_object() || !choice.contains("delta") || !choice["delta"].is_object()) {
            return out;
        }
        const auto& delta = choice["delta"];

        if (delta.contains("content") && delta["content"].is_string()) {
            out.message.content = delta["content"].get<std::string>();
        }

        if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
            for (const auto& tc : delta["tool_calls"]) {
                if (!tc.is_object()) {
                    continue;
                }
                int index = 0;
                if (tc.contains("index") && tc["index"].is_number_integer()) {
                    index = tc["index"].get<int>();
                }
                if (index < 0) {
                    continue;
                }
                if (static_cast<size_t>(index) >= out.message.toolCalls.size()) {
                    out.message.toolCalls.resize(index + 1);
                }
                ToolCall& slot = out.message.toolCalls[index];

                if (tc.contains("id") && tc["id"].is_string()) {
                    slot.id = tc["id"].get<std::string>();
                }
                if (tc.contains("function") && tc["function"].is_object()) {
                    const auto& fn = tc["function"];
                    if (fn.contains("name") && fn["name"].is_string()) {
                        slot.name = fn["name"].get<std::string>();
                    }
                    if (fn.contains("arguments") && fn["arguments"].is_string()) {
                        slot.arguments = fn["arguments"].get<std::string>();
                    }
                }
            }
        }
    } catch (const json::exception& e) {
        out.error.category = ErrorCategory::InvalidResponse;
        out.error.message = std::string("failed to parse SSE chunk: ") + e.what();
    }

    return out;
}

// ---------------------------------------------------------------------------
// Full (non-streaming) response parsing
// ---------------------------------------------------------------------------

CompletionResponse OpenAIProvider::parseFullResponse(const std::string& body) const {
    CompletionResponse out;
    out.message.role = Role::Assistant;

    try {
        auto j = json::parse(body);
        if (!j.is_object() || !j.contains("choices") || !j["choices"].is_array() ||
            j["choices"].empty()) {
            out.error.category = ErrorCategory::InvalidResponse;
            out.error.message = "OpenAI response missing 'choices'";
            return out;
        }

        const auto& choice = j["choices"][0];
        if (!choice.is_object() || !choice.contains("message") ||
            !choice["message"].is_object()) {
            out.error.category = ErrorCategory::InvalidResponse;
            out.error.message = "OpenAI response missing 'choices[0].message'";
            return out;
        }
        const auto& msg = choice["message"];

        if (msg.contains("content") && msg["content"].is_string()) {
            out.message.content = msg["content"].get<std::string>();
        }

        if (msg.contains("tool_calls") && msg["tool_calls"].is_array()) {
            for (const auto& tc : msg["tool_calls"]) {
                if (!tc.is_object()) {
                    continue;
                }
                ToolCall call;
                if (tc.contains("id") && tc["id"].is_string()) {
                    call.id = tc["id"].get<std::string>();
                }
                if (tc.contains("function") && tc["function"].is_object()) {
                    const auto& fn = tc["function"];
                    if (fn.contains("name") && fn["name"].is_string()) {
                        call.name = fn["name"].get<std::string>();
                    }
                    if (fn.contains("arguments")) {
                        // Arguments are conventionally a string, but tolerate
                        // already-parsed objects.
                        if (fn["arguments"].is_string()) {
                            call.arguments = fn["arguments"].get<std::string>();
                        } else {
                            call.arguments = fn["arguments"].dump();
                        }
                    }
                }
                out.message.toolCalls.push_back(std::move(call));
            }
        }
    } catch (const json::exception& e) {
        out.error.category = ErrorCategory::InvalidResponse;
        out.error.message = std::string("failed to parse OpenAI response: ") + e.what();
    }

    return out;
}

// ---------------------------------------------------------------------------
// sendCompletion
// ---------------------------------------------------------------------------

void OpenAIProvider::sendCompletion(const CompletionRequest& request,
                                    CancellationToken cancelToken) {
    const std::string base = stripTrailingSlash(
        !config_.baseUrl.empty() ? config_.baseUrl : getDefaultBaseUrl());
    const std::string url = base + "/chat/completions";

    const std::string body = buildRequestBody(request);

    std::map<std::string, std::string> headers;
    headers["Authorization"] = "Bearer " + config_.apiKey;
    headers["Content-Type"] = "application/json";
    if (request.stream) {
        headers["Accept"] = "text/event-stream";
    } else {
        headers["Accept"] = "application/json";
    }

    if (request.stream) {
        // Shared mutable state owned by the SSE + completion callbacks.
        // Both callbacks run on the HttpClient worker thread sequentially,
        // so a single mutex is enough to guard against torn reads should
        // cpp-httplib ever deliver concurrent callbacks.
        auto content = std::make_shared<std::string>();
        auto toolCalls = std::make_shared<std::vector<ToolCall>>();
        auto mu = std::make_shared<std::mutex>();
        auto requestCopy = std::make_shared<CompletionRequest>(request);

        SSECallback sseCb = [this, content, toolCalls, mu, requestCopy](
                                const std::string& eventData) {
            CompletionResponse delta = this->parseSSEChunk(eventData);
            if (delta.error) {
                // Surface the parse failure to the UI via a Token message is
                // inappropriate; deliver it as a completion-style error on
                // the UI queue so the window can show it inline.
                UIMessage msg;
                msg.type = UIMessageType::Error;
                msg.runId = requestCopy->runId;
                msg.data = delta.error.message;
                UIMessageQueue::getInstance().push(std::move(msg));
                return;
            }

            std::lock_guard<std::mutex> lock(*mu);

            if (!delta.message.content.empty()) {
                content->append(delta.message.content);
                deliverToken(*requestCopy, delta.message.content);
            }

            if (!delta.message.toolCalls.empty()) {
                if (toolCalls->size() < delta.message.toolCalls.size()) {
                    toolCalls->resize(delta.message.toolCalls.size());
                }
                for (size_t i = 0; i < delta.message.toolCalls.size(); ++i) {
                    const ToolCall& src = delta.message.toolCalls[i];
                    ToolCall& dst = (*toolCalls)[i];
                    if (!src.id.empty()) {
                        dst.id = src.id;
                    }
                    if (!src.name.empty()) {
                        dst.name = src.name;
                    }
                    // Arguments arrive as incremental string fragments — append.
                    if (!src.arguments.empty()) {
                        dst.arguments.append(src.arguments);
                    }
                }
            }
        };

        HttpCompletionCallback doneCb = [content, toolCalls, mu, requestCopy](
                                            HttpResponse httpResp) {
            CompletionResponse resp;
            resp.message.role = Role::Assistant;

            ProviderError err = classifyHttpError(httpResp);
            if (err) {
                resp.error = std::move(err);
            } else {
                std::lock_guard<std::mutex> lock(*mu);
                resp.message.content = *content;
                // Drop any empty trailing slots that never received a name
                // (defensive — OpenAI should populate name on the first chunk).
                for (auto& tc : *toolCalls) {
                    if (!tc.name.empty() || !tc.id.empty() || !tc.arguments.empty()) {
                        resp.message.toolCalls.push_back(tc);
                    }
                }
            }

            deliverCompletion(*requestCopy, std::move(resp));
        };

        HttpClient::getInstance().postAsync(url, headers, body,
                                            std::move(sseCb), std::move(doneCb),
                                            std::move(cancelToken));
    } else {
        auto requestCopy = std::make_shared<CompletionRequest>(request);

        HttpCompletionCallback doneCb = [this, requestCopy](HttpResponse httpResp) {
            CompletionResponse resp;
            resp.message.role = Role::Assistant;

            ProviderError err = classifyHttpError(httpResp);
            if (err) {
                resp.error = std::move(err);
            } else {
                resp = this->parseFullResponse(httpResp.body);
            }

            deliverCompletion(*requestCopy, std::move(resp));
        };

        HttpClient::getInstance().postAsync(url, headers, body,
                                            /*onSSE=*/nullptr, std::move(doneCb),
                                            std::move(cancelToken));
    }
}

} // namespace AI

#endif // HAVE_AI_CHAT
