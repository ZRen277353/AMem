#pragma once

#include <atomic>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
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

struct DriverInitializeRequest {
    std::string card;
};

struct DriverInitializationBackendResult {
    bool requestStarted = false;
    bool responseReceived = false;
    bool accepted = false;
    std::string message;
};

struct DriverInitializationReceipt {
    std::string message;
    bool completedAfterCancelRequest = false;
    bool completedAfterDeadline = false;
    uint64_t connectionGeneration = 0;
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

struct PointerResolveRequest {
    std::string moduleName;
    uint64_t baseOffset = 0;
    std::vector<uint64_t> offsets;
    bool dereferenceFinal = true;
};

struct PointerResolution {
    ModuleInfo module;
    uint64_t baseOffset = 0;
    uint64_t startAddress = 0;
    uint64_t address = 0;
    size_t dereferenceCount = 0;
    bool dereferencedFinal = false;
    TargetSnapshot target;
};

struct DisassemblyRequest {
    uint64_t address = 0;
    size_t instructionCount = 0;
};

struct InstructionWord {
    uint64_t address = 0;
    uint32_t encoding = 0;
};

struct DisassemblyBlock {
    uint64_t address = 0;
    std::vector<unsigned char> bytes;
    std::vector<InstructionWord> instructions;
    TargetSnapshot target;
};

struct SymbolResolveRequest {
    std::string moduleName;
    std::string symbolName;
};

struct SymbolListRequest {
    std::string moduleName;
    std::optional<uint64_t> expectedEpoch;
    size_t offset = 0;
    size_t limit = 100;
};

struct SymbolTableRequest {
    std::string moduleName;
};

struct SymbolInfo {
    uint64_t address = 0;
    std::string name;
};

struct SymbolSessionSnapshot {
    uint64_t epoch = 0;
    ModuleInfo module;
    size_t total = 0;
    TargetSnapshot target;
};

struct ResolvedSymbol {
    std::string name;
    uint64_t address = 0;
    SymbolSessionSnapshot session;
};

struct SymbolPage {
    std::vector<SymbolInfo> items;
    size_t total = 0;
    size_t offset = 0;
    std::optional<size_t> nextOffset;
    SymbolSessionSnapshot session;
};

struct SymbolTable {
    std::vector<SymbolInfo> items;
    SymbolSessionSnapshot session;
};

enum class BreakpointAccess : uint32_t {
    Read = 1,
    Write = 2,
    ReadWrite = 3,
    Execute = 4,
};

enum class BreakpointAction {
    Set,
    Remove,
    Suspend,
    Resume,
};

struct BreakpointSetRequest {
    uint64_t address = 0;
    BreakpointAccess access = BreakpointAccess::Write;
    uint32_t size = 4;
};

struct BreakpointAddressRequest {
    uint64_t address = 0;
};

struct BreakpointHitBatchRequest {
    uint64_t address = 0;
    size_t limit = 50000;
};

struct BreakpointMutationReceipt {
    uint64_t address = 0;
    BreakpointAction action = BreakpointAction::Set;
    std::optional<BreakpointAccess> access;
    std::optional<uint32_t> size;
    bool completedAfterCancelRequest = false;
    bool completedAfterDeadline = false;
    TargetSnapshot target;
};

struct BreakpointHit {
    uint64_t hitAddress = 0;
    uint64_t hitTime = 0;
    std::array<uint64_t, 31> registers{};
    uint64_t stackPointer = 0;
    uint64_t programCounter = 0;
    uint64_t pstate = 0;
    uint64_t originalX0 = 0;
    uint64_t syscallNumber = 0;
    std::array<std::array<unsigned char, 16>, 32> vectorRegisters{};
    uint32_t fpsr = 0;
    uint32_t fpcr = 0;
};

struct BreakpointHitBatch {
    uint64_t address = 0;
    std::vector<BreakpointHit> items;
    size_t available = 0;
    size_t dropped = 0;
    TargetSnapshot target;
};

struct BreakpointMutationBackendResult {
    bool requestStarted = false;
    bool responseReceived = false;
    bool applied = false;
};

enum class ScanStartKind {
    Value,
    Unknown,
    BytePattern,
};

enum class ScanDataType {
    Byte,
    Word,
    Dword,
    Qword,
    Xor,
    Float,
    Double,
    Bytes,
};

enum class ScanMode {
    Exact,
    Greater,
    Less,
    Between,
    Unknown,
    Increased,
    IncreasedBy,
    Decreased,
    DecreasedBy,
    Changed,
    Unchanged,
};

enum class ScanMemoryRegion : int32_t {
    All = -1,
    Anonymous = 1 << 5,
    CAlloc = 1 << 2,
    CHeap = 1 << 0,
    CData = 1 << 3,
    CBss = 1 << 4,
    JavaHeap = 1 << 1,
    Java = 1 << 16,
    Stack = 1 << 6,
    Video = 1 << 20,
    CodeApp = 1 << 14,
    CodeSystem = 1 << 15,
    Ashmem = 1 << 19,
    Bad = 1 << 17,
    Other = -2080896,
};

struct ScanStartRequest {
    ScanStartKind kind = ScanStartKind::Value;
    ScanDataType dataType = ScanDataType::Dword;
    ScanMode mode = ScanMode::Exact;
    ScanMemoryRegion memoryRegion = ScanMemoryRegion::All;
    uint64_t start = 0;
    uint64_t end = (std::numeric_limits<uint64_t>::max)();
    std::vector<unsigned char> value;
};

struct ScanRefineRequest {
    std::optional<uint64_t> expectedEpoch;
    std::optional<ScanDataType> dataType;
    ScanMode mode = ScanMode::Exact;
    std::vector<unsigned char> value;
};

struct ScanResultsRequest {
    std::optional<uint64_t> expectedEpoch;
    size_t offset = 0;
    size_t limit = 100;
};

struct ScanClearRequest {
    std::optional<uint64_t> expectedEpoch;
};

struct ScanSessionSnapshot {
    uint64_t epoch = 0;
    ScanStartKind kind = ScanStartKind::Value;
    ScanDataType dataType = ScanDataType::Dword;
    ScanMode mode = ScanMode::Exact;
    ScanMemoryRegion memoryRegion = ScanMemoryRegion::All;
    uint64_t start = 0;
    uint64_t end = (std::numeric_limits<uint64_t>::max)();
    size_t resultCount = 0;
    TargetSnapshot target;
};

struct ScanSummary {
    ScanSessionSnapshot session;
    bool completedAfterCancelRequest = false;
    bool completedAfterDeadline = false;
};

struct ScanResultItem {
    uint64_t address = 0;
    uint64_t value = 0;
};

struct ScanResultPage {
    std::vector<ScanResultItem> items;
    size_t total = 0;
    size_t offset = 0;
    std::optional<size_t> nextOffset;
    ScanSessionSnapshot session;
};

struct ScanClearResult {
    uint64_t clearedEpoch = 0;
    uint64_t currentEpoch = 0;
    bool completedAfterCancelRequest = false;
    bool completedAfterDeadline = false;
    TargetSnapshot target;
};

struct ScanExecutionBackendResult {
    bool requestStarted = false;
    bool responseReceived = false;
    int resultCount = -1;
    bool cancelRequested = false;
};

struct ScanProgressUpdate {
    float progress = 0.0f;
    uint64_t matchCount = 0;
    uint64_t scannedBytes = 0;
    uint64_t totalBytes = 0;
};

using ScanProgressSink = std::function<void(const ScanProgressUpdate&)>;

struct ScanRemoveRequest {
    std::optional<uint64_t> expectedEpoch;
    std::vector<uint64_t> addresses;
};

struct ScanRemoveResult {
    uint64_t previousEpoch = 0;
    uint64_t currentEpoch = 0;
    size_t requested = 0;
    size_t previousCount = 0;
    size_t currentCount = 0;
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
inline constexpr size_t kMaxPointerOffsetCount = 1024;
inline constexpr size_t kMaxDisassemblyInstructionCount = 512;
inline constexpr size_t kMaxSymbolPageSize = 1000;
inline constexpr size_t kMaxSymbolResultCount = 1000000;
inline constexpr size_t kMaxSymbolNameBytes = 64u * 1024u;
inline constexpr size_t kMaxSymbolPageNameBytes = 4u * 1024u * 1024u;
inline constexpr size_t kMaxSymbolTableNameBytes = 64u * 1024u * 1024u;
inline constexpr size_t kMaxAgentBreakpointHitBatchSize = 100;
inline constexpr size_t kMaxBreakpointHitBatchSize = 50000;
inline constexpr size_t kMaxBreakpointHitCount = 100000;
inline constexpr size_t kMaxScanValueBytes = 4096;
inline constexpr size_t kMaxScanResultPageSize = 1000;
inline constexpr size_t kMaxScanResultCount = 5000000;
inline constexpr size_t kMaxScanResultRemovalCount = 100000;
inline constexpr size_t kMaxTextParameterBytes = 4096;
inline constexpr size_t kMaxDriverCardBytes = 4096;
inline constexpr size_t kMaxScalarTypeNameBytes = 64;
inline constexpr size_t kMaxScalarValueTextBytes = 256;

} // namespace Mem
