#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "DeviceSession.h"
#include "client.hpp"


// 端口类型枚举（与服务端保持一致）
enum PortType {
  PORT_MAIN = 1,  // 主通信端口
  PORT_ERROR = 2, // 错误通知端口
  PORT_DEBUG = 3, // 调试端口
};

/**
 * Windows Socket客户端管理器
 * 管理三个端口的连接：主端口、调试端口、错误端口
 */
class WinSocketClientMgr {
private:
  // 三个端口的客户端
  WindowsSocketClient m_main_client;
  WindowsSocketClient m_debug_client;
  WindowsSocketClient m_error_client;

  // 每个端口的互斥锁
  std::mutex m_main_mutex;
  std::mutex m_debug_mutex;
  std::mutex m_error_mutex;

  // Higher-level operations hold these gates across multiple commands.
  // Recursion lets the owning thread reuse the normal command helpers.
  std::recursive_timed_mutex m_main_transaction_mutex;
  std::recursive_timed_mutex m_debug_transaction_mutex;
  std::recursive_timed_mutex m_error_transaction_mutex;

  // 禁止拷贝和赋值
  WinSocketClientMgr(const WinSocketClientMgr &) = delete;
  WinSocketClientMgr &operator=(const WinSocketClientMgr &) = delete;

  // 私有构造函数（单例模式）
  WinSocketClientMgr();
  ~WinSocketClientMgr();

  void CloseClients();

public:
  // 获取单例实例
  static WinSocketClientMgr &GetInstance() {
    static WinSocketClientMgr instance;
    return instance;
  }

  // 获取指定端口的客户端
  WindowsSocketClient *GetClient(PortType type);

  // 获取对应端口的锁
  std::mutex *GetMutex(PortType type);

  std::recursive_timed_mutex *GetTransactionMutex(PortType type);

  // 连接到服务器的所有端口
  bool ConnectMultiPort(const std::string &host, uint16_t Port);

  // 断开所有端口
  void DisconnectMultiPort();

  // 检查多端口是否已连接
  bool IsMultiPortConnected() const;

  DeviceSession::RequestLease AcquireRequestLease() {
    return DeviceSession::GetInstance().AcquireRequest();
  }

  // 当前连接身份。每次成功连接或断开活动连接时递增。
  uint64_t GetConnectionGeneration() const {
    return DeviceSession::GetInstance().GetGeneration();
  }

  bool IsConnectionPoisoned() const {
    return DeviceSession::GetInstance().IsPoisoned();
  }

};

// ==================== 全局便捷接口（向后兼容） ====================
// 获取单例管理器
inline WinSocketClientMgr &GetSocketMgr() {
  return WinSocketClientMgr::GetInstance();
}

// 便捷函数（直接调用WinSocketClientMgr的方法）
inline bool ConnectMultiPort(const std::string &host, uint16_t Port) {
  return GetSocketMgr().ConnectMultiPort(host, Port);
}

inline void DisconnectMultiPort() { GetSocketMgr().DisconnectMultiPort(); }

inline bool IsMultiPortConnected() {
  return GetSocketMgr().IsMultiPortConnected();
}

inline uint64_t GetConnectionGeneration() {
  return GetSocketMgr().GetConnectionGeneration();
}

inline bool IsConnectionPoisoned() {
  return GetSocketMgr().IsConnectionPoisoned();
}

struct ServerVersionInfo {
  int version = 0;
  std::string versionString;
};

struct ProcessInfoItem {
  int pid = 0;
  std::string name;
};

struct ModuleInfoItem {
  uint64_t base = 0;
  int type; // 模块类型
  int flag; // 模块读写标志位
  int size = 0;
  std::string name;
};

enum MemType {
  MemType_Null = 0,
  MemType_IO = 1,
  MemType_Syscall = 2,
  MemType_Kernel = 3,
  MemType_SysHook = 4
};

//============ 命令全部默认主端口 ============//

bool FetchServerVersion(ServerVersionInfo &outInfo, PortType type = PORT_MAIN);
bool GetMemType(int &outType, PortType type = PORT_MAIN);
bool InitDriver(std::string &Card, std::string &resStr,
                PortType type = PORT_MAIN);

bool FetchProcessList(std::vector<ProcessInfoItem> &outList,
                      PortType type = PORT_MAIN);

// Process / Module helpers
void SetCurrentPid(int pid);
int GetCurrentPid();
bool OpenProcessHandle(int pid, int &outHandle, PortType type = PORT_MAIN);
bool EnsureOpenHandle(int &outHandle, PortType type = PORT_MAIN);
bool CloseProcessHandle(int handle, PortType type = PORT_MAIN);
bool FetchModuleList(std::vector<ModuleInfoItem> &outList,
                     PortType type = PORT_MAIN);

// Memory helpers
bool ReadProcessMemoryBytes(uint64_t address, uint32_t size,
                            std::vector<unsigned char> &out,
                            PortType type = PORT_MAIN);
bool ReadProcessMemory_(uint64_t address, uint32_t size, void *out,
                        int32_t &Realread, PortType type = PORT_MAIN);
bool ReadBratchMemory(
    uint64_t address, uint32_t size,
    std::vector<std::pair<uint64_t, std::vector<uint8_t>>> &out,
    PortType type = PORT_MAIN);

bool ReadBratchAddr(
    std::vector<std::pair<uint64_t, int32_t> /*addr,size*/> &addrs,
    std::vector<std::pair<uint64_t, std::vector<uint8_t>> /*addr,data*/> &out,
    PortType type = PORT_MAIN);

