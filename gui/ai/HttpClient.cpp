#ifdef HAVE_AI_CHAT

#include "HttpClient.h"

// cpp-httplib is header-only and provides HTTPS support when compiled with
// CPPHTTPLIB_OPENSSL_SUPPORT. The CMake task gates this include behind
// HAVE_AI_CHAT so the header is only required when AI chat is enabled.
#include "httplib.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <sstream>
#include <thread>

namespace AI {

namespace {

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

// SSE 解析器：按行累积，遇到空行时把累积的 data 拼接后派发
class SSEParser {
public:
    explicit SSEParser(SSECallback cb) : cb_(std::move(cb)) {}

    // 追加原始字节；识别 \n 边界；\r 被忽略以兼容 \r\n
    void feed(const char* data, size_t len) {
        for (size_t i = 0; i < len; ++i) {
            char c = data[i];
            if (c == '\r') {
                continue;
            }
            if (c == '\n') {
                processLine(currentLine_);
                currentLine_.clear();
            } else {
                currentLine_.push_back(c);
            }
        }
    }

    // 流结束时，刷出剩余数据
    void finish() {
        if (!currentLine_.empty()) {
            processLine(currentLine_);
            currentLine_.clear();
        }
        flushEvent();
    }

private:
    void processLine(const std::string& line) {
        if (line.empty()) {
            // 空行 → 事件结束
            flushEvent();
            return;
        }
        // SSE 注释行以 ':' 开头，忽略
        if (line[0] == ':') {
            return;
        }
        // 仅处理 data: 字段
        if (line.compare(0, 5, "data:") == 0) {
            // 剥离 "data:" 和紧跟的一个可选空格
            size_t start = 5;
            if (start < line.size() && line[start] == ' ') {
                ++start;
            }
            if (!eventBuffer_.empty()) {
                eventBuffer_.push_back('\n');
            }
            eventBuffer_.append(line, start, std::string::npos);
        }
        // 其他字段（event:, id:, retry:）当前无需处理
    }

    void flushEvent() {
        if (eventBuffer_.empty()) {
            return;
        }
        // 过滤 OpenAI 风格的 [DONE] 终止事件
        if (eventBuffer_ == "[DONE]") {
            eventBuffer_.clear();
            return;
        }
        if (cb_) {
            cb_(eventBuffer_);
        }
        eventBuffer_.clear();
    }

    SSECallback cb_;
    std::string currentLine_;
    std::string eventBuffer_;
};

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

uint64_t HttpClient::postAsync(const std::string& url,
                               const std::map<std::string, std::string>& headers,
                               const std::string& body,
                               SSECallback onSSE,
                               HttpCompletionCallback onComplete,
                               CancellationToken cancelToken) {
    const uint64_t requestId = nextRequestId_.fetch_add(1);
    if (!cancelToken) {
        cancelToken = std::make_shared<std::atomic<bool>>(false);
    }

    {
        std::lock_guard<std::mutex> lock(activeMutex_);
        activeRequests_[requestId] = cancelToken;
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

    std::thread worker([this,
                        requestId,
                        url,
                        headers,
                        body,
                        onSSE = std::move(onSSE),
                        onComplete = std::move(onComplete),
                        cancelToken = std::move(cancelToken),
                        connTimeout,
                        readTimeout,
                        proxySnap]() mutable {
        HttpResponse response;

        auto removeFromActive = [this, requestId]() {
            std::lock_guard<std::mutex> lock(activeMutex_);
            activeRequests_.erase(requestId);
        };

        ParsedUrl parsed = parseUrl(url);
        if (!parsed.valid) {
            response.statusCode = 0;
            response.errorMessage = "invalid URL: " + url;
            removeFromActive();
            if (onComplete) {
                onComplete(std::move(response));
            }
            return;
        }

        // cpp-httplib 的 Client 构造接受形如 "https://host[:port]" 的 URL
        // 当启用 CPPHTTPLIB_OPENSSL_SUPPORT 时会自动处理 HTTPS
        httplib::Client client(parsed.base);

        client.set_connection_timeout(connTimeout, 0);
        client.set_read_timeout(readTimeout, 0);
        client.set_write_timeout(readTimeout, 0);
        client.set_keep_alive(false);
        client.set_follow_location(true);

        // TLS：交由 cpp-httplib/OpenSSL 使用系统证书存储进行校验
        client.enable_server_certificate_verification(true);

        if (proxySnap.enabled && !proxySnap.host.empty() && proxySnap.port > 0) {
            client.set_proxy(proxySnap.host.c_str(), proxySnap.port);
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
            }
            return true;
        };

        httplib::Response res;
        httplib::Error err = httplib::Error::Success;
        bool sendOk = client.send(req, res, err);

        // 流结束：刷出挂起的 SSE 事件
        if (onSSE) {
            parser.finish();
        }

        if ((cancelToken && cancelToken->load()) || aborted) {
            response.cancelled = true;
            response.errorMessage = "request cancelled";
            removeFromActive();
            if (onComplete) {
                onComplete(std::move(response));
            }
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
            removeFromActive();
            if (onComplete) {
                onComplete(std::move(response));
            }
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

        removeFromActive();
        if (onComplete) {
            onComplete(std::move(response));
        }
    });

    worker.detach();
    return requestId;
}

void HttpClient::cancelRequest(uint64_t requestId) {
    std::lock_guard<std::mutex> lock(activeMutex_);
    auto it = activeRequests_.find(requestId);
    if (it != activeRequests_.end() && it->second) {
        it->second->store(true);
    }
}

} // namespace AI

#endif // HAVE_AI_CHAT
