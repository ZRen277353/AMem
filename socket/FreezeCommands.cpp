#include "client_singleton.h"
#include "SocketCommand.h"
#include <cstring>

namespace {
constexpr int kMaxFreezeItemCount = 100000;
constexpr uint8_t kMaxFreezeDataSize = 8;

bool isValidCount(int value, int maxValue) {
    return value >= 0 && value <= maxValue;
}

bool isValidFreezeDataSize(uint8_t dataSize) {
    return dataSize > 0 && dataSize <= kMaxFreezeDataSize;
}
} // namespace

bool FreezeAdd(uint64_t address, uint8_t dataSize, const uint8_t data[8],
               PortType port) {
    if (!isValidFreezeDataSize(dataSize) || !data)
        return false;

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
    outList.clear();
    isPaused = false;
    interval_ms = 0;

    return SocketCommand::execute(port, [&](WindowsSocketClient* client, int handle) -> bool {
        unsigned char command = CMD_FREEZE_GETLIST;
        if (!SocketCommand::sendCommandWithHandle(client, command, handle))
            return false;
        CeFreezeListOutput output{};
        if (!client->Receive(&output, sizeof(output)))
            return false;
        if (!isValidCount(output.count, kMaxFreezeItemCount))
            return false;
        std::vector<CeFreezeItem> receivedItems;
        if (output.count > 0) {
            receivedItems.resize(static_cast<size_t>(output.count));
            if (!client->Receive(receivedItems.data(), static_cast<size_t>(output.count) * sizeof(CeFreezeItem)))
                return false;
            for (const auto &item : receivedItems) {
                if (!isValidFreezeDataSize(item.dataSize))
                    return false;
            }
        }
        outList = std::move(receivedItems);
        isPaused = output.isPaused != 0;
        interval_ms = output.interval_ms;
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
