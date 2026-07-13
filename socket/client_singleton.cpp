#include "client_singleton.h"
#include "client.hpp"
#include "SocketCommand.h"
#include "../gui/AppContext.h"
#include <iostream>
#include <string>

// ==================== WinSocketClientMgr 实现 ====================

WinSocketClientMgr::WinSocketClientMgr()
    : m_connection(DeviceSession::GetInstance(),
                   GetSystemWindowsSocketOps(),
                   GetSystemWindowsSocketOps(),
                   GetSystemWindowsSocketOps(),
                   [] {
                     ResetTrackedKernelBreakpoints();
                     AppContext::Get().clearProcessForDisconnect();
                   },
                   &AmemServerHandshake::Validate) {}

WinSocketClientMgr::~WinSocketClientMgr() = default;

WindowsSocketClient *WinSocketClientMgr::GetClient(PortType type) {
  switch (type) {
  case PORT_MAIN:
    return m_connection.GetClient(ManagedSocketPort::Main);
  case PORT_DEBUG:
    return m_connection.GetClient(ManagedSocketPort::Debug);
  case PORT_ERROR:
    return m_connection.GetClient(ManagedSocketPort::Error);
  default:         return nullptr;
  }
}

std::mutex *WinSocketClientMgr::GetMutex(PortType type) {
  switch (type) {
  case PORT_MAIN:  return &m_main_mutex;
  case PORT_DEBUG: return &m_debug_mutex;
  case PORT_ERROR: return &m_error_mutex;
  default:         return nullptr;
  }
}

std::recursive_timed_mutex *WinSocketClientMgr::GetTransactionMutex(
    PortType type) {
  switch (type) {
  case PORT_MAIN:  return &m_main_transaction_mutex;
  case PORT_DEBUG: return &m_debug_transaction_mutex;
  case PORT_ERROR: return &m_error_transaction_mutex;
  default:         return nullptr;
  }
}

bool WinSocketClientMgr::ConnectMultiPort(const std::string &host, uint16_t Port) {
  std::cout << "[MultiPort] Connecting to server..." << std::endl;
  std::cout << "  Main:  " << host << ":" << Port << std::endl;

  const MultiPortConnectFailure failure = m_connection.Connect(host, Port);
  if (failure == MultiPortConnectFailure::Main) {
    std::cerr << "[MultiPort] Failed to connect MAIN port" << std::endl;
    return false;
  }
  if (failure == MultiPortConnectFailure::Debug) {
    std::cerr << "[MultiPort] Failed to connect DEBUG port" << std::endl;
    return false;
  }
  if (failure == MultiPortConnectFailure::Error) {
    std::cerr << "[MultiPort] Failed to connect ERROR port" << std::endl;
    return false;
  }
  if (failure == MultiPortConnectFailure::Compatibility) {
    std::cerr << "[MultiPort] Server compatibility handshake failed"
              << std::endl;
    return false;
  }

  std::cout << "[MultiPort] All ports connected successfully!" << std::endl;
  return true;
}

void WinSocketClientMgr::DisconnectMultiPort() {
  if (m_connection.Disconnect())
    std::cout << "[MultiPort] All ports disconnected" << std::endl;
}

bool WinSocketClientMgr::IsMultiPortConnected() const {
  return m_connection.IsConnected();
}

// ==================== 进程管理 ====================

bool OpenProcessHandle(int pid, int &outHandle, PortType type) {
  outHandle = 0;
  return SocketCommand::executeNoHandle(
      type, [&](WindowsSocketClient* client) -> bool {
#pragma pack(1)
        struct { unsigned char command; int pid; } op;
#pragma pack()
        op.command = CMD_OPENPROCESS;
        op.pid = pid;
        if (!client->Send(&op, sizeof(op)))
          return false;
        int handle = 0;
        if (!client->Receive(&handle, sizeof(handle)))
          return false;
        if (handle == 0) {
          return false;
        }
        outHandle = handle;
        return true;
      });
}

bool EnsureOpenHandle(int &outHandle, PortType type) {
  (void)type;
  int handle = AppContext::Get().processHandle.load(std::memory_order_relaxed);
  if (handle) {
    outHandle = handle;
    return true;
  }
  return false;
}

bool CloseProcessHandle(int handle, PortType type) {
  if (handle == 0)
    return true;
  return SocketCommand::executeNoHandle(
      type, [&](WindowsSocketClient* client) -> bool {
        unsigned char command = CMD_CLOSEHANDLE;
        if (!client->Send(&command, sizeof(command)))
          return false;
        if (!client->Send(&handle, sizeof(handle)))
          return false;
        int result = 0;
        if (!client->Receive(&result, sizeof(result)))
          return false;
        return result != 0;
      });
}
