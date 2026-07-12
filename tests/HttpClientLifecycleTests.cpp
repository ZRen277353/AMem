#include "gui/ai/HttpClient.h"

#include "httplib.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class BlockingHttpServer {
public:
    BlockingHttpServer() {
        server_.Post("/wait", [this](const httplib::Request&,
                                     httplib::Response& response) {
            response.status = 200;
            response.set_chunked_content_provider(
                "text/plain",
                [this](size_t offset, httplib::DataSink& sink) {
                    if (offset == 0) {
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            handlerEntered_ = true;
                        }
                        cv_.notify_all();
                    }

                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        if (releaseHandler_) {
                            sink.done();
                            return false;
                        }
                    }
                    std::this_thread::sleep_for(10ms);
                    return sink.write("tick", 4);
                });
        });

        port_ = server_.bind_to_any_port("127.0.0.1");
        if (port_ <= 0) {
            throw std::runtime_error("could not bind local HTTP test server");
        }
        thread_ = std::thread([this] { server_.listen_after_bind(); });
    }

    ~BlockingHttpServer() {
        releaseHandler();
        server_.stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    BlockingHttpServer(const BlockingHttpServer&) = delete;
    BlockingHttpServer& operator=(const BlockingHttpServer&) = delete;

    std::string url() const {
        return "http://127.0.0.1:" + std::to_string(port_) + "/wait";
    }

    bool waitForHandler(std::chrono::milliseconds timeout = 2s) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout,
                            [this] { return handlerEntered_; });
    }

    void releaseHandler() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            releaseHandler_ = true;
        }
        cv_.notify_all();
    }

private:
    httplib::Server server_;
    int port_ = 0;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool handlerEntered_ = false;
    bool releaseHandler_ = false;
};

class CompletionGate {
public:
    void enter(AI::HttpResponse response) {
        std::unique_lock<std::mutex> lock(mutex_);
        response_ = std::move(response);
        entered_ = true;
        cv_.notify_all();
        cv_.wait(lock, [this] { return released_; });
    }

    bool waitForEntry(std::chrono::milliseconds timeout = 2s) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout, [this] { return entered_; });
    }

    void release() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            released_ = true;
        }
        cv_.notify_all();
    }

    AI::HttpResponse response() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return response_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    AI::HttpResponse response_;
    bool entered_ = false;
    bool released_ = false;
};

void testCancelStopsStreamingTransport() {
    BlockingHttpServer server;
    AI::HttpClient& client = AI::HttpClient::getInstance();

    std::mutex mutex;
    std::condition_variable cv;
    bool completed = false;
    AI::HttpResponse response;

    const uint64_t requestId = client.postAsync(
        server.url(), {}, "{}", nullptr,
        [&](AI::HttpResponse result) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                response = std::move(result);
                completed = true;
            }
            cv.notify_all();
        },
        std::make_shared<std::atomic<bool>>(false));

    const bool dispatched = requestId != 0;
    const bool handlerEntered = dispatched && server.waitForHandler();
    if (dispatched) {
        client.cancelRequest(requestId);
    }

    bool interruptedBeforeServerRelease = false;
    {
        std::unique_lock<std::mutex> lock(mutex);
        interruptedBeforeServerRelease =
            cv.wait_for(lock, 2s, [&] { return completed; });
    }
    server.releaseHandler();
    bool callbackCompleted = interruptedBeforeServerRelease;
    if (!callbackCompleted) {
        std::unique_lock<std::mutex> lock(mutex);
        callbackCompleted = cv.wait_for(lock, 2s, [&] { return completed; });
    }

    expect(dispatched, "cancel test request was not dispatched");
    expect(handlerEntered, "local server did not receive cancel test request");
    expect(callbackCompleted,
           "cancel test completion callback did not run");
    expect(interruptedBeforeServerRelease,
           "cancelRequest did not stop the streaming transport");
    expect(response.cancelled,
           "transport interruption did not preserve cancellation semantics");
}

