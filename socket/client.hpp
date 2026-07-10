#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include <functional>
#include <utility>

#include <cstring>
#include <cstdint>
#include <windows.h>

#include "socket_io_timeout.h"

// Windows Socket库链接
#pragma comment(lib, "ws2_32.lib")

// 定义命令常量（从ceserver.h复制）
#define CMD_GETVERSION 0
#define CMD_CLOSECONNECTION 1
#define CMD_TERMINATESERVER 2
#define CMD_OPENPROCESS 3
#define CMD_CLOSEHANDLE 7

#define CMD_GETARCHITECTURE 21
#define CMD_GETMEMTYPE 100


#define CMD_INITRWDRIVER 251



#define CMD_GETPROCESSLIST 247
#define CMD_GETMODULELIST 246
#define CMD_READPROCESSMEMORY 9
#define CMD_WRITEPROCESSMEMORY 10
#define CMD_SETRANGE 248
#define CMD_SCANVALUE 250
#define CMD_SCANNEXTVALUE 249
// 新增命令：获取搜索结果
#define CMD_GETSCANRESULT 245
#define CMD_GETSCANRESULT_COUNT 244
#define CMD_READBRATCHMEMORY 243
#define CMD_REMOVESCANRESULT 242
#define CMD_CLEARSCANRESULT 241
#define CMD_ADDSCANRESULT 240

#define CMD_SCANFUZZYVALUE     239
#define CMD_SCANNEXTFUZZYVALUE 238
#define CMD_SCANGROUPVALUE     237
#define CMD_SCANHEX 236
#define CMD_GETSCAN_TYPE_RESULT 235


//断点相关
#define CMD_KERNEL_SETBREAKPOINT 234
#define CMD_KERNEL_REMOVEBREAKPOINT 233
#define CMD_KERNEL_SUSPENDBREAKPOINT 232
#define CMD_KERNEL_RESUMEBREAKPOINT 231
#define CMD_KERNEL_READHWBPINFO 230

//扫基质
#define CMD_SCANPOINTER 229

#define CMD_READBRATCHADDR 228

#define CMD_SHELLEXEC 227

#define CMD_STOPPROCESS 226 //用于其他线程发送停止命令

//冻结相关命令
#define CMD_FREEZE_ADD 225
#define CMD_FREEZE_REMOVE 224
#define CMD_FREEZE_CLEAR 223
#define CMD_FREEZE_PAUSE 222
#define CMD_FREEZE_RESUME 221
#define CMD_FREEZE_GETLIST 220
#define CMD_FREEZE_UPDATE 219
#define CMD_FREEZE_SETINTERVAL 218

// ELF 符号相关命令
#define CMD_SYMBOL_INIT 217
#define CMD_SYMBOL_GETLIST 216
#define CMD_SYMBOL_FIND 215


#pragma pack(1)
struct CeVersion {
    int version;
    unsigned char stringsize;
};

struct CeModuleListEntry {
    int result;
    int flag;
    uint64_t modulebase;
    int modulesize;
    int modulenamesize;
};

struct CeReadProcessMemoryInput {
    uint32_t handle;
    uint64_t address;
    uint32_t size;
    uint8_t compress;
};

struct CeWriteProcessMemoryInput {
	uint32_t handle;
	uint64_t address;
	uint32_t size;
};

struct CeWriteProcessMemoryOutput {
	int32_t written;
};

struct CeReadProcessMemoryOutput {
    int read;
};

struct CeGetScanResultInput {
    int offset;
    int count;
};

struct CeGetScanResultOutput {
    int total_count;
    int actual_count;
};

struct ScanProgress {
    int msgType;
    float percent;
    uint64_t totalBytes;
    uint64_t scannedBytes;
    uint64_t matchCount;
};

struct _user_pt_regs {
    uint64_t regs[31];
    uint64_t sp;
    uint64_t pc;
    uint64_t pstate;
    uint64_t orig_x0;
    uint64_t syscallno;
};

struct _user_fpsimd_state {
	// unsigned char vregs[32][16];
#ifdef _MSC_VER
	unsigned char vregs[32][16];
#else
	__uint128_t	vregs[32];
#endif
	uint32_t fpsr;
	uint32_t fpcr;
};

struct HW_HIT_INFO {
    
    uint64_t hit_addr;
    uint64_t hit_time;
    struct _user_pt_regs regs_info;
    struct _user_fpsimd_state fpsimd_info;
};



struct CeReadBratchMemoryOutput{
		//int result;//发出去的都是有效页面，所以result可以不用
		uint64_t addr;
		std::vector<unsigned char> data;
};

struct CeReadBratchAddr{
	uint64_t addr;
	uint32_t size;
};

// 冻结项结构体
struct CeFreezeItem {
	uint64_t address;
	uint8_t dataSize;
	uint8_t data[8];
};

struct CeFreezeAddInput {
	uint64_t address;
	uint8_t dataSize;
	uint8_t data[8];
};

