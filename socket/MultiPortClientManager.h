#pragma once

#include "DeviceSession.h"
#include "client.hpp"

#include <cstdint>
#include <functional>
#include <string>

enum class ManagedSocketPort {
    Main,
    Debug,
    Error,
};

enum class MultiPortConnectFailure {
    None,
    Main,
    Debug,
    Error,
    Compatibility,
};

class MultiPortClientManager {
public:
    using ResetStateCallback = std::function<void()>;
    using ConnectionValidator =
        std::function<bool(WindowsSocketClient& mainClient)>;

    MultiPortClientManager(DeviceSession& session,
                           IWindowsSocketOps& mainOps,
                           IWindowsSocketOps& debugOps,
                           IWindowsSocketOps& errorOps,
                           ResetStateCallback resetState,
                           ConnectionValidator validateConnection = {});
    ~MultiPortClientManager();

    MultiPortClientManager(const MultiPortClientManager&) = delete;
    MultiPortClientManager& operator=(const MultiPortClientManager&) = delete;

    MultiPortConnectFailure Connect(const std::string& host, uint16_t port);
    bool Disconnect();
    bool IsConnected() const;

    WindowsSocketClient* GetClient(ManagedSocketPort port);
    const WindowsSocketClient* GetClient(ManagedSocketPort port) const;

private:
    void CloseClients();

    DeviceSession& session_;
    WindowsSocketClient mainClient_;
    WindowsSocketClient debugClient_;
    WindowsSocketClient errorClient_;
    ResetStateCallback resetState_;
    ConnectionValidator validateConnection_;
};
