#include "client_singleton.h"
#include "SocketCommand.h"

bool SetKernelBreakpoint(uint64_t address, uint32_t bpType, uint32_t bpSize, PortType port) {
    return SocketCommand::execute(port, [&](WindowsSocketClient* client, int handle) -> bool {
        unsigned char command = CMD_KERNEL_SETBREAKPOINT;
        if (!SocketCommand::sendCommandWithHandle(client, command, handle))
            return false;
        if (!client->Send(&address, sizeof(address)))
            return false;
        if (!client->Send(&bpType, sizeof(bpType)))
            return false;
        if (!client->Send(&bpSize, sizeof(bpSize)))
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
        if (result > 0) {
            infos.resize(result);
            if (!client->Receive(infos.data(), result * sizeof(HW_HIT_INFO)))
                return false;
        }
        return true;
    });
}
