#ifdef HAVE_AI_CHAT

#include "HttpClient.h"
#include "SSEParser.h"

// cpp-httplib is header-only and provides HTTPS support when compiled with
// CPPHTTPLIB_OPENSSL_SUPPORT. The CMake task gates this include behind
// HAVE_AI_CHAT so the header is only required when AI chat is enabled.
#include "httplib.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <exception>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace AI {

namespace {

thread_local const HttpClient* currentHttpClientWorker = nullptr;

class HttpWorkerThreadMarker {
public:
    explicit HttpWorkerThreadMarker(const HttpClient* owner)
        : previous_(currentHttpClientWorker) {
        currentHttpClientWorker = owner;
    }

    ~HttpWorkerThreadMarker() {
        currentHttpClientWorker = previous_;
    }

private:
    const HttpClient* previous_;
};

template <typename Callback>
class ScopeExit {
public:
    explicit ScopeExit(Callback callback)
        : callback_(std::move(callback)) {}

    ~ScopeExit() { callback_(); }

    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;

private:
    Callback callback_;
};

template <typename Callback>
ScopeExit<Callback> makeScopeExit(Callback callback) {
    return ScopeExit<Callback>(std::move(callback));
}

// 解析 URL：提取 scheme/host/port/path
// 返回 cpp-httplib Client 可接受的 base ("scheme://host[:port]") 和 path
struct ParsedUrl {
    std::string base;  // e.g. "https://api.openai.com"
    std::string path;  // e.g. "/v1/chat/completions"
    bool isHttps = false;
    bool valid = false;
};

ParsedUrl parseUrl(const std::string& url) {
    ParsedUrl out;
    auto schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) {
        return out;
    }
    std::string scheme = url.substr(0, schemeEnd);
    std::transform(scheme.begin(), scheme.end(), scheme.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (scheme != "http" && scheme != "https") {
        return out;
    }
    out.isHttps = (scheme == "https");

    size_t hostStart = schemeEnd + 3;
    size_t pathStart = url.find('/', hostStart);

    std::string hostPort;
    if (pathStart == std::string::npos) {
        hostPort = url.substr(hostStart);
        out.path = "/";
    } else {
        hostPort = url.substr(hostStart, pathStart - hostStart);
        out.path = url.substr(pathStart);
    }

    if (hostPort.empty()) {
        return out;
    }

    out.base = scheme + "://" + hostPort;
    out.valid = true;
    return out;
}

// 将 HTTP 状态码/网络错误映射到 HttpResponse.errorMessage
std::string describeHttplibError(httplib::Error err) {
    switch (err) {
    case httplib::Error::Success:            return "";
    case httplib::Error::Connection:         return "connection failed";
    case httplib::Error::BindIPAddress:      return "bind ip address failed";
    case httplib::Error::Read:               return "read error";
    case httplib::Error::Write:              return "write error";
    case httplib::Error::ExceedRedirectCount:return "too many redirects";
    case httplib::Error::Canceled:           return "request canceled";
    case httplib::Error::SSLConnection:      return "SSL/TLS connection failed";
    case httplib::Error::SSLLoadingCerts:    return "SSL certificate loading failed";
    case httplib::Error::SSLServerVerification: return "SSL server verification failed";
    case httplib::Error::UnsupportedMultipartBoundaryChars: return "unsupported multipart boundary";
    case httplib::Error::Compression:        return "compression error";
    case httplib::Error::ConnectionTimeout:  return "connection timeout";
    default:                                 return "unknown network error";
    }
}

} // namespace

// ---------------------------------------------------------------------------
// HttpClient implementation
// ---------------------------------------------------------------------------

void HttpClient::setConnectionTimeout(int seconds) {
    std::lock_guard<std::mutex> lock(configMutex_);
    connectionTimeout_ = seconds > 0 ? seconds : 1;
}

void HttpClient::setResponseTimeout(int seconds) {
    std::lock_guard<std::mutex> lock(configMutex_);
    responseTimeout_ = seconds > 0 ? seconds : 1;
}

void HttpClient::setProxy(const ProxyConfig& proxy) {
    std::lock_guard<std::mutex> lock(configMutex_);
    proxy_ = proxy;
}

int HttpClient::getConnectionTimeout() const {
    std::lock_guard<std::mutex> lock(configMutex_);
    return connectionTimeout_;
}

int HttpClient::getResponseTimeout() const {
    std::lock_guard<std::mutex> lock(configMutex_);
    return responseTimeout_;
}

ProxyConfig HttpClient::getProxy() const {
    std::lock_guard<std::mutex> lock(configMutex_);
    return proxy_;
}

HttpClient::~HttpClient() {
    (void)shutdown();
}