struct MemoryWriteIoResult {
  bool requestStarted = false;
  bool responseReceived = false;
  int32_t writtenBytes = 0;
};

MemoryWriteIoResult WriteProcessMemoryBytesTracked(
    uint64_t address, uint32_t size,
    const std::vector<unsigned char> &data,
    PortType type = PORT_MAIN);

bool WriteProcessMemoryBytes(uint64_t address, uint32_t size,
                             std::vector<unsigned char> &data,
                             PortType type = PORT_MAIN);

// Resolve helpers
bool GetModuleBaseByName(const std::string &moduleName, uint64_t &outBase,
                         PortType type = PORT_MAIN);
bool ResolveModuleOffsetChain(uint64_t &outAddress,
                              const std::string &moduleName,
                              uint64_t baseOffset,
                              const std::vector<uint64_t> &offsets,
                              bool derefFinal = true,
                              PortType type = PORT_MAIN);

// Scan helpers
bool ScanSetRange(int type, PortType port = PORT_MAIN);
int ScanValue(uint32_t flags, std::vector<unsigned char> &Value,
              uint64_t start = 0, uint64_t end = UINT64_MAX,
              PortType type = PORT_MAIN);
int ScanNextValue(std::vector<unsigned char> &Value, int flag,
                  uint64_t start = 0, uint64_t end = UINT64_MAX,
                  PortType type = PORT_MAIN);
int GetScanResultCount(PortType type = PORT_MAIN);
bool GetScanResult(int offset, int count,
                   std::vector<std::pair<uint64_t, uint64_t>> &results,
                   PortType type = PORT_MAIN);
bool RemoveScanResult(std::vector<uint64_t> address,
                      PortType type = PORT_MAIN);
bool ClearScanResult(PortType type = PORT_MAIN);

// 带进度回调的扫描函数
typedef void (*ScanProgressCallback)(float progress, uint64_t matchCount,
                                     uint64_t scannedBytes, uint64_t totalBytes,
                                     void *userData);
int ScanValueWithProgress(uint32_t flags, std::vector<unsigned char> &Value,
                          ScanProgressCallback callback, void *userData,
                          uint64_t start = 0, uint64_t end = UINT64_MAX,
                          PortType type = PORT_MAIN);
int ScanNextValueWithProgress(std::vector<unsigned char> &Value, int flag,
                              ScanProgressCallback callback, void *userData,
                              uint64_t start = 0, uint64_t end = UINT64_MAX,
                              PortType type = PORT_MAIN);
int ScanFuzzyValueWithProgress(uint32_t flags, ScanProgressCallback callback,
                               void *userData, uint64_t start = 0,
                               uint64_t end = UINT64_MAX,
                               PortType type = PORT_MAIN);
int ScanGroupValueWithProgress(
    std::vector<std::pair<std::vector<unsigned char>,
                          std::pair<char, char>> /*value,size,type*/> &Value,
    bool order /*是否按地址排序*/, ScanProgressCallback callback,
    void *userData, uint64_t start, uint64_t end, PortType type = PORT_MAIN);
int ScanHEXValueWithProgress(uint64_t start, uint64_t end,
                             std::vector<unsigned char> &Value,
                             ScanProgressCallback callback, void *userData,
                             PortType type = PORT_MAIN);
bool GetTypedScanResult(
    int offset, int count,
    std::vector<std::tuple<uint64_t, uint64_t, short>> &results,
    PortType type = PORT_MAIN);

// 内核断点相关
bool SetKernelBreakpoint(uint64_t address, uint32_t bpType, uint32_t bpSize,
                         PortType type = PORT_MAIN);
bool RemoveKernelBreakpoint(uint64_t address, PortType type = PORT_MAIN);
bool SuspendKernelBreakpoint(uint64_t address, PortType type = PORT_MAIN);
bool ResumeKernelBreakpoint(uint64_t address, PortType type = PORT_MAIN);
bool ReadKernelBreakpointInfo(uint64_t address, std::vector<HW_HIT_INFO> &infos,
                              PortType type = PORT_MAIN);
bool ClearTrackedKernelBreakpoints(PortType type = PORT_MAIN);

// 停止扫描
bool StopSearchScan(PortType type = PORT_DEBUG);

// 冻结功能
bool FreezeAdd(uint64_t address, uint8_t dataSize, const uint8_t data[8],
               PortType type = PORT_MAIN);
bool FreezeRemove(uint64_t address, PortType type = PORT_MAIN);
bool FreezeClear(PortType type = PORT_MAIN);
bool FreezePause(PortType type = PORT_MAIN);
bool FreezeResume(PortType type = PORT_MAIN);
bool FreezeGetList(std::vector<CeFreezeItem> &outList, bool &isPaused,
                   uint32_t &interval_ms, PortType type = PORT_MAIN);
bool FreezeUpdate(uint64_t address, const uint8_t data[8],
                  PortType type = PORT_MAIN);
bool FreezeSetInterval(uint32_t interval_ms, PortType type = PORT_MAIN);

// ELF 符号接口
bool SymbolInit(uint64_t moduleBase, int &outTotalCount,
                PortType type = PORT_MAIN);
bool SymbolGetList(int offset, int count,
                   std::vector<std::pair<uint64_t, std::string>> &outSymbols,
                   int *outTotalCount = nullptr,
                   PortType type = PORT_MAIN);
bool SymbolFind(uint64_t moduleBase, const std::string &name,
                uint64_t &outAddress, PortType type = PORT_MAIN);
