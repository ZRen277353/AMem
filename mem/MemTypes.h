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
    bool connectionPoisoned = false;
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

struct ModuleInfo {
    uint64_t base = 0;
    uint64_t size = 0;
    int type = 0;
    int flag = 0;
    std::string name;
};

struct ModuleListRequest {
    std::string filter;
    size_t offset = 0;
    size_t limit = 200;
};

struct ModulePage {
    std::vector<ModuleInfo> items;
    size_t total = 0;
    size_t offset = 0;
    std::optional<size_t> nextOffset;
    TargetSnapshot target;
};

struct ModuleResolveRequest {
    std::string name;
};

struct ResolvedModule {
    ModuleInfo module;
    TargetSnapshot target;
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

enum class ScalarType {
    Byte,
    Word,
    Dword,
    Qword,
    Xor,
    Float,
    Double,
};

struct ValueReadRequest {
    uint64_t address = 0;
    ScalarType type = ScalarType::Dword;
};

struct ValueWriteRequest {
    uint64_t address = 0;
    ScalarType type = ScalarType::Dword;
    std::vector<unsigned char> bytes;
};

struct ScalarValue {
    uint64_t address = 0;
    ScalarType type = ScalarType::Dword;
    std::vector<unsigned char> bytes;
    TargetSnapshot target;
};

struct DecodedScalarValue {
    ScalarType type = ScalarType::Dword;
    bool floatingPoint = false;
    uint64_t integerValue = 0;
    double floatingValue = 0.0;
};

struct MemoryWriteRequest {
    uint64_t address = 0;
    std::vector<unsigned char> bytes;
};

struct MemoryWriteBackendResult {
    bool requestStarted = false;
    bool responseReceived = false;
    int32_t writtenBytes = 0;
};

struct WriteReceipt {
    uint64_t address = 0;
    uint32_t requestedBytes = 0;
    uint32_t writtenBytes = 0;
    bool completedAfterCancelRequest = false;
    bool completedAfterDeadline = false;
    TargetSnapshot target;
};

inline constexpr uint32_t kMaxAgentMemoryReadBytes = 64u * 1024u;
inline constexpr uint32_t kMaxAgentMemoryWriteBytes = 4u * 1024u;
inline constexpr size_t kMaxProcessPageSize = 1000;
inline constexpr size_t kMaxModulePageSize = 1000;
inline constexpr size_t kMaxModuleResultCount = 65536;
inline constexpr size_t kMaxModuleNameBytesTotal = 16u * 1024u * 1024u;
inline constexpr size_t kMaxTextParameterBytes = 4096;
inline constexpr size_t kMaxScalarTypeNameBytes = 64;
inline constexpr size_t kMaxScalarValueTextBytes = 256;

} // namespace Mem
