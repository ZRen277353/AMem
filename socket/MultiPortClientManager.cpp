#include "MultiPortClientManager.h"

#include <utility>

MultiPortClientManager::MultiPortClientManager(
    DeviceSession& session,
    IWindowsSocketOps& mainOps,
    IWindowsSocketOps& debugOps,
    IWindowsSocketOps& errorOps,
    ResetStateCallback resetState)
    : session_(session),
      mainClient_(mainOps),
      debugClient_(debugOps),
      errorClient_(errorOps),
      resetState_(std::move(resetState)) {
    auto poison = [this] { session_.MarkPoisoned(); };
    mainClient_.SetPoisonCallback(poison);
    debugClient_.SetPoisonCallback(poison);
    errorClient_.SetPoisonCallback(std::move(poison));
}

MultiPortClientManager::~MultiPortClientManager() {
    auto lifecycle = session_.AcquireLifecycle();
    session_.Disconnect();
    CloseClients();
}

MultiPortConnectFailure MultiPortClientManager::Connect(
    const std::string& host, uint16_t port) {
    auto lifecycle = session_.AcquireLifecycle();
    session_.BeginConnect();
    CloseClients();
    if (resetState_) {
        resetState_();
    }

    if (!mainClient_.Connect(host, port)) {
        CloseClients();
        session_.FinishConnect(false);
        return MultiPortConnectFailure::Main;
    }
    if (!debugClient_.Connect(host, port)) {
        CloseClients();
        session_.FinishConnect(false);
        return MultiPortConnectFailure::Debug;
    }
    if (!errorClient_.Connect(host, port)) {
        CloseClients();
        session_.FinishConnect(false);
        return MultiPortConnectFailure::Error;
    }

    session_.FinishConnect(true);
    return MultiPortConnectFailure::None;
}

bool MultiPortClientManager::Disconnect() {
    auto lifecycle = session_.AcquireLifecycle();
    const bool hadSession =
        session_.GetState() != DeviceSession::State::Disconnected;
    session_.Disconnect();
    if (resetState_) {
        resetState_();
    }
    CloseClients();
    return hadSession;
}

bool MultiPortClientManager::IsConnected() const {
    return session_.IsConnected();
}

WindowsSocketClient* MultiPortClientManager::GetClient(
    ManagedSocketPort port) {
    return const_cast<WindowsSocketClient*>(
        static_cast<const MultiPortClientManager*>(this)->GetClient(port));
}

const WindowsSocketClient* MultiPortClientManager::GetClient(
    ManagedSocketPort port) const {
    switch (port) {
    case ManagedSocketPort::Main:
        return &mainClient_;
    case ManagedSocketPort::Debug:
        return &debugClient_;
    case ManagedSocketPort::Error:
        return &errorClient_;
    }
    return nullptr;
}

void MultiPortClientManager::CloseClients() {
    mainClient_.Close();
    debugClient_.Close();
    errorClient_.Close();
}