struct CeFreezeUpdateInput {
	uint64_t address;
	uint8_t data[8];
};

struct CeFreezeListOutput {
	int count;
	uint8_t isPaused;
	uint32_t interval_ms;
};

struct CeSymbolInitInput {
    uint32_t hProcess;
    uint64_t moduleBase;
};

struct CeSymbolInitOutput {
    int result;
    int totalCount;
};

struct CeGetSymbolListInput {
    int offset;
    int count;
};

struct CeGetSymbolListOutput {
    int totalCount;
    int actualCount;
};

struct CeSymbolEntry {
    uint64_t address;
    int nameSize;
};

struct CeFindSymbolInput {
    uint32_t hProcess;
    uint64_t moduleBase;
    int nameSize;
};

struct CeFindSymbolOutput {
    int result;
    uint64_t address;
};

#pragma pack()

class WindowsSocketClient {
private:
    SOCKET sock_;
    bool connected_;
    WSADATA wsaData_;
    std::function<void()> poisonCallback_;

    void NotifyPoisoned() {
        if (poisonCallback_) {
            poisonCallback_();
        }
    }

public:
    WindowsSocketClient() : sock_(INVALID_SOCKET), connected_(false) {
        // 初始化Winsock
        int result = WSAStartup(MAKEWORD(2, 2), &wsaData_);
        if (result != 0) {
            std::cerr << "WSAStartup failed: " << result << std::endl;
        }
    }

    ~WindowsSocketClient() {
        Close();
        WSACleanup();
    }

    bool Connect(const std::string& host, uint16_t port) {
        if (connected_) {
            Close();
        }

        sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock_ == INVALID_SOCKET) {
            std::cerr << "socket() failed: " << WSAGetLastError() << std::endl;
            return false;
        }

        sockaddr_in serverAddr{};
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(port);

        if (inet_pton(AF_INET, host.c_str(), &serverAddr.sin_addr) != 1) {
            std::cerr << "Invalid address: " << host << std::endl;
            closesocket(sock_);
            sock_ = INVALID_SOCKET;
            return false;
        }

        if (connect(sock_, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
            std::cerr << "connect() failed: " << WSAGetLastError() << std::endl;
            closesocket(sock_);
            sock_ = INVALID_SOCKET;
            return false;
        }

        connected_ = true;
        std::cout << "Connected to " << host << ":" << port << std::endl;
        return true;
    }

    bool Send(const void* data, size_t size) {
        if (!connected_ || sock_ == INVALID_SOCKET) return false;

        SocketIoTimeout::SocketOptionTimeoutGuard timeoutGuard(sock_, SO_SNDTIMEO);
        const char* buffer = static_cast<const char*>(data);
        size_t totalSent = 0;

        while (totalSent < size) {
            int sent = ::send(sock_, buffer + totalSent, static_cast<int>(size - totalSent), 0);
            if (sent == SOCKET_ERROR) {
                int err = WSAGetLastError();
                std::cerr << "send() failed: " << err << std::endl;
                // Any failed I/O may leave this unframed stream out of sync.
                timeoutGuard.dismissRestore();
                NotifyPoisoned();
                Close();
                return false;
            }
            if (sent <= 0) {
                timeoutGuard.dismissRestore();
                NotifyPoisoned();
                Close();
                return false;
            }
            totalSent += sent;
        }
        return true;
    }

    bool Receive(void* buffer, size_t size) {
        if (!connected_ || sock_ == INVALID_SOCKET) return false;

        SocketIoTimeout::SocketOptionTimeoutGuard timeoutGuard(sock_, SO_RCVTIMEO);
        char* buf = static_cast<char*>(buffer);
        size_t totalReceived = 0;

        while (totalReceived < size) {
            int received = ::recv(sock_, buf + totalReceived, static_cast<int>(size - totalReceived), 0);
            if (received == SOCKET_ERROR) {
                int err = WSAGetLastError();
                std::cerr << "recv() failed: " << err << std::endl;
                timeoutGuard.dismissRestore();
                NotifyPoisoned();
                Close();
                return false;
            }
            if (received == 0) {
                std::cerr << "Connection closed by server" << std::endl;
                timeoutGuard.dismissRestore();
                NotifyPoisoned();
                Close();
                return false;
            }
            totalReceived += received;
        }
        return true;
    }

    void Close() {
        if (sock_ != INVALID_SOCKET) {
            closesocket(sock_);
            sock_ = INVALID_SOCKET;
        }
        connected_ = false;
    }

    bool IsConnected() const { return connected_; }

    void SetPoisonCallback(std::function<void()> callback) {
        poisonCallback_ = std::move(callback);
    }
};


static void CloseServer(WindowsSocketClient& client) {
    std::cout << "\n=== Closing Server ===" << std::endl;
    unsigned char command = CMD_TERMINATESERVER;
    client.Send(&command, sizeof(command));
}