bool HttpClient::shutdown() {
    if (currentHttpClientWorker == this) {
        return false;
    }

    std::vector<std::function<void()>> stopCallbacks;
    {
        std::lock_guard<std::mutex> lock(activeMutex_);
        shuttingDown_ = true;
        stopCallbacks.reserve(activeRequests_.size());
        for (auto& entry : activeRequests_) {
            ActiveRequest& request = *entry.second;
            if (request.cancelToken) {
                request.cancelToken->store(true);
            }
            if (!request.completed && request.stopTransport) {
                stopCallbacks.push_back(request.stopTransport);
            }
        }
    }

    for (auto& stop : stopCallbacks) {
        try {
            stop();
        } catch (...) {
            // Transport interruption is best effort; joining below is the
            // ownership guarantee and remains mandatory.
        }
    }

    std::lock_guard<std::mutex> joinLock(workerJoinMutex_);
    std::vector<std::thread> workers;
    {
        std::lock_guard<std::mutex> lock(activeMutex_);
        workers.reserve(activeRequests_.size());
        for (auto& entry : activeRequests_) {
            if (entry.second->worker.joinable()) {
                workers.push_back(std::move(entry.second->worker));
            }
        }
        activeRequests_.clear();
    }

    for (std::thread& worker : workers) {
        worker.join();
    }
    return true;
}

void HttpClient::markRequestCompleted(uint64_t requestId) {
    std::function<void()> releaseTransport;
    {
        std::lock_guard<std::mutex> lock(activeMutex_);
        auto it = activeRequests_.find(requestId);
        if (it == activeRequests_.end()) {
            return;
        }
        releaseTransport = std::move(it->second->stopTransport);
        it->second->completed = true;
    }
    // Destroy the captured httplib::Client outside activeMutex_.
    releaseTransport = {};
}

void HttpClient::reapCompletedWorkers() {
    std::lock_guard<std::mutex> joinLock(workerJoinMutex_);
    std::vector<std::thread> completed;
    {
        std::lock_guard<std::mutex> lock(activeMutex_);
        for (auto it = activeRequests_.begin(); it != activeRequests_.end();) {
            if (!it->second->completed) {
                ++it;
                continue;
            }
            if (it->second->worker.joinable()) {
                completed.push_back(std::move(it->second->worker));
            }
            it = activeRequests_.erase(it);
        }
    }

    for (std::thread& worker : completed) {
        worker.join();
    }
}

