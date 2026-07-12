#include "client_singleton.h"
#include "SocketCommand.h"
#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

namespace {
constexpr int kMaxBreakpointHitCount = 100000;
constexpr size_t kBreakpointHitReadChunk = 64;

std::mutex g_trackedBreakpointMutex;
std::vector<uint64_t> g_trackedBreakpointAddresses;

bool isValidBreakpointType(uint32_t bpType) {
    return bpType >= 1 && bpType <= 4;
}

bool isValidBreakpointSize(uint32_t bpSize) {
    return bpSize == 1 || bpSize == 2 || bpSize == 4 || bpSize == 8;
}

void trackBreakpointAddress(uint64_t address) {
    std::lock_guard<std::mutex> lock(g_trackedBreakpointMutex);
    if (std::find(g_trackedBreakpointAddresses.begin(), g_trackedBreakpointAddresses.end(), address) ==
        g_trackedBreakpointAddresses.end()) {
        g_trackedBreakpointAddresses.push_back(address);
    }
}

void untrackBreakpointAddress(uint64_t address) {
    std::lock_guard<std::mutex> lock(g_trackedBreakpointMutex);
    g_trackedBreakpointAddresses.erase(
        std::remove(g_trackedBreakpointAddresses.begin(), g_trackedBreakpointAddresses.end(), address),
        g_trackedBreakpointAddresses.end());
}

std::vector<uint64_t> snapshotTrackedBreakpointAddresses() {
    std::lock_guard<std::mutex> lock(g_trackedBreakpointMutex);
    return g_trackedBreakpointAddresses;
}

void clearTrackedBreakpointAddresses() {
    std::lock_guard<std::mutex> lock(g_trackedBreakpointMutex);
    g_trackedBreakpointAddresses.clear();
}
} // namespace

BreakpointMutationIoResult SetKernelBreakpointTracked(
    uint64_t address, uint32_t bpType, uint32_t bpSize, PortType port) {
    BreakpointMutationIoResult result;
    if (!isValidBreakpointType(bpType) || !isValidBreakpointSize(bpSize))
        return result;

    (void)SocketCommand::execute(port, [&](WindowsSocketClient* client, int handle) -> bool {
        result.requestStarted = true;
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
        int applied = 0;
        if (!client->Receive(&applied, sizeof(applied)))
            return false;
        result.responseReceived = true;
        result.applied = applied != 0;
        if (result.applied) {
            trackBreakpointAddress(address);
        }
        return true;
    });
    return result;
}

BreakpointMutationIoResult RemoveKernelBreakpointTracked(
    uint64_t address, PortType port) {
    BreakpointMutationIoResult result;
    (void)SocketCommand::execute(port, [&](WindowsSocketClient* client, int handle) -> bool {
        result.requestStarted = true;
        unsigned char command = CMD_KERNEL_REMOVEBREAKPOINT;
        if (!SocketCommand::sendCommandWithHandle(client, command, handle))
            return false;
        if (!client->Send(&address, sizeof(address)))
            return false;
        int applied = 0;
        if (!client->Receive(&applied, sizeof(applied)))
            return false;
        result.responseReceived = true;
        result.applied = applied != 0;
        if (result.applied) {
            untrackBreakpointAddress(address);
        }
        return true;
    });
    return result;
}

BreakpointMutationIoResult SuspendKernelBreakpointTracked(
    uint64_t address, PortType port) {
    BreakpointMutationIoResult result;
    (void)SocketCommand::execute(port, [&](WindowsSocketClient* client, int handle) -> bool {
        result.requestStarted = true;
        unsigned char command = CMD_KERNEL_SUSPENDBREAKPOINT;
        if (!SocketCommand::sendCommandWithHandle(client, command, handle))
            return false;
        if (!client->Send(&address, sizeof(address)))
            return false;
        int applied = 0;
        if (!client->Receive(&applied, sizeof(applied)))
            return false;
        result.responseReceived = true;
        result.applied = applied != 0;
        return true;
    });
    return result;
}

