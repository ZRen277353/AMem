#pragma once
#ifdef HAVE_AI_CHAT

#include "AIProvider.h"

#include <mutex>
#include <queue>
#include <string>

namespace AI {

// UI 消息类型
// 使用 enum class 避免与其他模块的符号冲突
enum class UIMessageType {
    Token,      // 流式 token，累加到当前 assistant 消息
    Completion, // 一次完整的响应（含可选的 tool_calls / error）
    Error       // 通信或解析错误
};

// 后台线程 → 主线程（ImGui）消息
struct UIMessage {
    UIMessageType type = UIMessageType::Token;
    std::string runId;
    std::string data;                 // Token 内容 or Error 描述
    CompletionResponse response;      // Completion 时的完整响应
};

// 线程安全队列，Meyer's singleton
//
// 生产者：HttpClient 后台线程、Provider SSE 解析回调
// 消费者：ImGui 主线程每帧 pollMessages()
class UIMessageQueue {
public:
    static UIMessageQueue& getInstance() {
        static UIMessageQueue instance;
        return instance;
    }

    // 推入一条消息（线程安全）
    void push(UIMessage msg);

    // 尝试弹出一条消息
    // 返回 true 表示成功弹出并写入 outMsg，false 表示队列为空
    bool tryPop(UIMessage& outMsg);

    // 队列是否为空（线程安全）
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