uint64_t HttpClient::postAsync(const std::string& url,
                               const std::map<std::string, std::string>& headers,
                               const std::string& body,
                               SSECallback onSSE,
                               HttpCompletionCallback onComplete,
                               CancellationToken cancelToken) {
    {
        std::lock_guard<std::mutex> lock(activeMutex_);
        if (shuttingDown_) {
            return 0;
        }
    }
    reapCompletedWorkers();

    const uint64_t requestId = nextRequestId_.fetch_add(1);
    if (!cancelToken) {
        cancelToken = std::make_shared<std::atomic<bool>>(false);
    }

    // 捕获当前配置快照，避免后台线程读取时与 setter 竞争
    int connTimeout;
    int readTimeout;
    ProxyConfig proxySnap;
    {
        std::lock_guard<std::mutex> lock(configMutex_);
        connTimeout = connectionTimeout_;
        readTimeout = responseTimeout_;
        proxySnap = proxy_;
    }

    auto workerTask = [this,
                       requestId,
                       url,
                       headers,
                       body,
                       onSSE = std::move(onSSE),
                       onComplete = std::move(onComplete),
                       cancelToken,
                       connTimeout,
                       readTimeout,
                       proxySnap]() mutable {
        HttpWorkerThreadMarker workerMarker(this);
        auto completionGuard = makeScopeExit(
            [this, requestId] { markRequestCompleted(requestId); });
        HttpResponse response;

        auto completeSafely = [&](HttpResponse resp) {
            if (!onComplete) {
                return;
            }
            try {
                onComplete(std::move(resp));
            } catch (const std::exception&) {
                // Completion callbacks run on this worker thread. Letting an
                // exception escape would terminate the process.
            } catch (...) {
                // See above.
            }
        };

        try {
        ParsedUrl parsed = parseUrl(url);
        if (!parsed.valid) {
            response.statusCode = 0;
            response.errorMessage = "invalid URL: " + url;
            completeSafely(std::move(response));
            return;
        }

        // cpp-httplib 的 Client 构造接受形如 "https://host[:port]" 的 URL
        // 当启用 CPPHTTPLIB_OPENSSL_SUPPORT 时会自动处理 HTTPS
        auto client = std::make_shared<httplib::Client>(parsed.base);

        {
            std::lock_guard<std::mutex> lock(activeMutex_);
            auto it = activeRequests_.find(requestId);
            if (it != activeRequests_.end()) {
                it->second->stopTransport = [client] { client->stop(); };
            }
        }

        if (cancelToken->load()) {
            response.cancelled = true;
            response.errorMessage = "request cancelled";
            completeSafely(std::move(response));
            return;
        }

        client->set_connection_timeout(connTimeout, 0);
        client->set_read_timeout(readTimeout, 0);
        client->set_write_timeout(readTimeout, 0);
        client->set_keep_alive(false);
        client->set_follow_location(true);

        // TLS：交由 cpp-httplib/OpenSSL 使用系统证书存储进行校验
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
        client->enable_server_certificate_verification(true);
#endif

        if (proxySnap.enabled && !proxySnap.host.empty() && proxySnap.port > 0) {
            client->set_proxy(proxySnap.host.c_str(), proxySnap.port);
        }

        httplib::Headers httplibHeaders;
        bool hasContentType = false;
        for (const auto& kv : headers) {
            httplibHeaders.emplace(kv.first, kv.second);
            std::string key = kv.first;
            std::transform(key.begin(), key.end(), key.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (key == "content-type") {
                hasContentType = true;
            }
        }
        std::string contentType = "application/json";
        if (hasContentType) {
            for (const auto& kv : headers) {
                std::string key = kv.first;
                std::transform(key.begin(), key.end(), key.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (key == "content-type") {
                    contentType = kv.second;
                    break;
                }
            }
        }

        // SSE 解析器，仅在回调存在时用于分发事件
        SSEParser parser(onSSE);
        std::string accumulated;
        bool aborted = false;

        // cpp-httplib 没有直接暴露 "POST + response ContentReceiver" 的便捷重载，
        // 所以我们走低层 Request/send 路径：构造一个 Request、设置
        // content_receiver 回调以便在数据到达时流式处理，然后调用 client.send()。
        // 注意 Request.content_receiver 的签名是 ContentReceiverWithProgress，
        // 接受 (data, len, offset, total) 四参数，我们只关心前两个。
        httplib::Request req;
        req.method = "POST";
        req.path = parsed.path;
        req.headers = httplibHeaders;
        req.body = body;
        req.set_header("Content-Type", contentType);

        req.content_receiver = [&](const char* data, size_t dataLen,
                                   uint64_t /*offset*/, uint64_t /*total*/) -> bool {
            if (cancelToken && cancelToken->load()) {
                aborted = true;
                return false; // 通知 cpp-httplib 终止连接
            }
            accumulated.append(data, dataLen);
            if (onSSE) {
                parser.feed(data, dataLen);
                if (parser.callbackFailed()) {
                    return false;
                }
            }
            return true;
        };

        httplib::Response res;
        httplib::Error err = httplib::Error::Success;
        bool sendOk = client->send(req, res, err);

        // 流结束：刷出挂起的 SSE 事件
        if (onSSE) {
            parser.finish();
        }

        if (parser.callbackFailed()) {
            response.statusCode = 0;
            response.errorMessage = "SSE callback failed";
            if (!parser.callbackError().empty()) {
                response.errorMessage += ": " + parser.callbackError();
            }
            completeSafely(std::move(response));
            return;
        }

        if ((cancelToken && cancelToken->load()) || aborted) {
            response.cancelled = true;
            response.errorMessage = "request cancelled";
            completeSafely(std::move(response));
            return;
        }

        if (!sendOk) {
            response.statusCode = 0;
            response.errorMessage = describeHttplibError(err);
            if (err == httplib::Error::ConnectionTimeout ||
                err == httplib::Error::Read ||
                err == httplib::Error::Write) {
                response.timedOut = true;
            }
            completeSafely(std::move(response));
            return;
        }

        response.statusCode = res.status;
        // res.body 在使用 content_receiver 时通常为空；以 accumulated 为准
        response.body = !accumulated.empty() ? std::move(accumulated) : res.body;

        if (response.statusCode < 200 || response.statusCode >= 300) {
            std::ostringstream oss;
            oss << "HTTP " << response.statusCode;
            response.errorMessage = oss.str();
        }

        completeSafely(std::move(response));
        } catch (const std::exception& error) {
            response.statusCode = 0;
            response.errorMessage = std::string("HTTP worker failed: ") +
                                    error.what();
            completeSafely(std::move(response));
        } catch (...) {
            response.statusCode = 0;
            response.errorMessage = "HTTP worker failed: unknown error";
            completeSafely(std::move(response));
        }
    };

    {
        std::lock_guard<std::mutex> lock(activeMutex_);
        if (shuttingDown_) {
            return 0;
        }
        auto request = std::make_unique<ActiveRequest>();
        request->cancelToken = cancelToken;
        ActiveRequest* requestPtr = request.get();
        activeRequests_.emplace(requestId, std::move(request));
        try {
            requestPtr->worker = std::thread(std::move(workerTask));
        } catch (...) {
            activeRequests_.erase(requestId);
            return 0;
        }
    }
    return requestId;
}

void HttpClient::cancelRequest(uint64_t requestId) {
    std::function<void()> stopTransport;
    {
        std::lock_guard<std::mutex> lock(activeMutex_);
        auto it = activeRequests_.find(requestId);
        if (it == activeRequests_.end() || it->second->completed) {
            return;
        }
        if (it->second->cancelToken) {
            it->second->cancelToken->store(true);
        }
        stopTransport = it->second->stopTransport;
    }
    if (stopTransport) {
        try {
            stopTransport();
        } catch (...) {
            // The worker still owns and will join the transport lifecycle.
        }
    }
}

} // namespace AI

#endif // HAVE_AI_CHAT
