#pragma once
#ifdef HAVE_AI_CHAT

#include "ToolExecutor.h"

#include <mutex>
#include <queue>
#include <string>

namespace AI {

enum class UIMessageType {
    Token,
    Completion,
    Error,
    ToolResult
};

// Background threads push messages here; ChatWindow consumes them from the
// ImGui thread in pollMessages().
struct UIMessage {
    UIMessageType type = UIMessageType::Token;
    std::string runId;
    std::string data;
    CompletionResponse response;
    ToolCall toolCall;
    AI::ToolResult toolResult;
    long long durationMs = 0;
};

class UIMessageQueue {
public:
    static UIMessageQueue& getInstance() {
        static UIMessageQueue instance;
        return instance;
    }

    void push(UIMessage msg);
    bool tryPop(UIMessage& outMsg);
    bool empty() const;

private:
    UIMessageQueue() = default;
    ~UIMessageQueue() = default;
    UIMessageQueue(const UIMessageQueue&) = delete;
    UIMessageQueue& operator=(const UIMessageQueue&) = delete;

    mutable std::mutex mutex_;
    std::queue<UIMessage> queue_;
};

} // namespace AI

#endif // HAVE_AI_CHAT
