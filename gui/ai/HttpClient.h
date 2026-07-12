#pragma once
#ifdef HAVE_AI_CHAT

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace AI {

// HTTP 响应结果
struct HttpResponse {
    int statusCode = 0;
    std::string body;
    std::string errorMessage;
    bool cancelled = false;
    bool timedOut = false;
};

// HTTP 代理配置
struct ProxyConfig {
    std::string host;
    int port = 0;
    bool enabled = false;
};

// SSE 事件回调（在后台线程调用）
// eventData 是一个完整 SSE 事件的 data 字段拼接结果，已剥离 "data: " 前缀
using SSECallback = std::function<void(const std::string& eventData)>;

// HTTP 完成回调（在后台线程调用）
using HttpCompletionCallback = std::function<void(HttpResponse response)>;
using CancellationToken = std::shared_ptr<std::atomic<bool>>;

// 异步 HTTPS 客户端（Meyer's 单例）
class HttpClient {
public:
    static HttpClient& getInstance() {
        static HttpClient instance;
        return instance;
    }

    // 配置
    void setConnectionTimeout(int seconds); // default 30
    void setResponseTimeout(int seconds);   // default 300
    void setProxy(const ProxyConfig& proxy);

    int getConnectionTimeout() const;
    int getResponseTimeout() const;
    ProxyConfig getProxy() const;

    // 异步 POST 请求（支持 SSE 流式响应）
    // 返回请求 ID，可用于 cancelRequest()
    // onSSE 每次接收到一个完整 SSE 事件（以空行分隔）时被调用
    // onComplete 在请求结束（成功/失败/取消）时被调用
    // cancelToken is owned by the async request; cancelRequest() sets it.
    uint64_t postAsync(const std::string& url,
                       const std::map<std::string, std::string>& headers,
                       const std::string& body,
                       SSECallback onSSE,
                       HttpCompletionCallback onComplete,
                       CancellationToken cancelToken);

    // 取消进行中的请求
    void cancelRequest(uint64_t requestId);

    // Stop accepting requests, cancel active transports, and join every owned
    // worker after its completion callback returns. Returns false only when
    // called from one of this client's own callbacks, where joining the
    // current thread would deadlock. Call again from the main thread to drain.
    bool shutdown();

private:
    HttpClient() = default;
    ~HttpClient();
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    // 配置字段（setter/getter 已加锁）
    mutable std::mutex configMutex_;
    int connectionTimeout_ = 30;
    int responseTimeout_ = 300;
    ProxyConfig proxy_;

    struct ActiveRequest {
        CancellationToken cancelToken;
        std::function<void()> stopTransport;
        std::thread worker;
        bool completed = false;
    };

    void markRequestCompleted(uint64_t requestId);
    void reapCompletedWorkers();

    // Active and completed-but-not-yet-joined request workers. A completed
    // record remains here until the next dispatch or shutdown joins it.
    std::mutex activeMutex_;
    std::unordered_map<uint64_t, std::unique_ptr<ActiveRequest>> activeRequests_;
    std::mutex workerJoinMutex_;
    bool shuttingDown_ = false; // set by shutdown(); guarded by activeMutex_

    std::atomic<uint64_t> nextRequestId_{1};
};

} // namespace AI

#endif // HAVE_AI_CHAT
