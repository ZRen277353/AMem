#pragma once

#include "client_singleton.h"
#include "client.hpp"
#include "socket_request_manager.h"
#include <functional>
#include <chrono>

namespace SocketCommand {

// 需要进程句柄的命令（大多数）
// fn(WindowsSocketClient* client, int handle) -> bool
template<typename Func>
bool execute(PortType port, Func&& fn) {
    auto* client = GetSocketMgr().GetClient(port);
    if (!client || !client->IsConnected())
        return false;
    int handle = 0;
    if (!EnsureOpenHandle(handle, port))
        return false;
    auto* portMutex = GetSocketMgr().GetMutex(port);
    return SocketRequestManager::GetInstance().ExecuteRequestWithLock(
        portMutex, [&]() -> bool {
            return fn(client, handle);
        });
}

// 不需要句柄的命令（GetVersion, GetProcessList, InitDriver）
// fn(WindowsSocketClient* client) -> bool
template<typename Func>
bool executeNoHandle(PortType port, Func&& fn) {
    auto* client = GetSocketMgr().GetClient(port);
    if (!client || !client->IsConnected())
        return false;
    auto* portMutex = GetSocketMgr().GetMutex(port);
    return SocketRequestManager::GetInstance().ExecuteRequestWithLock(
        portMutex, [&]() -> bool {
            return fn(client);
        });
}

// 需要句柄的命令，返回 int 结果（扫描类）
template<typename Func>
int executeWithResult(PortType port, Func&& fn) {
    auto* client = GetSocketMgr().GetClient(port);
    if (!client || !client->IsConnected())
        return 0;
    int handle = 0;
    if (!EnsureOpenHandle(handle, port))
        return 0;
    int result = 0;
    auto* portMutex = GetSocketMgr().GetMutex(port);
    bool success = SocketRequestManager::GetInstance().ExecuteRequestWithLock(
        portMutex, [&]() -> bool {
            return fn(client, handle, result);
        });
    return success ? result : 0;
}

// 发送命令字节+句柄的 helper
inline bool sendCommandWithHandle(WindowsSocketClient* client, uint8_t cmd, int handle) {
    if (!client->Send(&cmd, sizeof(cmd)))
        return false;
    if (!client->Send(&handle, sizeof(handle)))
        return false;
    return true;
}

// 扫描进度接收循环（带节流，避免高频回调导致 UI 卡顿）
inline bool receiveProgressLoop(WindowsSocketClient* client, ScanProgressCallback cb, void* userData) {
    auto lastCallTime = std::chrono::steady_clock::now();
    constexpr auto THROTTLE_INTERVAL = std::chrono::milliseconds(100);

    while (true) {
        ScanProgress progress;
        if (!client->Receive(&progress, sizeof(progress)))
            return false;

        bool isTerminal = (progress.msgType == 2 || progress.msgType == 3);
        if (cb) {
            auto now = std::chrono::steady_clock::now();
            // 终止消息或超过节流间隔时才回调
            if (isTerminal || (now - lastCallTime) >= THROTTLE_INTERVAL) {
                cb(progress.percent, progress.matchCount,
                   progress.scannedBytes, progress.totalBytes, userData);
                lastCallTime = now;
            }
        }

        if (progress.msgType == 2) // 扫描完成
            return true;
        if (progress.msgType == 3) // 扫描出错
            return false;
    }
}

} // namespace SocketCommand
