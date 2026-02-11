#include "client_singleton.h"
#include "SocketCommand.h"
#include <iostream>
#include <iomanip>

bool ScanSetRange(int type, PortType port) {
    return SocketCommand::execute(port, [&](WindowsSocketClient* client, int handle) -> bool {
        unsigned char command = CMD_SETRANGE;
        if (!SocketCommand::sendCommandWithHandle(client, command, handle))
            return false;
        if (!client->Send(&type, sizeof(type)))
            return false;
        return true;
    });
}

int ScanValue(uint32_t flags, std::vector<unsigned char> &Value, uint64_t start,
              uint64_t end, PortType port) {
    return SocketCommand::executeWithResult(port, [&](WindowsSocketClient* client, int handle, int& len) -> bool {
        unsigned char command = CMD_SCANVALUE;
        if (!SocketCommand::sendCommandWithHandle(client, command, handle))
            return false;
#pragma pack(1)
        struct { uint64_t start; uint64_t end; int size; unsigned int flag; } scanParams;
#pragma pack()
        scanParams.start = start; scanParams.end = end;
        scanParams.size = Value.size(); scanParams.flag = flags;
        client->Send(&scanParams, sizeof(scanParams));
        client->Send(Value.data(), Value.size());
        while (true) {
            ScanProgress progress;
            if (!client->Receive(&progress, sizeof(progress))) break;
            if (progress.msgType == 1) {
                std::cout << "\r进度: " << std::fixed << std::setprecision(2)
                          << progress.percent << "%, 命中: " << progress.matchCount << std::flush;
            } else if (progress.msgType == 2) { break; }
            else if (progress.msgType == 3) { return false; }
        }
        if (!client->Receive(&len, 4)) return false;
        return true;
    });
}

int ScanNextValue(std::vector<unsigned char> &Value, int flag, uint64_t start,
                  uint64_t end, PortType port) {
    return SocketCommand::executeWithResult(port, [&](WindowsSocketClient* client, int handle, int& len) -> bool {
        unsigned char command = CMD_SCANNEXTVALUE;
        if (!SocketCommand::sendCommandWithHandle(client, command, handle))
            return false;
#pragma pack(1)
        struct { uint64_t start; uint64_t end; int size; unsigned int flag; } scanParams;
#pragma pack()
        scanParams.start = start; scanParams.end = end;
        scanParams.size = Value.size(); scanParams.flag = flag;
        client->Send(&scanParams, sizeof(scanParams));
        client->Send(Value.data(), Value.size());
        while (true) {
            ScanProgress progress;
            if (!client->Receive(&progress, sizeof(progress))) break;
            if (progress.msgType == 2) break;
            else if (progress.msgType == 3) return false;
        }
        if (!client->Receive(&len, 4)) return false;
        return true;
    });
}

int GetScanResultCount(PortType port) {
    return SocketCommand::executeWithResult(port, [&](WindowsSocketClient* client, int handle, int& count) -> bool {
        unsigned char command = CMD_GETSCANRESULT_COUNT;
        if (!SocketCommand::sendCommandWithHandle(client, command, handle))
            return false;
        if (!client->Receive(&count, sizeof(count)))
            return false;
        return true;
    });
}

bool GetScanResult(int offset, int count,
                   std::vector<std::pair<uint64_t, uint64_t>> &results, PortType port) {
    return SocketCommand::execute(port, [&](WindowsSocketClient* client, int handle) -> bool {
        unsigned char command = CMD_GETSCANRESULT;
        if (!SocketCommand::sendCommandWithHandle(client, command, handle))
            return false;
        CeGetScanResultInput input;
        input.offset = offset; input.count = count;
        if (!client->Send(&input, sizeof(input)))
            return false;
        CeGetScanResultOutput output;
        if (!client->Receive(&output, sizeof(output)))
            return false;
        if (output.actual_count == 0)
            return false;
        results.resize(output.actual_count);
        if (!client->Receive(results.data(), output.actual_count * sizeof(uint64_t) * 2))
            return false;
        return true;
    });
}

bool RemoveScanResult(std::vector<uint64_t> address, PortType port) {
    return SocketCommand::execute(port, [&](WindowsSocketClient* client, int handle) -> bool {
        unsigned char command = CMD_REMOVESCANRESULT;
        if (!SocketCommand::sendCommandWithHandle(client, command, handle))
            return false;
        if (!client->Send(address.data(), address.size() * sizeof(uint64_t)))
            return false;
        return true;
    });
}

bool ClearScanResult(PortType port) {
    return SocketCommand::execute(port, [&](WindowsSocketClient* client, int handle) -> bool {
        unsigned char command = CMD_CLEARSCANRESULT;
        return SocketCommand::sendCommandWithHandle(client, command, handle);
    });
}

int ScanValueWithProgress(uint32_t flags, std::vector<unsigned char> &Value,
                          ScanProgressCallback callback, void *userData,
                          uint64_t start, uint64_t end, PortType port) {
    return SocketCommand::executeWithResult(port, [&](WindowsSocketClient* client, int handle, int& len) -> bool {
        unsigned char command = CMD_SCANVALUE;
        if (!SocketCommand::sendCommandWithHandle(client, command, handle))
            return false;
#pragma pack(1)
        struct { uint64_t start; uint64_t end; int size; unsigned int flag; } scanParams;
#pragma pack()
        scanParams.start = start; scanParams.end = end;
        scanParams.size = Value.size(); scanParams.flag = flags;
        client->Send(&scanParams, sizeof(scanParams));
        client->Send(Value.data(), Value.size());
        if (!SocketCommand::receiveProgressLoop(client, callback, userData))
            return false;
        if (!client->Receive(&len, sizeof(len)))
            return false;
        return true;
    });
}

