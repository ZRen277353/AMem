#include "client_singleton.h"
#include "SocketCommand.h"
#include <cstring>

bool FreezeAdd(uint64_t address, uint8_t dataSize, const uint8_t data[8],
               PortType port) {
    return SocketCommand::execute(port, [&](WindowsSocketClient* client, int handle) -> bool {
        unsigned char command = CMD_FREEZE_ADD;
        if (!SocketCommand::sendCommandWithHandle(client, command, handle))
            return false;
        CeFreezeAddInput input{};
        input.address = address;
        input.dataSize = dataSize;
        memcpy(input.data, data, 8);
        if (!client->Send(&input, sizeof(input)))
            return false;
        int result = 0;
        if (!client->Receive(&result, sizeof(result)))
            return false;
        return result != 0;
    });
}

bool FreezeRemove(uint64_t address, PortType port) {
    return SocketCommand::execute(port, [&](WindowsSocketClient* client, int handle) -> bool {
        unsigned char command = CMD_FREEZE_REMOVE;
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

bool FreezeClear(PortType port) {
    return SocketCommand::execute(port, [&](WindowsSocketClient* client, int handle) -> bool {
        unsigned char command = CMD_FREEZE_CLEAR;
        if (!SocketCommand::sendCommandWithHandle(client, command, handle))
            return false;
        int result = 0;
        if (!client->Receive(&result, sizeof(result)))
            return false;
        return result != 0;
    });
}

bool FreezePause(PortType port) {
    return SocketCommand::execute(port, [&](WindowsSocketClient* client, int handle) -> bool {
        unsigned char command = CMD_FREEZE_PAUSE;
        if (!SocketCommand::sendCommandWithHandle(client, command, handle))
            return false;
        int result = 0;
        if (!client->Receive(&result, sizeof(result)))
            return false;
        return result != 0;
    });
}

bool FreezeResume(PortType port) {
    return SocketCommand::execute(port, [&](WindowsSocketClient* client, int handle) -> bool {
        unsigned char command = CMD_FREEZE_RESUME;
        if (!SocketCommand::sendCommandWithHandle(client, command, handle))
            return false;
        int result = 0;
        if (!client->Receive(&result, sizeof(result)))
            return false;
        return result != 0;
    });
}

bool FreezeGetList(std::vector<CeFreezeItem> &outList, bool &isPaused,
                   uint32_t &interval_ms, PortType port) {
    return SocketCommand::execute(port, [&](WindowsSocketClient* client, int handle) -> bool {
        unsigned char command = CMD_FREEZE_GETLIST;
        if (!SocketCommand::sendCommandWithHandle(client, command, handle))
            return false;
        CeFreezeListOutput output{};
        if (!client->Receive(&output, sizeof(output)))
            return false;
        isPaused = output.isPaused != 0;
        interval_ms = output.interval_ms;
        outList.clear();
        if (output.count > 0) {
            outList.resize(output.count);
            if (!client->Receive(outList.data(), output.count * sizeof(CeFreezeItem)))
                return false;
        }
        return true;
    });
}

bool FreezeUpdate(uint64_t address, const uint8_t data[8], PortType port) {
    return SocketCommand::execute(port, [&](WindowsSocketClient* client, int handle) -> bool {
        unsigned char command = CMD_FREEZE_UPDATE;
        if (!SocketCommand::sendCommandWithHandle(client, command, handle))
            return false;
        CeFreezeUpdateInput input{};
        input.address = address;
        memcpy(input.data, data, 8);
        if (!client->Send(&input, sizeof(input)))
            return false;
        int result = 0;
        if (!client->Receive(&result, sizeof(result)))
            return false;
        return result != 0;
    });
}

bool FreezeSetInterval(uint32_t interval_ms, PortType port) {
    return SocketCommand::execute(port, [&](WindowsSocketClient* client, int handle) -> bool {
        unsigned char command = CMD_FREEZE_SETINTERVAL;
        if (!SocketCommand::sendCommandWithHandle(client, command, handle))
            return false;
        if (!client->Send(&interval_ms, sizeof(interval_ms)))
            return false;
        int result = 0;
        if (!client->Receive(&result, sizeof(result)))
            return false;
        return result != 0;
    });
}
