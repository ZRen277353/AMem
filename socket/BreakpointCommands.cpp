#include "client_singleton.h"
#include "SocketCommand.h"
#include <cstring>

namespace {
constexpr int kMaxBreakpointHitCount = 100000;
} // namespace

bool SetKernelBreakpoint(uint64_t address, uint32_t bpType, uint32_t bpSize, PortType port) {
    return SocketCommand::execute(port, [&](WindowsSocketClient* client, int handle) -> bool {
        unsigned char command = CMD_KERNEL_SETBREAKPOINT;
        if (!SocketCommand::sendCommandWithHandle(client, command, handle))
            return false;
        // 合并三次 Send 为一次连续字节发送
        unsigned char buf[sizeof(address) + sizeof(bpType) + sizeof(bpSize)];
        memcpy(buf, &address, sizeof(address));
        memcpy(buf + sizeof(address), &bpType, sizeof(bpType));
        memcpy(buf + sizeof(address) + sizeof(bpType), &bpSize, sizeof(bpSize));
        if (!client->Send(buf, sizeof(buf)))
            return false;
        int result = 0;
        if (!client->Receive(&result, sizeof(result)))
            return false;
        return result != 0;
    });
}

bool RemoveKernelBreakpoint(uint64_t address, PortType port) {
    return SocketCommand::execute(port, [&](WindowsSocketClient* client, int handle) -> bool {
        unsigned char command = CMD_KERNEL_REMOVEBREAKPOINT;
        if (!SocketCommand::sendCommandWithHandle(client, command, handle))
            return false;
        if (!client->Send(&address, sizeof(address)))
            return false;
        int result = 0;
        if (!client->Receive(&result, sizeof(result)))
            return false;
        return result != 0;
    });
}

bool SuspendKernelBreakpoint(uint64_t address, PortType port) {
    return SocketCommand::execute(port, [&](WindowsSocketClient* client, int handle) -> bool {
        unsigned char command = CMD_KERNEL_SUSPENDBREAKPOINT;
        if (!SocketCommand::sendCommandWithHandle(client, command, handle))
            return false;
        if (!client->Send(&address, sizeof(address)))
            return false;
        int result = 0;
        if (!client->Receive(&result, sizeof(result)))
            return false;
        return result != 0;
    });
}

bool ResumeKernelBreakpoint(uint64_t address, PortType port) {
    return SocketCommand::execute(port, [&](WindowsSocketClient* client, int handle) -> bool {
        unsigned char command = CMD_KERNEL_RESUMEBREAKPOINT;
        if (!SocketCommand::sendCommandWithHandle(client, command, handle))
            return false;
        if (!client->Send(&address, sizeof(address)))
            return false;
        int result = 0;
        if (!client->Receive(&result, sizeof(result)))
            return false;
        return result != 0;
    });
}

bool ReadKernelBreakpointInfo(uint64_t address, std::vector<HW_HIT_INFO> &infos, PortType port) {
    return SocketCommand::execute(port, [&](WindowsSocketClient* client, int handle) -> bool {
        unsigned char command = CMD_KERNEL_READHWBPINFO;
        if (!SocketCommand::sendCommandWithHandle(client, command, handle))
            return false;
        if (!client->Send(&address, sizeof(address)))
            return false;
        int result = 0;
        uint64_t TotalCount = 0;
        if (!client->Receive(&result, sizeof(result)))
            return false;
        if (!client->Receive(&TotalCount, sizeof(TotalCount)))
            return false;
        if (result < 0 || result > kMaxBreakpointHitCount)
            return false;
        if (result > 0) {
            infos.resize(static_cast<size_t>(result));
            if (!client->Receive(infos.data(), static_cast<size_t>(result) * sizeof(HW_HIT_INFO)))
                return false;
        }
        return true;
    });
}
