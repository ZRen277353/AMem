#include "client_singleton.h"
#include "SocketCommand.h"
#include "../gui/MemoryTypes.h"
#include <iostream>
#include <iomanip>

namespace {
constexpr int kMaxScanResultCount = 5000000;
constexpr int kMaxScanResultPageCount = 1000;
constexpr size_t kMaxScanResultRemovalCount = 100000;
constexpr size_t kMaxScanValueBytes = 4096;
constexpr size_t kMaxGroupScanItems = 1024;

bool isValidScanPageRequest(int offset, int count) {
    return offset >= 0 && count >= 0 && count <= kMaxScanResultPageCount;
}

bool isValidScanResultCount(int count) {
    return count >= 0 && count <= kMaxScanResultCount;
}

bool isValidScanResultHeader(int offset, int requestedCount,
                             const CeGetScanResultOutput& output) {
    if (!isValidScanResultCount(output.total_count) ||
        output.actual_count < 0 ||
        output.actual_count > requestedCount ||
        output.actual_count > output.total_count) {
        return false;
    }

    if (offset >= output.total_count) {
        return output.actual_count == 0;
    }

    return output.actual_count <= output.total_count - offset;
}

bool isValidScanRange(uint64_t start, uint64_t end) {
    return start <= end;
}

bool isValidScanBytes(const std::vector<unsigned char>& value) {
    return !value.empty() && value.size() <= kMaxScanValueBytes;
}

bool scanNextFlagRequiresValue(uint32_t flag) {
    return (flag & (_ADD_UNKNOW_VAL | _SUB_UNKNOW_VAL |
                    _CHANGED_VAL | _UNCHANGED_VAL)) == 0;
}

bool isValidScanNextBytes(const std::vector<unsigned char>& value, uint32_t flag) {
    if (!scanNextFlagRequiresValue(flag)) {
        return value.size() <= kMaxScanValueBytes;
    }
    return isValidScanBytes(value);
}

bool isValidGroupScanValues(
    const std::vector<std::pair<std::vector<unsigned char>, std::pair<char, char>>> &values) {
    if (values.empty() || values.size() > kMaxGroupScanItems)
        return false;

    for (const auto &entry : values) {
        const int declaredSize = static_cast<int>(entry.second.first);
        if (declaredSize <= 0 ||
            static_cast<size_t>(declaredSize) > kMaxScanValueBytes ||
            entry.first.size() != static_cast<size_t>(declaredSize)) {
            return false;
        }
    }
    return true;
}

bool receiveScanResultCount(WindowsSocketClient* client, int& count) {
    if (!client->Receive(&count, sizeof(count)))
        return false;
    return isValidScanResultCount(count);
}
} // namespace

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
    if (!isValidScanRange(start, end) || !isValidScanBytes(Value))
        return -1;

    return SocketCommand::executeWithResult(port, [&](WindowsSocketClient* client, int handle, int& len) -> bool {
        unsigned char command = CMD_SCANVALUE;
        if (!SocketCommand::sendCommandWithHandle(client, command, handle))
            return false;
#pragma pack(1)
        struct { uint64_t start; uint64_t end; int size; unsigned int flag; } scanParams;
#pragma pack()
        scanParams.start = start; scanParams.end = end;
        scanParams.size = static_cast<int>(Value.size()); scanParams.flag = flags;
        if (!client->Send(&scanParams, sizeof(scanParams)))
            return false;
        if (scanParams.size > 0 && !client->Send(Value.data(), Value.size()))
            return false;
        while (true) {
            ScanProgress progress;
            if (!client->Receive(&progress, sizeof(progress)))
                return false;
            if (progress.msgType == 1) {
                std::cout << "\r进度: " << std::fixed << std::setprecision(2)
                          << progress.percent << "%, 命中: " << progress.matchCount << std::flush;
            } else if (progress.msgType == 2) { break; }
            else if (progress.msgType == 3) { return false; }
            else { return false; }
        }
        return receiveScanResultCount(client, len);
    }, -1);
}

int ScanNextValue(std::vector<unsigned char> &Value, int flag, uint64_t start,
                  uint64_t end, PortType port) {
    if (flag < 0 || !isValidScanRange(start, end) ||
        !isValidScanNextBytes(Value, static_cast<uint32_t>(flag)))
        return -1;

    return SocketCommand::executeWithResult(port, [&](WindowsSocketClient* client, int handle, int& len) -> bool {
        unsigned char command = CMD_SCANNEXTVALUE;
        if (!SocketCommand::sendCommandWithHandle(client, command, handle))
            return false;
#pragma pack(1)
        struct { uint64_t start; uint64_t end; int size; unsigned int flag; } scanParams;
#pragma pack()
        scanParams.start = start; scanParams.end = end;
        scanParams.size = static_cast<int>(Value.size()); scanParams.flag = static_cast<unsigned int>(flag);
        if (!client->Send(&scanParams, sizeof(scanParams)))
            return false;
        if (scanParams.size > 0 && !client->Send(Value.data(), Value.size()))
            return false;
        while (true) {
            ScanProgress progress;
            if (!client->Receive(&progress, sizeof(progress)))
                return false;
            if (progress.msgType == 1) continue;
            if (progress.msgType == 2) break;
            else if (progress.msgType == 3) return false;
            else return false;
        }
        return receiveScanResultCount(client, len);
    }, -1);
}

