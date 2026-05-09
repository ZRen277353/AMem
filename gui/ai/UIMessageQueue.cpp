#ifdef HAVE_AI_CHAT

#include "UIMessageQueue.h"

#include <utility>

namespace AI {

void UIMessageQueue::push(UIMessage msg) {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.emplace(std::move(msg));
}

bool UIMessageQueue::tryPop(UIMessage& outMsg) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty()) {
        return false;
    }
    outMsg = std::move(queue_.front());
    queue_.pop();
    return true;
}

bool UIMessageQueue::empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.empty();
}

} // namespace AI

#endif // HAVE_AI_CHAT