void testWorkerCannotJoinItself() {
    AI::HttpClient& client = AI::HttpClient::getInstance();
    std::mutex mutex;
    std::condition_variable cv;
    bool completed = false;
    bool shutdownResult = true;

    const uint64_t requestId = client.postAsync(
        "not-a-valid-url", {}, "{}", nullptr,
        [&](AI::HttpResponse) {
            shutdownResult = client.shutdown();
            {
                std::lock_guard<std::mutex> lock(mutex);
                completed = true;
            }
            cv.notify_all();
        },
        std::make_shared<std::atomic<bool>>(false));

    bool callbackCompleted = false;
    {
        std::unique_lock<std::mutex> lock(mutex);
        callbackCompleted = cv.wait_for(lock, 2s, [&] { return completed; });
    }

    expect(requestId != 0, "self-shutdown test request was not dispatched");
    expect(callbackCompleted, "self-shutdown callback did not complete");
    expect(!shutdownResult,
           "shutdown from an owned worker must report that it cannot self-join");
}

void testShutdownInterruptsAndJoinsCompletion() {
    BlockingHttpServer server;
    AI::HttpClient& client = AI::HttpClient::getInstance();
    CompletionGate callback;

    const uint64_t requestId = client.postAsync(
        server.url(), {}, "{}", nullptr,
        [&](AI::HttpResponse response) { callback.enter(std::move(response)); },
        std::make_shared<std::atomic<bool>>(false));
    const bool dispatched = requestId != 0;
    const bool handlerEntered = dispatched && server.waitForHandler();

    std::atomic<bool> shutdownReturned{false};
    bool callbackEntered = false;
    bool returnedBeforeCallbackRelease = false;
    bool shutdownResult = false;
    std::thread callbackObserver([&] {
        callbackEntered = callback.waitForEntry();
        if (callbackEntered) {
            std::this_thread::sleep_for(100ms);
            returnedBeforeCallbackRelease = shutdownReturned.load();
        }
        callback.release();
        server.releaseHandler();
    });

    shutdownResult = client.shutdown();
    shutdownReturned.store(true);
    callbackObserver.join();

    expect(dispatched, "shutdown test request was not dispatched");
    expect(handlerEntered, "local server did not receive shutdown test request");
    expect(callbackEntered,
           "shutdown did not interrupt the blocked transport and run completion");
    expect(!returnedBeforeCallbackRelease,
           "shutdown returned while its final completion callback was active");
    expect(shutdownResult && shutdownReturned.load(),
           "main-thread shutdown did not join all owned HTTP workers");
    expect(callback.response().cancelled,
           "shutdown transport interruption was not reported as cancellation");
}

void testPostRejectedAfterShutdown() {
    AI::HttpClient& client = AI::HttpClient::getInstance();
    const uint64_t requestId = client.postAsync(
        "http://127.0.0.1/", {}, "{}", nullptr, nullptr,
        std::make_shared<std::atomic<bool>>(false));
    expect(requestId == 0, "shutdown HttpClient accepted a new request");
    expect(client.shutdown(), "idempotent shutdown should remain fully drained");
}

} // namespace

int main() {
    AI::HttpClient::getInstance().setConnectionTimeout(2);
    AI::HttpClient::getInstance().setResponseTimeout(5);

    const std::vector<std::pair<const char*, void (*)()>> tests = {
        {"cancel stops streaming transport", &testCancelStopsStreamingTransport},
        {"worker self-shutdown rejection", &testWorkerCannotJoinItself},
        {"shutdown interrupts and joins callback",
         &testShutdownInterruptsAndJoinsCompletion},
        {"post rejected after shutdown", &testPostRejectedAfterShutdown},
    };

    size_t passed = 0;
    for (const auto& test : tests) {
        try {
            test.second();
            ++passed;
            std::cout << "[PASS] " << test.first << '\n';
        } catch (const std::exception& error) {
            std::cerr << "[FAIL] " << test.first << ": " << error.what()
                      << '\n';
            return 1;
        }
    }

    std::cout << passed << " HTTP lifecycle test groups passed\n";
    return 0;
}