int GetScanResultCount(PortType port) {
    return SocketCommand::executeWithResult(port, [&](WindowsSocketClient* client, int handle, int& count) -> bool {
        unsigned char command = CMD_GETSCANRESULT_COUNT;
        if (!SocketCommand::sendCommandWithHandle(client, command, handle))
            return false;
        return receiveScanResultCount(client, count);
    }, -1);
}

bool GetScanResult(int offset, int count,
                   std::vector<std::pair<uint64_t, uint64_t>> &results, PortType port) {
    if (!isValidScanPageRequest(offset, count))
        return false;

    return SocketCommand::execute(port, [&](WindowsSocketClient* client, int handle) -> bool {
        unsigned char command = CMD_GETSCANRESULT;
        if (!SocketCommand::sendCommandWithHandle(client, command, handle))
            return false;
        CeGetScanResultInput input;
        input.offset = offset; input.count = count;
        if (!client->Send(&input, sizeof(input)))
            return false;
        CeGetScanResultOutput output{};
        if (!client->Receive(&output, sizeof(output)))
            return false;
        if (!isValidScanResultHeader(offset, count, output))
            return false;
        results.clear();
        if (output.actual_count <= 0)
            return true;
        results.resize(static_cast<size_t>(output.actual_count));
        if (!client->Receive(results.data(), static_cast<size_t>(output.actual_count) * sizeof(uint64_t) * 2))
            return false;
        return true;
    });
}

