#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Mem {

using CancellationToken = std::shared_ptr<std::atomic<bool>>;

struct TargetSnapshot {
    int pid = 0;
    int processHandle = 0;
    uint64_t processRevision = 0;
    uint64_t connectionGeneration = 0;

    bool isAttached() const {
        return pid > 0 && processHandle != 0;
    }
};

inline bool operator==(const TargetSnapshot& lhs, const TargetSnapshot& rhs) {
    return lhs.pid == rhs.pid &&
           lhs.processHandle == rhs.processHandle &&
           lhs.processRevision == rhs.processRevision &&
           lhs.connectionGeneration == rhs.connectionGeneration;
}

inline bool operator!=(const TargetSnapshot& lhs, const TargetSnapshot& rhs) {
    return !(lhs == rhs);
}

struct OperationContext {
    uint64_t connectionGeneration = 0;
    std::optional<TargetSnapshot> target;
    CancellationToken cancellation;
    std::chrono::steady_clock::time_point deadline =
        (std::chrono::steady_clock::time_point::max)();
};

struct Status {
    bool connected = false;
    uint64_t connectionGeneration = 0;
    TargetSnapshot target;
    std::string processName;
    std::optional<int> serverVersion;
    std::string serverVersionString;
    std::optional<int> architectureType;
    std::string architectureName;
};

struct ProcessInfo {
    int pid = 0;
    std::string name;
};

struct ProcessListRequest {
    std::string filter;
    size_t offset = 0;
    size_t limit = 200;
};

struct ProcessPage {
    std::vector<ProcessInfo> items;
    size_t total = 0;
    size_t offset = 0;
    std::optional<size_t> nextOffset;
};

struct OpenProcessRequest {
    int pid = 0;
    std::string name;
};

struct OpenProcessResult {
    TargetSnapshot target;
    std::string name;
};

struct MemoryReadRequest {
    uint64_t address = 0;
    uint32_t size = 0;
};

struct MemoryBlock {
    uint64_t address = 0;
    std::vector<unsigned char> bytes;
    TargetSnapshot target;
};

inline constexpr uint32_t kMaxAgentMemoryReadBytes = 64u * 1024u;
inline constexpr size_t kMaxProcessPageSize = 1000;
inline constexpr size_t kMaxTextParameterBytes = 4096;

} // namespace Mem