BreakpointMutationIoResult ResumeKernelBreakpointTracked(
    uint64_t address, PortType port) {
    BreakpointMutationIoResult result;
    (void)SocketCommand::execute(port, [&](WindowsSocketClient* client, int handle) -> bool {
        result.requestStarted = true;
        unsigned char command = CMD_KERNEL_RESUMEBREAKPOINT;
        if (!SocketCommand::sendCommandWithHandle(client, command, handle))
            return false;
        if (!client->Send(&address, sizeof(address)))
            return false;
        int applied = 0;
        if (!client->Receive(&applied, sizeof(applied)))
            return false;
        result.responseReceived = true;
        result.applied = applied != 0;
        return true;
    });
    return result;
}

bool SetKernelBreakpoint(uint64_t address, uint32_t bpType,
                         uint32_t bpSize, PortType port) {
    const auto result = SetKernelBreakpointTracked(
        address, bpType, bpSize, port);
    return result.responseReceived && result.applied;
}

bool RemoveKernelBreakpoint(uint64_t address, PortType port) {
    const auto result = RemoveKernelBreakpointTracked(address, port);
    return result.responseReceived && result.applied;
}

bool SuspendKernelBreakpoint(uint64_t address, PortType port) {
    const auto result = SuspendKernelBreakpointTracked(address, port);
    return result.responseReceived && result.applied;
}

bool ResumeKernelBreakpoint(uint64_t address, PortType port) {
    const auto result = ResumeKernelBreakpointTracked(address, port);
    return result.responseReceived && result.applied;
}

bool ReadKernelBreakpointInfo(uint64_t address, std::vector<HW_HIT_INFO> &infos, PortType port) {
    size_t total = 0;
    return ReadKernelBreakpointInfoPage(
        address, 0, static_cast<size_t>(kMaxBreakpointHitCount),
        infos, total, port);
}

bool ReadKernelBreakpointInfoPage(
    uint64_t address, size_t offset, size_t limit,
    std::vector<HW_HIT_INFO> &infos, size_t &total, PortType port) {
    infos.clear();
    total = 0;
    if (offset > static_cast<size_t>(kMaxBreakpointHitCount) ||
        limit == 0 || limit > static_cast<size_t>(kMaxBreakpointHitCount)) {
        return false;
    }

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
        if (result < 0 ||
            result > kMaxBreakpointHitCount ||
            TotalCount > static_cast<uint64_t>(kMaxBreakpointHitCount) ||
            static_cast<uint64_t>(result) > TotalCount)
            return false;
        const size_t resultCount = static_cast<size_t>(result);
        const size_t pageBegin = (std::min)(offset, resultCount);
        const size_t pageEnd = pageBegin + (std::min)(limit, resultCount - pageBegin);
        std::vector<HW_HIT_INFO> receivedInfos;
        receivedInfos.reserve(pageEnd - pageBegin);
        std::vector<HW_HIT_INFO> chunk(
            (std::min)(kBreakpointHitReadChunk, resultCount));
        for (size_t chunkBegin = 0; chunkBegin < resultCount;) {
            const size_t chunkCount = (std::min)(
                chunk.size(), resultCount - chunkBegin);
            if (!client->Receive(
                    chunk.data(), chunkCount * sizeof(HW_HIT_INFO))) {
                return false;
            }
            for (size_t index = 0; index < chunkCount; ++index) {
                const size_t absoluteIndex = chunkBegin + index;
                if (absoluteIndex >= pageBegin &&
                    absoluteIndex < pageEnd) {
                    receivedInfos.push_back(chunk[index]);
                }
            }
            chunkBegin += chunkCount;
        }
        // The protocol has no offset parameter. Only `result` entries follow
        // this response, so pagination is bounded to the retrievable entries
        // rather than advertising an unreachable cumulative TotalCount.
        total = resultCount;
        infos.swap(receivedInfos);
        return true;
    });
}

bool ClearTrackedKernelBreakpoints(PortType port) {
    SocketCommand::TransactionLease transaction(port);
    if (!transaction) {
        clearTrackedBreakpointAddresses();
        return false;
    }
    auto addresses = snapshotTrackedBreakpointAddresses();
    bool allRemoved = true;
    for (uint64_t address : addresses) {
        if (!RemoveKernelBreakpoint(address, port)) {
            allRemoved = false;
        }
    }
    clearTrackedBreakpointAddresses();
    return allRemoved;
}

void ResetTrackedKernelBreakpoints() {
    clearTrackedBreakpointAddresses();
}
