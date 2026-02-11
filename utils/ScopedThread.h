#pragma once

#include <thread>
#include <atomic>
#include <functional>

class ScopedThread {
public:
    ScopedThread() = default;
    ~ScopedThread() { stop(); }

    // 禁止拷贝
    ScopedThread(const ScopedThread&) = delete;
    ScopedThread& operator=(const ScopedThread&) = delete;

    // 启动线程，fn 的第一个参数为 const atomic<bool>& cancel
    template<typename Fn, typename... Args>
    void launch(Fn&& fn, Args&&... args) {
        stop();
        m_cancel.store(false, std::memory_order_relaxed);
        m_thread = std::thread(std::forward<Fn>(fn), std::ref(m_cancel), std::forward<Args>(args)...);
    }

    // 非阻塞请求取消
    void requestStop() {
        m_cancel.store(true, std::memory_order_release);
    }

    // 请求取消 + join
    void stop() {
        requestStop();
        if (m_thread.joinable())
            m_thread.join();
    }

    bool running() const {
        return m_thread.joinable();
    }

    const std::atomic<bool>& cancelToken() const { return m_cancel; }

private:
    std::thread m_thread;
    std::atomic<bool> m_cancel{false};
};