int ScanNextValueWithProgress(std::vector<unsigned char> &Value, int flag,
                              ScanProgressCallback callback, void *userData,
                              uint64_t start, uint64_t end, PortType port) {
    return SocketCommand::executeWithResult(port, [&](WindowsSocketClient* client, int handle, int& len) -> bool {
        unsigned char command = CMD_SCANNEXTVALUE;
        if (!SocketCommand::sendCommandWithHandle(client, command, handle))
            return false;
#pragma pack(1)
        struct { uint64_t start; uint64_t end; int size; unsigned int flag; } scanParams;
#pragma pack()
        scanParams.start = start; scanParams.end = end;
        scanParams.size = Value.size(); scanParams.flag = flag;
        client->Send(&scanParams, sizeof(scanParams));
        client->Send(Value.data(), Value.size());
        if (!SocketCommand::receiveProgressLoop(client, callback, userData))
            return false;
        if (!client->Receive(&len, sizeof(len)))
            return false;
        return true;
    });
}

int ScanFuzzyValueWithProgress(uint32_t flags, ScanProgressCallback callback,
                               void *userData, uint64_t start, uint64_t end,
                               PortType port) {
    return SocketCommand::executeWithResult(port, [&](WindowsSocketClient* client, int handle, int& len) -> bool {
        unsigned char command = CMD_SCANFUZZYVALUE;
        if (!SocketCommand::sendCommandWithHandle(client, command, handle))
            return false;
#pragma pack(1)
        struct { uint64_t start; uint64_t end; unsigned int flag; } scanParams;
#pragma pack()
        scanParams.start = start; scanParams.end = end; scanParams.flag = flags;
        client->Send(&scanParams, sizeof(scanParams));
        if (!SocketCommand::receiveProgressLoop(client, callback, userData))
            return false;
        if (!client->Receive(&len, sizeof(len)))
            return false;
        return true;
    });
}

int ScanGroupValueWithProgress(
    std::vector<std::pair<std::vector<unsigned char>, std::pair<char, char>>> &Value,
    bool order, ScanProgressCallback callback, void *userData,
    uint64_t start, uint64_t end, PortType port) {
    return SocketCommand::executeWithResult(port, [&](WindowsSocketClient* client, int handle, int& SearchCount) -> bool {
        unsigned char command = CMD_SCANGROUPVALUE;
        if (!SocketCommand::sendCommandWithHandle(client, command, handle))
            return false;
#pragma pack(1)
        struct { uint64_t start; uint64_t end; char order; int len; } scanParams;
#pragma pack()
        int len = Value.size();
        scanParams.start = start; scanParams.end = end;
        scanParams.order = order; scanParams.len = len;
        client->Send(&scanParams, sizeof(scanParams));
        std::vector<char> types, sizes;
        for (auto &it : Value) { types.push_back(it.second.second); sizes.push_back(it.second.first); }
        client->Send(types.data(), types.size());
        client->Send(sizes.data(), sizes.size());
        for (int i = 0; i < len; i++)
            client->Send(Value[i].first.data(), sizes[i]);
        if (!SocketCommand::receiveProgressLoop(client, callback, userData))
            return false;
        if (!client->Receive(&SearchCount, sizeof(SearchCount)))
            return false;
        return true;
    });
}

int ScanHEXValueWithProgress(uint64_t start, uint64_t end,
                             std::vector<unsigned char> &Value,
                             ScanProgressCallback callback, void *userData,
                             PortType port) {
    return SocketCommand::executeWithResult(port, [&](WindowsSocketClient* client, int handle, int& len) -> bool {
        unsigned char command = CMD_SCANHEX;
        if (!SocketCommand::sendCommandWithHandle(client, command, handle))
            return false;
#pragma pack(1)
        struct { uint64_t start; uint64_t end; int size; } scanParams;
#pragma pack()
        scanParams.start = start; scanParams.end = end; scanParams.size = Value.size();
        client->Send(&scanParams, sizeof(scanParams));
        client->Send(Value.data(), Value.size());
        if (!SocketCommand::receiveProgressLoop(client, callback, userData))
            return false;
        if (!client->Receive(&len, sizeof(len)))
            return false;
        return true;
    });
}

bool GetTypedScanResult(int offset, int count,
                        std::vector<std::tuple<uint64_t, uint64_t, short>> &results,
                        PortType port) {
    return SocketCommand::execute(port, [&](WindowsSocketClient* client, int handle) -> bool {
        unsigned char command = CMD_GETSCAN_TYPE_RESULT;
        if (!SocketCommand::sendCommandWithHandle(client, command, handle))
            return false;
        CeGetScanResultInput input;
        input.offset = offset; input.count = count;
        if (!client->Send(&input, sizeof(input)))
            return false;
        CeGetScanResultOutput output;
        if (!client->Receive(&output, sizeof(output)))
            return false;
        if (output.actual_count == 0)
            return false;
        for (int i = 0; i < output.actual_count; i++) {
            uint64_t addr, value; short type;
            client->Receive(&addr, sizeof(addr));
            client->Receive(&value, sizeof(value));
            client->Receive(&type, sizeof(type));
            results.push_back(std::make_tuple(addr, value, type));
        }
        return true;
    });
}

bool StopSearchScan(PortType port) {
    return SocketCommand::execute(port, [&](WindowsSocketClient* client, int handle) -> bool {
        unsigned char command = CMD_STOPPROCESS;
        if (!SocketCommand::sendCommandWithHandle(client, command, handle))
            return false;
        int result = 0;
        if (!client->Receive(&result, sizeof(result)))
            return false;
        return result != 0;
    });
}
