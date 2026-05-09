#pragma once
#ifdef HAVE_AI_CHAT

#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace AI {

// 错误类别
enum class ErrorCategory {
    None,
    Network,
    Authentication,
    RateLimit,
    InvalidResponse,
    Cancelled,
    Timeout,
    Unknown
};

// 错误对象
struct ProviderError {
    ErrorCategory category = ErrorCategory::None;
    std::string message;
    int httpStatusCode = 0;
    std::string providerErrorCode;

    explicit operator bool() const { return category != ErrorCategory::None; }
};

// 聊天消息角色
enum class Role {
    System,
    User,
    Assistant,
    Tool
};

// 工具调用
struct ToolCall {
    std::string id;
    std::string name;
    std::string arguments; // JSON string
};

// 聊天消息
struct ChatMessage {
    Role role = Role::User;
    std::string content;
    std::vector<ToolCall> toolCalls;
    std::string toolCallId; // For tool role messages
    std::string name;       // Tool name for tool results

    // Unix timestamp (seconds since epoch) of when the message was
    // created. 0 means "unknown / legacy entry". Rendered next to each
    // message in the chat window so users can see conversation pacing.
    long long timestamp = 0;

    // How long (milliseconds) the message took to produce. Only populated
    // for assistant messages — measured from the moment the request was
    // dispatched until the final Completion event arrived. 0 for other
    // roles or when the measurement wasn't captured (e.g. a session
    // loaded from an older file).
    long long durationMs = 0;
};

// 工具定义
struct ToolDefinition {
    std::string name;
    std::string description;
    std::string parametersSchema; // JSON Schema string
};

// 工具安全级别
enum class ToolSafety {
    ReadOnly,
    Write
};

// Provider 能力
struct ProviderCapabilities {
    bool supportsStreaming = false;
    bool supportsToolCalling = false;
    int maxContextTokens = 4096;
};

// 完成响应
struct CompletionResponse {
    ChatMessage message;
    ProviderError error;
};

// 流式回调
using StreamCallback = std::function<void(const std::string& token)>;
using CompletionCallback = std::function<void(CompletionResponse response)>;

// 请求参数
struct CompletionRequest {
    std::vector<ChatMessage> messages;
    std::vector<ToolDefinition> tools;
    std::string model;
    bool stream = true;
    StreamCallback onToken;
    CompletionCallback onComplete;
};

// Provider 配置
struct ProviderConfig {
    std::string apiKey;
    std::string baseUrl;
    std::string model;
    std::string apiVersion; // For Anthropic
};

// AI Provider 抽象接口
class AIProvider {
public:
    virtual ~AIProvider() = default;

    virtual std::string getName() const = 0;
    virtual std::string getDefaultBaseUrl() const = 0;
    virtual ProviderCapabilities getCapabilities() const = 0;

    virtual void configure(const ProviderConfig& config) = 0;
    virtual const ProviderConfig& getConfig() const = 0;

    // 发送聊天完成请求（异步，在后台线程调用）
    virtual void sendCompletion(const CompletionRequest& request,
                                std::atomic<bool>& cancelFlag) = 0;
};

} // namespace AI

#endif // HAVE_AI_CHAT