bool RemoveScanResult(std::vector<uint64_t> address, PortType port) {
    if (address.empty() || address.size() > kMaxScanResultRemovalCount)
        return false;

    return SocketCommand::execute(port, [&](WindowsSocketClient* client, int handle) -> bool {
        unsigned char command = CMD_REMOVESCANRESULT;
        if (!SocketCommand::sendCommandWithHandle(client, command, handle))
            return false;
        int len = static_cast<int>(address.size());
        if (!client->Send(&len, sizeof(len)))
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
    if (!isValidScanRange(start, end) || !isValidScanBytes(Value))
        return -1;

    return SocketCommand::executeWithResult(port, [&](WindowsSocketClient* client, int handle, int& len) -> bool {
        unsigned char command = CMD_SCANVALUE;
        if (!SocketCommand::sendCommandWithHandle(client, command, handle))
            return false;
#pragma pack(1)
        struct { uint64_t start; uint64_t end; int size; unsigned int flag; } scanParams;
#pragma pack()
        scanParams.start = start; scanParams.end = end;
        scanParams.size = static_cast<int>(Value.size()); scanParams.flag = flags;
        if (!client->Send(&scanParams, sizeof(scanParams)))
            return false;
        if (scanParams.size > 0 && !client->Send(Value.data(), Value.size()))
            return false;
        if (!SocketCommand::receiveProgressLoop(client, callback, userData))
            return false;
        return receiveScanResultCount(client, len);
    }, -1);
}

int ScanNextValueWithProgress(std::vector<unsigned char> &Value, int flag,
                              ScanProgressCallback callback, void *userData,
                              uint64_t start, uint64_t end, PortType port) {
    if (flag < 0 || !isValidScanRange(start, end) ||
        !isValidScanNextBytes(Value, static_cast<uint32_t>(flag)))
        return -1;

    return SocketCommand::executeWithResult(port, [&](WindowsSocketClient* client, int handle, int& len) -> bool {
        unsigned char command = CMD_SCANNEXTVALUE;
        if (!SocketCommand::sendCommandWithHandle(client, command, handle))
            return false;
#pragma pack(1)
        struct { uint64_t start; uint64_t end; int size; unsigned int flag; } scanParams;
#pragma pack()
        scanParams.start = start; scanParams.end = end;
        scanParams.size = static_cast<int>(Value.size()); scanParams.flag = static_cast<unsigned int>(flag);
        if (!client->Send(&scanParams, sizeof(scanParams)))
            return false;
        if (scanParams.size > 0 && !client->Send(Value.data(), Value.size()))
            return false;
        if (!SocketCommand::receiveProgressLoop(client, callback, userData))
            return false;
        return receiveScanResultCount(client, len);
    }, -1);
}

int ScanFuzzyValueWithProgress(uint32_t flags, ScanProgressCallback callback,
                               void *userData, uint64_t start, uint64_t end,
                               PortType port) {
    if (!isValidScanRange(start, end))
        return -1;

    return SocketCommand::executeWithResult(port, [&](WindowsSocketClient* client, int handle, int& len) -> bool {
        unsigned char command = CMD_SCANFUZZYVALUE;
        if (!SocketCommand::sendCommandWithHandle(client, command, handle))
            return false;
#pragma pack(1)
        struct { uint64_t start; uint64_t end; unsigned int flag; } scanParams;
#pragma pack()
        scanParams.start = start; scanParams.end = end; scanParams.flag = flags;
        if (!client->Send(&scanParams, sizeof(scanParams)))
            return false;
        if (!SocketCommand::receiveProgressLoop(client, callback, userData))
            return false;
        return receiveScanResultCount(client, len);
    }, -1);
}

int ScanGroupValueWithProgress(
    std::vector<std::pair<std::vector<unsigned char>, std::pair<char, char>>> &Value,
    bool order, ScanProgressCallback callback, void *userData,
    uint64_t start, uint64_t end, PortType port) {
    if (!isValidScanRange(start, end) || !isValidGroupScanValues(Value))
        return -1;

    return SocketCommand::executeWithResult(port, [&](WindowsSocketClient* client, int handle, int& SearchCount) -> bool {
        unsigned char command = CMD_SCANGROUPVALUE;
        if (!SocketCommand::sendCommandWithHandle(client, command, handle))
            return false;
#pragma pack(1)
        struct { uint64_t start; uint64_t end; char order; int len; } scanParams;
#pragma pack()
        int len = static_cast<int>(Value.size());
        scanParams.start = start; scanParams.end = end;
        scanParams.order = order; scanParams.len = len;
        if (!client->Send(&scanParams, sizeof(scanParams)))
            return false;
        std::vector<char> types, sizes;
        for (auto &it : Value) { types.push_back(it.second.second); sizes.push_back(it.second.first); }
        if (!client->Send(types.data(), types.size()))
            return false;
        if (!client->Send(sizes.data(), sizes.size()))
            return false;
        for (int i = 0; i < len; i++) {
            if (!client->Send(Value[i].first.data(), sizes[i]))
                return false;
        }
        if (!SocketCommand::receiveProgressLoop(client, callback, userData))
            return false;
        return receiveScanResultCount(client, SearchCount);
    }, -1);
}

int ScanHEXValueWithProgress(uint64_t start, uint64_t end,
                             std::vector<unsigned char> &Value,
                             ScanProgressCallback callback, void *userData,
                             PortType port) {
    if (!isValidScanRange(start, end) || !isValidScanBytes(Value))
        return -1;

    return SocketCommand::executeWithResult(port, [&](WindowsSocketClient* client, int handle, int& len) -> bool {
        unsigned char command = CMD_SCANHEX;
        if (!SocketCommand::sendCommandWithHandle(client, command, handle))
            return false;
#pragma pack(1)
        struct { uint64_t start; uint64_t end; int size; } scanParams;
#pragma pack()
        scanParams.start = start; scanParams.end = end; scanParams.size = static_cast<int>(Value.size());
        if (!client->Send(&scanParams, sizeof(scanParams)))
            return false;
        if (scanParams.size > 0 && !client->Send(Value.data(), Value.size()))
            return false;
        if (!SocketCommand::receiveProgressLoop(client, callback, userData))
            return false;
        return receiveScanResultCount(client, len);
    }, -1);
}

bool GetTypedScanResult(int offset, int count,
                        std::vector<std::tuple<uint64_t, uint64_t, short>> &results,
                        PortType port) {
    if (!isValidScanPageRequest(offset, count))
        return false;

    return SocketCommand::execute(port, [&](WindowsSocketClient* client, int handle) -> bool {
        unsigned char command = CMD_GETSCAN_TYPE_RESULT;
        if (!SocketCommand::sendCommandWithHandle(client, command, handle))
            return false;
        CeGetScanResultInput input;
        input.offset = offset; input.count = count;
        if (!client->Send(&input, sizeof(input)))
            return false;
        CeGetScanResultOutput output{};
        if (!client->Receive(&output, sizeof(output)))
            return false;
        if (!isValidScanResultHeader(offset, count, output))
            return false;
        results.clear();
        if (output.actual_count <= 0)
            return true;
        results.reserve(static_cast<size_t>(output.actual_count));
        for (int i = 0; i < output.actual_count; i++) {
            uint64_t addr = 0;
            uint64_t value = 0;
            short valueType = 0;
            if (!client->Receive(&addr, sizeof(addr)))
                return false;
            if (!client->Receive(&value, sizeof(value)))
                return false;
            if (!client->Receive(&valueType, sizeof(valueType)))
                return false;
            results.push_back(std::make_tuple(addr, value, valueType));
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
