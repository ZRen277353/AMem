#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include <unordered_map>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

/**
 * Legacy loopback HTTP server for migration diagnostics.
 * It is unauthenticated, bypasses Agent approval, and is excluded from
 * default builds. Do not add new product capabilities here.
 */
class IpcServer {
public:
    using Handler = std::function<json(const json& params)>;

    static IpcServer& GetInstance();

    // 启动/停止
    bool Start(uint16_t port = 28100);
    void Stop();
    bool IsRunning() const { return running_.load(); }
    uint16_t GetPort() const { return port_; }

    // 注册路由
    void RegisterMethod(const std::string& method, Handler handler);

private:
    IpcServer() = default;
    ~IpcServer() { Stop(); }
    IpcServer(const IpcServer&) = delete;
    IpcServer& operator=(const IpcServer&) = delete;

    void ServerThread();
    void HandleClient(uintptr_t clientSocket);
    std::string BuildHttpResponse(int statusCode, const std::string& body);
    json DispatchRequest(const json& request);

    // 注册所有内置路由
    void RegisterBuiltinMethods();

    std::atomic<bool> running_{false};
    uint16_t port_ = 28100;
    uintptr_t listenSocket_ = ~(uintptr_t)0; // INVALID_SOCKET
    std::thread serverThread_;
    std::unordered_map<std::string, Handler> handlers_;
};
