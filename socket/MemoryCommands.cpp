#include "client_singleton.h"
#include "SocketCommand.h"

bool ReadProcessMemoryBytes(uint64_t address, uint32_t size,
                            std::vector<unsigned char> &out, PortType port) {
    return SocketCommand::execute(port, [&](WindowsSocketClient* client, int handle) -> bool {
#pragma pack(1)
        struct { unsigned char command; CeReadProcessMemoryInput input; } op;
#pragma pack()
        op.command = CMD_READPROCESSMEMORY;
        op.input.handle = handle;
        op.input.address = address;
        op.input.size = size;
        op.input.compress = 0;
        if (!client->Send(&op, sizeof(op)))
            return false;
        CeReadProcessMemoryOutput outHdr{};
        if (!client->Receive(&outHdr, sizeof(outHdr)))
            return false;
        out.resize(size);
        client->Receive(out.data(), out.size());
        if (outHdr.read <= 0) {
            out.clear();
            return false;
        }
        return true;
    });
}

bool WriteProcessMemoryBytes(uint64_t address, uint32_t size,
                             std::vector<unsigned char> &data, PortType port) {
    return SocketCommand::execute(port, [&](WindowsSocketClient* client, int handle) -> bool {
#pragma pack(1)
        struct { unsigned char command; CeWriteProcessMemoryInput input; } op;
#pragma pack()
        op.command = CMD_WRITEPROCESSMEMORY;
        op.input.handle = handle;
        op.input.address = address;
        op.input.size = size;
        if (!client->Send(&op, sizeof(op)))
            return false;
        client->Send(data.data(), data.size());
        CeWriteProcessMemoryOutput output;
        if (!client->Receive(&output, sizeof(output)))
            return false;
        return output.written == size;
    });
}

bool ReadProcessMemory_(uint64_t address, uint32_t size, void *out,
                        int32_t &Realread, PortType port) {
    return SocketCommand::execute(port, [&](WindowsSocketClient* client, int handle) -> bool {
#pragma pack(1)
        struct { unsigned char command; CeReadProcessMemoryInput input; } op;
#pragma pack()
        op.command = CMD_READPROCESSMEMORY;
        op.input.handle = handle;
        op.input.address = address;
        op.input.size = size;
        op.input.compress = 0;
        if (!client->Send(&op, sizeof(op)))
            return false;
        client->Receive(&Realread, sizeof(Realread));
        client->Receive(out, size);
        if (Realread <= 0)
            return false;
        return true;
    });
}

bool ReadBratchMemory(uint64_t address, uint32_t size,
                      std::vector<std::pair<uint64_t, std::vector<uint8_t>>> &out,
                      PortType port) {
    return SocketCommand::execute(port, [&](WindowsSocketClient* client, int handle) -> bool {
#pragma pack(1)
        struct { unsigned char command; int handle; uint64_t address; uint32_t size; } op;
#pragma pack()
        op.command = CMD_READBRATCHMEMORY;
        op.handle = handle;
        op.address = address;
        op.size = size;
        if (!client->Send(&op, sizeof(op)))
            return false;
        int len = 0;
        if (!client->Receive(&len, sizeof(len)))
            return false;
        if (len <= 0) {
            out.clear();
            return true;
        }
        out.resize(len);
        for (int i = 0; i < len; i++) {
            uint64_t addr = 0;
            std::vector<unsigned char> data(4096);
            if (!client->Receive(&addr, sizeof(addr)))
                return false;
            if (!client->Receive(data.data(), 4096))
                return false;
            out[i] = {addr, data};
        }
        return true;
    });
}

bool ReadBratchAddr(std::vector<std::pair<uint64_t, int32_t>> &addrs,
                    std::vector<std::pair<uint64_t, std::vector<uint8_t>>> &out,
                    PortType port) {
    return SocketCommand::execute(port, [&](WindowsSocketClient* client, int handle) -> bool {
        unsigned char command = CMD_READBRATCHADDR;
        if (!SocketCommand::sendCommandWithHandle(client, command, handle))
            return false;
        int len = addrs.size();
        if (!client->Send(&len, sizeof(len)))
            return false;
        std::vector<CeReadBratchAddr> input(len);
        for (int i = 0; i < len; i++) {
            input[i].addr = addrs[i].first;
            input[i].size = addrs[i].second;
        }
        if (!client->Send(input.data(), len * sizeof(CeReadBratchAddr)))
            return false;
        out.clear();
        out.resize(len);
        int result = 0;
        if (!client->Receive(&result, sizeof(result)))
            return false;
        for (int i = 0; i < len; i++) {
            uint64_t addr = 0;
            int32_t sz = input[i].size;
            std::vector<unsigned char> data(sz);
            if (!client->Receive(&addr, sizeof(addr)))
                return false;
            if (!client->Receive(data.data(), sz))
                return false;
            out[i] = {addr, data};
        }
        return true;
    });
}
