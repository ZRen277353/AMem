#include "../gui/ai/AgentMemTools.h"
#include "../gui/ai/AgentController.h"
#include "../gui/ai/AgentMutationAudit.h"
#include "../gui/ai/AgentTaskExecutor.h"
#include "../gui/ai/AiLimits.h"
#include "../gui/ai/ChatSession.h"
#include "../gui/ai/ProviderRegistry.h"
#include "../gui/ai/ToolExecutor.h"
#include "../gui/ai/ToolCallSecurity.h"
#include "../mem/Address.h"
#include "../mem/MemService.h"
#include "../mem/ValueCodec.h"
#include "../socket/DeviceSession.h"
#include "../third_party/nlohmann/json.hpp"

#include <algorithm>
#include <functional>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <list>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace AI {

// AgentController's provider-dispatch path is not exercised by this
// no-device test binary. Supplying the one referenced registry method keeps
// the controller's tool/context behavior independently linkable.
AIProvider* ProviderRegistry::getProvider(const std::string&) {
    return nullptr;
}

} // namespace AI

namespace Gui {

std::list<std::pair<std::string, int>> logs;
std::mutex logsMutex;

} // namespace Gui

namespace {

using nlohmann::json;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

AI::ToolCall toolCall(std::string id,
                      std::string name,
                      std::string arguments = "{}");

class FakeBackend final : public Mem::IMemBackend {
public:
    bool connected = true;
    bool poisoned = false;
    bool connectSucceeds = true;
    int connectCalls = 0;
    int disconnectCalls = 0;
    std::string lastConnectHost;
    uint16_t lastConnectPort = 0;
    uint64_t generation = 1;
    Mem::TargetSnapshot target{42, 420, 2, 1};
    std::string selectedName = "com.example.game";
    std::vector<Mem::ProcessInfo> processes = {
        {10, "android.system"},
        {42, "com.example.game"},
        {84, "com.example.game.helper"},
    };
    std::vector<Mem::ModuleInfo> modules = {
        {0x1000, 0x2000, 1, 5, "/system/lib64/libc.so"},
        {0x5000, 0x3000, 2, 3, "/data/app/libgame.so"},
        {0x9000, 0x1000, 2, 1, "/data/app/libgame_helper.so"},
    };
    std::vector<unsigned char> memory = {0xDE, 0xAD, 0xBE, 0xEF};
    bool fetchProcessesSucceeds = true;
    bool fetchModulesSucceeds = true;
    bool openProcessSucceeds = true;
    bool readMemorySucceeds = true;
    bool changeTargetAfterRead = false;
    bool changeGenerationAfterRead = false;
    bool batchReadSucceeds = true;
    bool batchReturnPartialSet = false;
    bool batchReturnShortBlock = false;
    bool changeTargetAfterBatch = false;
    bool changeGenerationAfterBatch = false;
    bool changeGenerationDuringOpen = false;
    bool changeTargetAfterModuleFetch = false;
    bool changeGenerationAfterModuleFetch = false;
    bool writeRequestStarted = true;
    bool writeResponseReceived = true;
    int32_t writeReportedBytes = -1;
    bool changeTargetAfterWrite = false;
    bool changeGenerationAfterWrite = false;
    int writeDelayMs = 0;
    Mem::CancellationToken cancelDuringWrite;
    int writeCalls = 0;
    bool driverRequestStarted = true;
    bool driverResponseReceived = true;
    bool driverAccepted = true;
    bool changeGenerationAfterDriver = false;
    Mem::CancellationToken cancelDuringDriver;
    int driverDelayMs = 0;
    int driverCalls = 0;
    uint64_t lastDriverContextGeneration = 0;
    std::string driverMessage = "2026-07-12 12:00:00";
    std::string lastDriverCard;
    int readCalls = 0;
    int batchReadCalls = 0;
    Mem::MemoryReadChannel lastReadChannel =
        Mem::MemoryReadChannel::Foreground;
    Mem::MemoryReadChannel lastTransactionChannel =
        Mem::MemoryReadChannel::Foreground;
    int fetchModuleCalls = 0;
    int beginReadTransactionCalls = 0;
    int transactionFetchCalls = 0;
    int transactionReadCalls = 0;
    bool transactionActive = false;
    bool allTransactionOperationsGuarded = true;
    bool transactionBlockedCompetitor = false;
    bool beginReadTransactionSucceeds = true;
    std::timed_mutex transactionMutex;
    std::unordered_map<uint64_t, uint64_t> pointerMemory;
    int cancelAfterTransactionReads = 0;
    Mem::CancellationToken transactionCancellation;
    uint64_t scanEpochValue = 0;
    std::vector<Mem::ScanResultItem> scanResultsData = {
        {0x1000, 10},
        {0x2000, 20},
        {0x3000, 30},
    };
    bool scanSetRangeSucceeds = true;
    bool scanRequestStarted = true;
    bool scanResponseReceived = true;
    bool scanCountSucceeds = true;
    bool scanResultsSucceed = true;
    bool scanClearSucceeds = true;
    bool scanRemoveSucceeds = true;
    int scanReportedCount = -1;
    bool changeTargetAfterScan = false;
    bool changeGenerationAfterScan = false;
    Mem::CancellationToken cancelDuringScan;
    int beginScanTransactionCalls = 0;
    int scanSetRangeCalls = 0;
    int scanStartCalls = 0;
    int scanRefineCalls = 0;
    int scanCountCalls = 0;
    int scanResultPageCalls = 0;
    int scanClearCalls = 0;
    int scanRemoveCalls = 0;
    std::optional<Mem::ScanStartRequest> lastScanStart;
    std::optional<Mem::ScanRefineRequest> lastScanRefine;
    uint64_t symbolEpochValue = 0;
    uint64_t activeSymbolModuleBase = 0;
    std::vector<Mem::SymbolInfo> symbolData = {
        {0x5100, "GameInit"},
        {0x5200, "GameUpdate"},
        {0x5300, "GameRender"},
    };
    bool symbolInitializeSucceeds = true;
    bool symbolFetchSucceeds = true;
    bool symbolFindSucceeds = true;
    bool changeTargetAfterSymbol = false;
    bool changeGenerationAfterSymbol = false;
    int beginSymbolTransactionCalls = 0;
    int symbolInitializeCalls = 0;
    int symbolFetchCalls = 0;
    int symbolFindCalls = 0;
    bool breakpointRequestStarted = true;
    bool breakpointResponseReceived = true;
    bool breakpointApplied = true;
    bool breakpointHitsSucceed = true;
    bool changeTargetAfterBreakpoint = false;
    bool changeGenerationAfterBreakpoint = false;
    bool changeTargetAfterBreakpointHits = false;
    bool changeGenerationAfterBreakpointHits = false;
    int breakpointDelayMs = 0;
    Mem::CancellationToken cancelDuringBreakpoint;
    int breakpointSetCalls = 0;
    int breakpointRemoveCalls = 0;
    int breakpointSuspendCalls = 0;
    int breakpointResumeCalls = 0;
    int breakpointHitBatchCalls = 0;
    uint64_t lastBreakpointAddress = 0;
    Mem::BreakpointAccess lastBreakpointAccess =
        Mem::BreakpointAccess::Write;
    uint32_t lastBreakpointSize = 0;
    std::vector<Mem::BreakpointHit> breakpointHitsData = [] {
        std::vector<Mem::BreakpointHit> hits(3);
        hits[0].hitAddress = 0x7000;
        hits[0].hitTime = 100;
        hits[0].registers[0] = 0xA0;
        hits[0].programCounter = 0x7100;
        hits[0].stackPointer = 0x7200;
        hits[0].originalX0 = 0x7300;
        hits[0].syscallNumber = 64;
        hits[0].vectorRegisters[0][0] = 0xAB;
        hits[0].vectorRegisters[31][15] = 0xCD;
        hits[0].fpsr = 0x01000000;
        hits[0].fpcr = 0x00400000;
        hits[1].hitAddress = 0x7004;
        hits[1].hitTime = 200;
        hits[1].registers[1] = 0xB1;
        hits[1].programCounter = 0x7104;
        hits[1].stackPointer = 0x7204;
        hits[2].hitAddress = 0x7008;
        hits[2].hitTime = 300;
        hits[2].registers[2] = 0xC2;
        hits[2].programCounter = 0x7108;
        hits[2].stackPointer = 0x7208;
        return hits;
    }();
    uint64_t lastReadAddress = 0;
    uint32_t lastReadSize = 0;
    uint64_t lastWriteAddress = 0;
    std::vector<unsigned char> lastWriteBytes;
    bool freezeRequestStarted = true;
    bool freezeResponseReceived = true;
    bool freezeApplied = true;
    bool changeTargetAfterFreeze = false;
    bool changeGenerationAfterFreeze = false;
    int freezeDelayMs = 0;
    Mem::CancellationToken cancelDuringFreeze;
    int freezeAddCalls = 0;
    int freezeUpdateCalls = 0;
    int freezeRemoveCalls = 0;
    int freezeClearCalls = 0;
    uint64_t lastFreezeAddress = 0;
    std::vector<unsigned char> lastFreezeBytes;
    Mem::FreezeAction lastFreezeAction = Mem::FreezeAction::Add;

    bool isConnected() const override {
        return connected;
    }

    bool isConnectionPoisoned() const override {
        return poisoned;
    }

    uint64_t connectionGeneration() const override {
        return generation;
    }

    Mem::TargetSnapshot targetSnapshot() const override {
        Mem::TargetSnapshot snapshot = target;
        snapshot.connectionGeneration = generation;
        return snapshot;
    }

    std::string processName() const override {
        return selectedName;
    }

    bool connect(const std::string& host, uint16_t port) override {
        ++connectCalls;
        lastConnectHost = host;
        lastConnectPort = port;
        ++generation;
        target = Mem::TargetSnapshot{0, 0, target.processRevision + 2,
                                     generation};
        selectedName.clear();
        poisoned = false;
        connected = connectSucceeds;
        return connected;
    }

    bool disconnect() override {
        ++disconnectCalls;
        const bool wasConnected = connected || poisoned;
        if (wasConnected) {
            ++generation;
        }
        connected = false;
        poisoned = false;
        target = Mem::TargetSnapshot{0, 0, target.processRevision + 2,
                                     generation};
        selectedName.clear();
        return wasConnected;
    }

    bool fetchServerVersion(int& version,
                            std::string& versionString) override {
        version = 101;
        versionString = "1.0.1";
        return connected;
    }

    bool fetchArchitecture(int& type, std::string& name) override {
        type = 3;
        name = "Kernel";
        return connected;
    }

    Mem::DriverInitializationBackendResult initializeDriver(
        const Mem::OperationContext& context,
        const std::string& card) override {
        ++driverCalls;
        lastDriverCard = card;
        lastDriverContextGeneration = context.connectionGeneration;
        if (driverDelayMs > 0) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(driverDelayMs));
        }
        if (cancelDuringDriver) {
            cancelDuringDriver->store(true, std::memory_order_release);
        }
        if (changeGenerationAfterDriver) {
            ++generation;
        }
        Mem::DriverInitializationBackendResult result;
        result.requestStarted = driverRequestStarted;
        result.responseReceived = driverResponseReceived;
        result.accepted = driverAccepted;
        result.message = driverMessage;
        return result;
    }

    bool fetchProcesses(const Mem::OperationContext&,
                        std::vector<Mem::ProcessInfo>& output) override {
        if (!fetchProcessesSucceeds) {
            return false;
        }
        output = processes;
        return true;
    }

    bool openProcess(const Mem::OperationContext&,
                     int pid,
                     const std::string& name) override {
        if (!openProcessSucceeds) {
            return false;
        }
        target.pid = pid;
        target.processHandle = pid + 1000;
        target.processRevision += 2;
        target.connectionGeneration = generation;
        selectedName = name;
        if (changeGenerationDuringOpen) {
            ++generation;
        }
        return true;
    }

    bool fetchModules(const Mem::OperationContext&,
                      std::vector<Mem::ModuleInfo>& output) override {
        ++fetchModuleCalls;
        if (!fetchModulesSucceeds) {
            return false;
        }
        output = modules;
        if (changeTargetAfterModuleFetch) {
            target.processRevision += 2;
        }
        if (changeGenerationAfterModuleFetch) {
            ++generation;
        }
        return true;
    }

    class ReadTransaction final : public Mem::IMemReadTransaction {
    public:
        ReadTransaction(FakeBackend& backend,
                        Mem::MemoryReadChannel channel)
            : backend_(backend), channel_(channel),
              lock_(backend.transactionMutex) {
            backend_.transactionActive = true;
            backend_.lastTransactionChannel = channel;
        }

        ~ReadTransaction() override {
            backend_.transactionActive = false;
        }

        bool fetchModules(std::vector<Mem::ModuleInfo>& output) override {
            ++backend_.transactionFetchCalls;
            backend_.allTransactionOperationsGuarded =
                backend_.allTransactionOperationsGuarded &&
                backend_.transactionActive;
            return backend_.fetchModules({}, output);
        }

        bool readMemory(uint64_t address,
                        uint32_t size,
                        std::vector<unsigned char>& output) override {
            ++backend_.transactionReadCalls;
            backend_.allTransactionOperationsGuarded =
                backend_.allTransactionOperationsGuarded &&
                backend_.transactionActive;
            if (backend_.transactionReadCalls == 1) {
                bool competitorAcquired = false;
                std::thread competitor([&] {
                    competitorAcquired =
                        backend_.transactionMutex.try_lock_for(
                            std::chrono::milliseconds(5));
                    if (competitorAcquired) {
                        backend_.transactionMutex.unlock();
                    }
                });
                competitor.join();
                backend_.transactionBlockedCompetitor =
                    !competitorAcquired;
            }
            const bool result = backend_.readMemory(
                {}, channel_,
                address, size, output);
            if (backend_.cancelAfterTransactionReads > 0 &&
                backend_.transactionReadCalls >=
                    backend_.cancelAfterTransactionReads &&
                backend_.transactionCancellation) {
                backend_.transactionCancellation->store(
                    true, std::memory_order_release);
            }
            return result;
        }

        bool readMemoryBatch(
            const std::vector<Mem::MemoryReadRequest>& requests,
            std::vector<Mem::MemoryBlock>& blocks) override {
            ++backend_.batchReadCalls;
            blocks.clear();
            if (!backend_.batchReadSucceeds) {
                return false;
            }
            blocks.reserve(requests.size());
            for (size_t index = 0; index < requests.size(); ++index) {
                const auto& request = requests[index];
                if (backend_.batchReturnPartialSet &&
                    index + 1 == requests.size()) {
                    break;
                }
                Mem::MemoryBlock block;
                block.address = request.address;
                if (!readMemory(request.address, request.size, block.bytes)) {
                    return false;
                }
                if (backend_.batchReturnShortBlock && index == 0 &&
                    !block.bytes.empty()) {
                    block.bytes.pop_back();
                }
                blocks.push_back(std::move(block));
            }
            if (backend_.changeTargetAfterBatch) {
                backend_.target.processRevision += 2;
            }
            if (backend_.changeGenerationAfterBatch) {
                ++backend_.generation;
            }
            return true;
        }

    private:
        FakeBackend& backend_;
        Mem::MemoryReadChannel channel_;
        std::unique_lock<std::timed_mutex> lock_;
    };

    std::unique_ptr<Mem::IMemReadTransaction> beginReadTransaction(
        const Mem::OperationContext& context,
        Mem::MemoryReadChannel channel) override {
        ++beginReadTransactionCalls;
        if (!beginReadTransactionSucceeds || !context.target ||
            context.connectionGeneration != generation ||
            targetSnapshot() != *context.target) {
            return nullptr;
        }
        return std::make_unique<ReadTransaction>(*this, channel);
    }

    class ScanTransaction final : public Mem::IMemScanTransaction {
    public:
        explicit ScanTransaction(FakeBackend& backend)
            : backend_(backend), lock_(backend.transactionMutex) {
            backend_.transactionActive = true;
        }

        ~ScanTransaction() override {
            backend_.transactionActive = false;
        }

        uint64_t scanEpoch() const override {
            return backend_.scanEpochValue;
        }

        bool setRange(Mem::ScanMemoryRegion) override {
            ++backend_.scanSetRangeCalls;
            ++backend_.scanEpochValue;
            return backend_.scanSetRangeSucceeds;
        }

        Mem::ScanExecutionBackendResult startScan(
            const Mem::ScanStartRequest& request,
            const Mem::ScanProgressSink& progress) override {
            ++backend_.scanStartCalls;
            ++backend_.scanEpochValue;
            backend_.lastScanStart = request;
            if (progress) {
                progress(Mem::ScanProgressUpdate{
                    0.5f, 2, 4096, 8192});
            }
            return backend_.scanExecutionResult();
        }

        Mem::ScanExecutionBackendResult refineScan(
            const Mem::ScanSessionSnapshot&,
            const Mem::ScanRefineRequest& request,
            const Mem::ScanProgressSink& progress) override {
            ++backend_.scanRefineCalls;
            ++backend_.scanEpochValue;
            backend_.lastScanRefine = request;
            if (progress) {
                progress(Mem::ScanProgressUpdate{
                    1.0f, 1, 8192, 8192});
            }
            return backend_.scanExecutionResult();
        }

        bool scanResultCount(int& count) override {
            ++backend_.scanCountCalls;
            if (!backend_.scanCountSucceeds) {
                return false;
            }
            count = static_cast<int>(backend_.scanResultsData.size());
            return true;
        }

        bool fetchScanResults(
            size_t offset,
            size_t limit,
            std::vector<Mem::ScanResultItem>& results) override {
            ++backend_.scanResultPageCalls;
            if (!backend_.scanResultsSucceed) {
                return false;
            }
            const size_t begin = (std::min)(
                offset, backend_.scanResultsData.size());
            const size_t end = begin + (std::min)(
                limit, backend_.scanResultsData.size() - begin);
            results.assign(backend_.scanResultsData.begin() + begin,
                           backend_.scanResultsData.begin() + end);
            return true;
        }

        bool clearScan() override {
            ++backend_.scanClearCalls;
            ++backend_.scanEpochValue;
            if (!backend_.scanClearSucceeds) {
                return false;
            }
            backend_.scanResultsData.clear();
            return true;
        }

        bool removeScanResults(
            const std::vector<uint64_t>& addresses) override {
            ++backend_.scanRemoveCalls;
            ++backend_.scanEpochValue;
            if (!backend_.scanRemoveSucceeds) {
                return false;
            }
            backend_.scanResultsData.erase(
                std::remove_if(
                    backend_.scanResultsData.begin(),
                    backend_.scanResultsData.end(),
                    [&](const Mem::ScanResultItem& item) {
                        return std::binary_search(
                            addresses.begin(), addresses.end(),
                            item.address);
                    }),
                backend_.scanResultsData.end());
            return true;
        }

    private:
        FakeBackend& backend_;
        std::unique_lock<std::timed_mutex> lock_;
    };

    Mem::ScanExecutionBackendResult scanExecutionResult() {
        if (cancelDuringScan) {
            cancelDuringScan->store(true, std::memory_order_release);
        }
        if (changeTargetAfterScan) {
            target.processRevision += 2;
        }
        if (changeGenerationAfterScan) {
            ++generation;
        }
        Mem::ScanExecutionBackendResult result;
        result.requestStarted = scanRequestStarted;
        result.responseReceived = scanResponseReceived;
        result.resultCount = scanReportedCount >= 0
            ? scanReportedCount
            : static_cast<int>(scanResultsData.size());
        result.cancelRequested = cancelDuringScan &&
            cancelDuringScan->load(std::memory_order_acquire);
        return result;
    }

    uint64_t scanEpoch() const override {
        return scanEpochValue;
    }

    std::unique_ptr<Mem::IMemScanTransaction> beginScanTransaction(
        const Mem::OperationContext& context) override {
        ++beginScanTransactionCalls;
        if (!context.target || context.connectionGeneration != generation ||
            targetSnapshot() != *context.target) {
            return nullptr;
        }
        return std::make_unique<ScanTransaction>(*this);
    }

    class SymbolTransaction final : public Mem::IMemSymbolTransaction {
    public:
        explicit SymbolTransaction(FakeBackend& backend)
            : backend_(backend), lock_(backend.transactionMutex) {
            backend_.transactionActive = true;
        }

        ~SymbolTransaction() override {
            backend_.transactionActive = false;
        }

        uint64_t symbolEpoch() const override {
            return backend_.symbolEpochValue;
        }

        bool fetchModules(std::vector<Mem::ModuleInfo>& output) override {
            backend_.allTransactionOperationsGuarded =
                backend_.allTransactionOperationsGuarded &&
                backend_.transactionActive;
            return backend_.fetchModules({}, output);
        }

        bool initializeSymbols(uint64_t moduleBase,
                               int& totalCount) override {
            ++backend_.symbolInitializeCalls;
            ++backend_.symbolEpochValue;
            backend_.activeSymbolModuleBase = moduleBase;
            totalCount = static_cast<int>(backend_.symbolData.size());
            backend_.applySymbolStateChanges();
            return backend_.symbolInitializeSucceeds;
        }

        bool fetchSymbols(size_t offset,
                          size_t limit,
                          std::vector<Mem::SymbolInfo>& symbols,
                          int& totalCount) override {
            ++backend_.symbolFetchCalls;
            backend_.allTransactionOperationsGuarded =
                backend_.allTransactionOperationsGuarded &&
                backend_.transactionActive;
            if (!backend_.symbolFetchSucceeds) {
                return false;
            }
            totalCount = static_cast<int>(backend_.symbolData.size());
            const size_t begin = (std::min)(offset, backend_.symbolData.size());
            const size_t end = begin + (std::min)(
                limit, backend_.symbolData.size() - begin);
            symbols.assign(backend_.symbolData.begin() + begin,
                           backend_.symbolData.begin() + end);
            return true;
        }

        bool findSymbol(uint64_t moduleBase,
                        const std::string& name,
                        uint64_t& address) override {
            ++backend_.symbolFindCalls;
            backend_.allTransactionOperationsGuarded =
                backend_.allTransactionOperationsGuarded &&
                backend_.transactionActive;
            if (!backend_.symbolFindSucceeds ||
                moduleBase != backend_.activeSymbolModuleBase) {
                return false;
            }
            const auto found = std::find_if(
                backend_.symbolData.begin(), backend_.symbolData.end(),
                [&](const Mem::SymbolInfo& symbol) {
                    return symbol.name == name;
                });
            if (found == backend_.symbolData.end()) {
                return false;
            }
            address = found->address;
            return true;
        }

    private:
        FakeBackend& backend_;
        std::unique_lock<std::timed_mutex> lock_;
    };

    void applySymbolStateChanges() {
        if (changeTargetAfterSymbol) {
            target.processRevision += 2;
        }
        if (changeGenerationAfterSymbol) {
            ++generation;
        }
    }

    uint64_t symbolEpoch() const override {
        return symbolEpochValue;
    }

    std::unique_ptr<Mem::IMemSymbolTransaction> beginSymbolTransaction(
        const Mem::OperationContext& context) override {
        ++beginSymbolTransactionCalls;
        if (!context.target || context.connectionGeneration != generation ||
            targetSnapshot() != *context.target) {
            return nullptr;
        }
        return std::make_unique<SymbolTransaction>(*this);
    }

    Mem::BreakpointMutationBackendResult breakpointMutation(
        uint64_t address) {
        lastBreakpointAddress = address;
        if (breakpointDelayMs > 0) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(breakpointDelayMs));
        }
        if (cancelDuringBreakpoint) {
            cancelDuringBreakpoint->store(true, std::memory_order_release);
        }
        if (changeTargetAfterBreakpoint) {
            target.processRevision += 2;
        }
        if (changeGenerationAfterBreakpoint) {
            ++generation;
        }
        return Mem::BreakpointMutationBackendResult{
            breakpointRequestStarted,
            breakpointResponseReceived,
            breakpointApplied,
        };
    }

    Mem::BreakpointMutationBackendResult setBreakpoint(
        const Mem::OperationContext&,
        uint64_t address,
        Mem::BreakpointAccess access,
        uint32_t size) override {
        ++breakpointSetCalls;
        lastBreakpointAccess = access;
        lastBreakpointSize = size;
        return breakpointMutation(address);
    }

    Mem::BreakpointMutationBackendResult removeBreakpoint(
        const Mem::OperationContext&, uint64_t address) override {
        ++breakpointRemoveCalls;
        return breakpointMutation(address);
    }

    Mem::BreakpointMutationBackendResult suspendBreakpoint(
        const Mem::OperationContext&, uint64_t address) override {
        ++breakpointSuspendCalls;
        return breakpointMutation(address);
    }

    Mem::BreakpointMutationBackendResult resumeBreakpoint(
        const Mem::OperationContext&, uint64_t address) override {
        ++breakpointResumeCalls;
        return breakpointMutation(address);
    }

    bool fetchBreakpointHitBatch(
        const Mem::OperationContext&,
        uint64_t address,
        size_t limit,
        std::vector<Mem::BreakpointHit>& hits,
        size_t& total) override {
        ++breakpointHitBatchCalls;
        lastBreakpointAddress = address;
        if (!breakpointHitsSucceed) {
            return false;
        }
        total = breakpointHitsData.size();
        const size_t count = (std::min)(limit, total);
        hits.assign(breakpointHitsData.end() - count,
                    breakpointHitsData.end());
        if (changeTargetAfterBreakpointHits) {
            target.processRevision += 2;
        }
        if (changeGenerationAfterBreakpointHits) {
            ++generation;
        }
        return true;
    }

    bool readMemory(const Mem::OperationContext&,
                    Mem::MemoryReadChannel channel,
                    uint64_t address,
                    uint32_t size,
                    std::vector<unsigned char>& output) override {
        ++readCalls;
        lastReadChannel = channel;
        lastReadAddress = address;
        lastReadSize = size;
        if (!readMemorySucceeds) {
            return false;
        }
        const auto pointer = pointerMemory.find(address);
        if (size == sizeof(uint64_t) && pointer != pointerMemory.end()) {
            output.resize(sizeof(uint64_t));
            for (size_t i = 0; i < sizeof(uint64_t); ++i) {
                output[i] = static_cast<unsigned char>(
                    (pointer->second >> (i * 8u)) & 0xFFu);
            }
        } else {
            const size_t resultSize = (std::min)(
                static_cast<size_t>(size), memory.size());
            output.assign(memory.begin(), memory.begin() + resultSize);
        }
        if (changeTargetAfterRead) {
            target.processRevision += 2;
        }
        if (changeGenerationAfterRead) {
            ++generation;
        }
        return true;
    }

    Mem::MemoryWriteBackendResult writeMemory(
        const Mem::OperationContext&,
        uint64_t address,
        const std::vector<unsigned char>& bytes) override {
        ++writeCalls;
        lastWriteAddress = address;
        lastWriteBytes = bytes;
        if (writeDelayMs > 0) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(writeDelayMs));
        }
        if (cancelDuringWrite) {
            cancelDuringWrite->store(true, std::memory_order_release);
        }
        if (changeTargetAfterWrite) {
            target.processRevision += 2;
        }
        if (changeGenerationAfterWrite) {
            ++generation;
        }

        Mem::MemoryWriteBackendResult result;
        result.requestStarted = writeRequestStarted;
        result.responseReceived = writeResponseReceived;
        result.writtenBytes = writeReportedBytes >= 0
            ? writeReportedBytes
            : static_cast<int32_t>(bytes.size());
        return result;
    }

    Mem::FreezeMutationBackendResult freezeMutation(
        Mem::FreezeAction action,
        uint64_t address,
        const std::vector<unsigned char>& bytes) {
        lastFreezeAction = action;
        lastFreezeAddress = address;
        lastFreezeBytes = bytes;
        if (freezeDelayMs > 0) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(freezeDelayMs));
        }
        if (cancelDuringFreeze) {
            cancelDuringFreeze->store(true, std::memory_order_release);
        }
        if (changeTargetAfterFreeze) {
            target.processRevision += 2;
        }
        if (changeGenerationAfterFreeze) {
            ++generation;
        }
        return {freezeRequestStarted, freezeResponseReceived, freezeApplied};
    }

    Mem::FreezeMutationBackendResult freezeAdd(
        const Mem::OperationContext&,
        uint64_t address,
        const std::vector<unsigned char>& bytes) override {
        ++freezeAddCalls;
        return freezeMutation(Mem::FreezeAction::Add, address, bytes);
    }

    Mem::FreezeMutationBackendResult freezeUpdate(
        const Mem::OperationContext&,
        uint64_t address,
        const std::vector<unsigned char>& bytes) override {
        ++freezeUpdateCalls;
        return freezeMutation(Mem::FreezeAction::Update, address, bytes);
    }

    Mem::FreezeMutationBackendResult freezeRemove(
        const Mem::OperationContext&, uint64_t address) override {
        ++freezeRemoveCalls;
        return freezeMutation(Mem::FreezeAction::Remove, address, {});
    }

    Mem::FreezeMutationBackendResult freezeClear(
        const Mem::OperationContext&) override {
        ++freezeClearCalls;
        return freezeMutation(Mem::FreezeAction::Clear, 0, {});
    }
};

void testDriverInitializationAndSecretRedaction() {
    FakeBackend backend;
    Mem::MemService service(backend);
    const Mem::OperationContext context = service.captureContext(false);

    auto empty = service.initializeDriver(context, {});
    expect(!empty.ok() &&
               empty.error().code == Mem::ErrorCode::InvalidArgument &&
               backend.driverCalls == 0,
           "empty driver cards must fail before reaching the backend");

    const std::string secret = "test-card-secret-7f3a";
    auto completed = service.initializeDriver(
        context, Mem::DriverInitializeRequest{secret});
    expect(completed.ok() && backend.lastDriverCard == secret &&
               backend.lastDriverContextGeneration == 1 &&
               completed.value().connectionGeneration == 1 &&
               !completed.value().completedAfterCancelRequest,
           "confirmed driver initialization should preserve its connection receipt");

    backend.driverRequestStarted = false;
    backend.driverResponseReceived = false;
    auto unsent = service.initializeDriver(
        context, Mem::DriverInitializeRequest{secret});
    expect(!unsent.ok() &&
               unsent.error().code == Mem::ErrorCode::ProtocolError &&
               unsent.error().retryable,
           "unsent driver initialization should remain retryable");

    backend.driverRequestStarted = true;
    auto unknown = service.initializeDriver(
        context, Mem::DriverInitializeRequest{secret});
    expect(!unknown.ok() &&
               unknown.error().code == Mem::ErrorCode::CompletionUnknown &&
               !unknown.error().retryable,
           "sent driver initialization without a response must be completion_unknown");

    backend.driverResponseReceived = true;
    backend.driverAccepted = false;
    backend.driverMessage = "authorization rejected";
    auto rejected = service.initializeDriver(
        context, Mem::DriverInitializeRequest{secret});
    expect(!rejected.ok() &&
               rejected.error().code == Mem::ErrorCode::PermissionDenied &&
               !rejected.error().retryable,
           "server rejection must be a confirmed non-retryable failure");

    backend.driverAccepted = true;
    backend.driverMessage = "initialized";
    backend.cancelDuringDriver = std::make_shared<std::atomic<bool>>(false);
    Mem::OperationContext cancelledContext = context;
    cancelledContext.cancellation = backend.cancelDuringDriver;
    auto cancelledAfterCompletion = service.initializeDriver(
        cancelledContext, Mem::DriverInitializeRequest{secret});
    expect(cancelledAfterCompletion.ok() &&
               cancelledAfterCompletion.value().completedAfterCancelRequest,
           "confirmed initialization must retain a late cancellation marker");

    backend.cancelDuringDriver.reset();
    backend.driverDelayMs = 30;
    Mem::OperationContext deadlineContext = context;
    deadlineContext.deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(10);
    auto completedAfterDeadline = service.initializeDriver(
        deadlineContext, Mem::DriverInitializeRequest{secret});
    expect(completedAfterDeadline.ok() &&
               completedAfterDeadline.value().completedAfterDeadline,
           "confirmed initialization must retain a late deadline marker");

    backend.driverDelayMs = 0;
    backend.changeGenerationAfterDriver = true;
    auto replacedConnection = service.initializeDriver(
        context, Mem::DriverInitializeRequest{secret});
    expect(!replacedConnection.ok() &&
               replacedConnection.error().code ==
                   Mem::ErrorCode::CompletionUnknown,
           "confirmed initialization on a replaced connection must be completion_unknown");

    FakeBackend adapterBackend;
    Mem::MemService adapterService(adapterBackend);
    AI::AgentMemTools tools(adapterService);
    const json adapterResult = json::parse(tools.driverInitialize(
        std::string("{\"card\":\"") + secret + "\"}",
        adapterService.captureContext(false)));
    expect(adapterResult.at("success").get<bool>() &&
               adapterResult.at("completion") == "completed" &&
               adapterResult.dump().find(secret) == std::string::npos,
           "driver adapter should return a structured receipt without echoing the card");

    AI::ToolCall call = toolCall(
        "driver-secret", "driver_initialize",
        std::string("{\"card\":\"") + secret + "\"}");
    AI::applyToolCallRedaction(call);
    expect(call.arguments.find(secret) != std::string::npos &&
               AI::toolCallArgumentsForDisplay(call).find(secret) ==
                   std::string::npos &&
               AI::toolCallArgumentsForDisplay(call).find("[REDACTED]") !=
                   std::string::npos,
           "execution arguments should remain available while storage/display arguments are redacted");

    AI::ToolExecutor::getInstance().registerTool(
        "driver_initialize", "driver test", "{}", AI::ToolSafety::Write,
        [](const std::string&, const Mem::OperationContext&) {
            return std::string(R"({"success":true})");
        },
        AI::ToolTargetPolicy::None);
    AI::AgentRunner runner;
    AI::AgentRunner::Config config;
    auto awaiting = runner.beginToolCalls({call}, config);
    expect(awaiting.kind ==
               AI::AgentRunner::OutcomeKind::NeedsConfirmation,
           "driver initialization must remain write-classified");
    auto denied = runner.resumeDenied(config);
    expect(!denied.messages.empty() &&
               denied.messages.front().content.find(secret) ==
                   std::string::npos &&
               denied.messages.front().content.find("[REDACTED]") !=
                   std::string::npos,
           "denied-tool audit output must not contain the driver card");
}

void testAddressContract() {
    const auto parsed = Mem::parseAddress(" 0x7ff0 ");
    expect(parsed.ok() && parsed.value() == 0x7ff0,
           "explicit hexadecimal address should parse");
    expect(Mem::formatAddress(parsed.value()) == "0x7FF0",
           "address should use canonical uppercase formatting");

    const auto missingPrefix = Mem::parseAddress("7FF0");
    expect(!missingPrefix.ok() &&
               missingPrefix.error().code == Mem::ErrorCode::InvalidArgument,
           "canonical address must require 0x prefix");

    const auto compatible = Mem::parseAddress("7FF0", false);
    expect(compatible.ok() && compatible.value() == 0x7ff0,
           "low-level compatibility mode should accept unprefixed hexadecimal addresses");

    const auto overflow = Mem::parseAddress("0x10000000000000000");
    expect(!overflow.ok(), "uint64 address overflow must fail");
}

void testScalarValueCodec() {
    const auto intAlias = Mem::parseScalarType(" int32 ");
    expect(intAlias.ok() && intAlias.value() == Mem::ScalarType::Dword,
           "int32 should normalize to dword");
    expect(!Mem::parseScalarType("vector128").ok(),
           "unsupported scalar types must fail");

    const auto dword =
        Mem::encodeScalarValue(Mem::ScalarType::Dword, "0x12345678");
    expect(dword.ok() &&
               dword.value() ==
                   std::vector<unsigned char>({0x78, 0x56, 0x34, 0x12}),
           "dword encoding should be little endian");

    const auto negative =
        Mem::encodeScalarValue(Mem::ScalarType::Word, "-1");
    expect(negative.ok() &&
               negative.value() ==
                   std::vector<unsigned char>({0xFF, 0xFF}),
           "negative integer encoding should preserve two's complement bits");
    expect(!Mem::encodeScalarValue(Mem::ScalarType::Byte, "256").ok(),
           "integer overflow must be rejected");
    expect(!Mem::encodeScalarValue(Mem::ScalarType::Byte, "-129").ok(),
           "negative integer underflow must be rejected");

    const auto qword = Mem::encodeScalarValue(
        Mem::ScalarType::Qword, "18446744073709551615");
    expect(qword.ok() && qword.value().size() == 8 &&
               qword.value().front() == 0xFF &&
               qword.value().back() == 0xFF,
           "qword strings should preserve the full uint64 range");

    const auto encodedFloat =
        Mem::encodeScalarValue(Mem::ScalarType::Float, "1.5");
    expect(encodedFloat.ok() &&
               encodedFloat.value() ==
                   std::vector<unsigned char>({0x00, 0x00, 0xC0, 0x3F}),
           "float encoding should preserve IEEE-754 bytes");
    expect(!Mem::encodeScalarValue(Mem::ScalarType::Double, "nan").ok(),
           "non-finite write values must be rejected");

    const auto decoded =
        Mem::decodeScalarValue(Mem::ScalarType::Dword, dword.value());
    expect(decoded.ok() && decoded.value().integerValue == 0x12345678 &&
               Mem::formatScalarValue(decoded.value()) == "305419896",
           "integer decoding should expose exact decimal bits");
    const auto decodedFloat =
        Mem::decodeScalarValue(Mem::ScalarType::Float, encodedFloat.value());
    expect(decodedFloat.ok() && decodedFloat.value().floatingPoint &&
               std::fabs(decodedFloat.value().floatingValue - 1.5) < 0.0001,
           "float decoding should preserve the scalar value");
    expect(!Mem::decodeScalarValue(
                Mem::ScalarType::Qword, {0x01, 0x02}).ok(),
           "decode must reject a byte-count/type mismatch");
}

void testStatusAndConnectionGeneration() {
    FakeBackend backend;
    Mem::MemService service(backend);

    const auto context = service.captureContext(false);
    const auto response = service.status(context);
    expect(response.ok(), "connected status should succeed");
    expect(response.value().serverVersion == 101,
           "status should include server version");
    expect(response.value().architectureName == "Kernel",
           "status should include architecture");
    expect(response.value().target.pid == 42,
           "status should include target snapshot");

    const auto staleContext = service.captureContext(false);
    ++backend.generation;
    const auto stale = service.status(staleContext);
    expect(!stale.ok() &&
               stale.error().code == Mem::ErrorCode::ConnectionChanged,
           "stale connection generation must be rejected");
}

void testConnectionLifecycleService() {
    FakeBackend backend;
    Mem::MemService service(backend);

    Mem::ConnectRequest request;
    request.host = "127.0.0.1";
    request.port = 52736;
    const Mem::OperationContext initial = service.captureContext(false);

    Mem::ConnectRequest invalid = request;
    invalid.host.clear();
    const auto emptyHost = service.connect(initial, invalid);
    expect(!emptyHost.ok() &&
               emptyHost.error().code == Mem::ErrorCode::InvalidArgument &&
               backend.connectCalls == 0,
           "empty connection host must fail before backend access");

    invalid = request;
    invalid.port = 65534;
    const auto invalidPort = service.connect(initial, invalid);
    expect(!invalidPort.ok() &&
               invalidPort.error().code == Mem::ErrorCode::InvalidArgument &&
               backend.connectCalls == 0,
           "base port must leave room for all three Android ports");

    const auto connected = service.connect(initial, request);
    expect(connected.ok() && connected.value().connected &&
               connected.value().connectionGeneration == 2 &&
               backend.connectCalls == 1 &&
               backend.lastConnectHost == request.host &&
               backend.lastConnectPort == request.port,
           "connect should return the new connection generation");
    const auto connectedSnapshot = service.connectionSnapshot();
    expect(connectedSnapshot.connected &&
               !connectedSnapshot.connectionPoisoned &&
               connectedSnapshot.connectionGeneration == 2 &&
               !backend.targetSnapshot().isAttached(),
           "connect should publish a usable detached connection snapshot");

    const auto staleDisconnect = service.disconnect(initial);
    expect(!staleDisconnect.ok() &&
               staleDisconnect.error().code ==
                   Mem::ErrorCode::ConnectionChanged &&
               backend.disconnectCalls == 0,
           "disconnect must reject a stale connection generation");

    const auto disconnected = service.disconnect(
        service.captureContext(false));
    expect(disconnected.ok() && disconnected.value().wasConnected &&
               disconnected.value().connectionGeneration == 3 &&
               backend.disconnectCalls == 1 && !backend.connected,
           "disconnect should invalidate the active connection generation");

    const auto disconnectedAgain = service.disconnect(
        service.captureContext(false));
    expect(disconnectedAgain.ok() &&
               !disconnectedAgain.value().wasConnected &&
               disconnectedAgain.value().connectionGeneration == 3,
           "disconnect should be idempotent while already disconnected");

    backend.connectSucceeds = false;
    const auto failedConnect = service.connect(
        service.captureContext(false), request);
    expect(!failedConnect.ok() &&
               failedConnect.error().code == Mem::ErrorCode::ProtocolError &&
               failedConnect.error().retryable &&
               backend.connectCalls == 2 && !backend.connected &&
               backend.generation == 4,
           "failed three-port connect should remain detached on a new generation");
}

void testProcessPaginationAndOpen() {
    FakeBackend backend;
    Mem::MemService service(backend);

    Mem::ProcessListRequest request;
    request.filter = "GAME";
    request.limit = 1;
    const auto page = service.listProcesses(service.captureContext(false), request);
    expect(page.ok(), "filtered process list should succeed");
    expect(page.value().total == 2 && page.value().items.size() == 1,
           "process filter and page limit should be applied");
    expect(page.value().nextOffset == 1,
           "truncated process page should expose next offset");

    Mem::ProcessListRequest invalidRequest;
    invalidRequest.limit = Mem::kMaxProcessPageSize + 1;
    const auto invalid = service.listProcesses(
        service.captureContext(false), invalidRequest);
    expect(!invalid.ok() &&
               invalid.error().code == Mem::ErrorCode::InvalidArgument,
           "oversized process page must fail validation");

    Mem::OpenProcessRequest openRequest;
    openRequest.pid = 84;
    const auto opened = service.openProcess(
        service.captureContext(false), openRequest);
    expect(opened.ok() && opened.value().target.pid == 84,
           "process open should return the new target snapshot");
    expect(opened.value().name == "com.example.game.helper",
           "process open result should include the resolved name");
    expect(backend.selectedName == "com.example.game.helper",
           "process open should resolve an omitted process name");

    backend.changeGenerationDuringOpen = true;
    openRequest.pid = 42;
    const auto changed = service.openProcess(
        service.captureContext(false), openRequest);
    expect(!changed.ok() &&
               changed.error().code == Mem::ErrorCode::ConnectionChanged,
           "connection replacement during process open must be rejected");
}

void testMemoryReadTargetValidation() {
    FakeBackend backend;
    Mem::MemService service(backend);

    Mem::MemoryReadRequest request{0x1000, 4};
    const auto response = service.readMemory(
        service.captureContext(true), request);
    expect(response.ok() && response.value().bytes == backend.memory,
           "bounded memory read should return backend bytes");

    backend.changeTargetAfterRead = true;
    const auto targetChanged = service.readMemory(
        service.captureContext(true), request);
    expect(!targetChanged.ok() &&
               targetChanged.error().code == Mem::ErrorCode::TargetChanged,
           "memory bytes must be discarded when target changes in flight");
    backend.changeTargetAfterRead = false;

    backend.changeGenerationAfterRead = true;
    const auto connectionChanged = service.readMemory(
        service.captureContext(true), request);
    expect(!connectionChanged.ok() &&
               connectionChanged.error().code ==
                   Mem::ErrorCode::ConnectionChanged,
           "memory bytes must be discarded when connection changes in flight");
    backend.changeGenerationAfterRead = false;

    backend.target = {};
    const auto noTarget = service.readMemory(
        service.captureContext(true), request);
    expect(!noTarget.ok() &&
               noTarget.error().code == Mem::ErrorCode::NoTarget,
           "memory read without an attached target must fail");

    backend.target = {42, 420, 8, backend.generation};
    request.address = (std::numeric_limits<uint64_t>::max)() - 1;
    request.size = 4;
    const auto overflow = service.readMemory(
        service.captureContext(true), request);
    expect(!overflow.ok() &&
               overflow.error().code == Mem::ErrorCode::InvalidArgument,
           "overflowing memory range must fail validation");

    request = {0x1000, 4};
    Mem::OperationContext cancelled = service.captureContext(true);
    cancelled.cancellation = std::make_shared<std::atomic<bool>>(true);
    const auto cancelledRead = service.readMemory(cancelled, request);
    expect(!cancelledRead.ok() &&
               cancelledRead.error().code == Mem::ErrorCode::CancelRequested,
           "cancelled operation must stop before backend access");

    Mem::OperationContext expired = service.captureContext(true);
    expired.deadline = std::chrono::steady_clock::now();
    const auto expiredRead = service.readMemory(expired, request);
    expect(!expiredRead.ok() &&
               expiredRead.error().code == Mem::ErrorCode::Timeout,
           "expired operation deadline must fail before backend access");
}

void testMemoryBatchService() {
    FakeBackend backend;
    backend.memory = {0x10, 0x20, 0x30, 0x40};
    Mem::MemService service(backend);

    Mem::MemoryBatchReadRequest request;
    request.channel = Mem::MemoryReadChannel::Background;
    request.items = {
        Mem::MemoryReadRequest{0x1000, 2},
        Mem::MemoryReadRequest{0x2000, 4},
    };
    const auto batch = service.readMemoryBatch(
        service.captureContext(true), request);
    expect(batch.ok() && batch.value().items.size() == 2 &&
               batch.value().items[0].address == 0x1000 &&
               batch.value().items[0].bytes ==
                   std::vector<unsigned char>({0x10, 0x20}) &&
               batch.value().items[1].address == 0x2000 &&
               batch.value().items[1].bytes == backend.memory &&
               batch.value().target == backend.targetSnapshot(),
           "memory batch should preserve request order, sizes and target");
    expect(backend.batchReadCalls == 1 &&
               backend.lastTransactionChannel ==
                   Mem::MemoryReadChannel::Background &&
               backend.lastReadChannel ==
                   Mem::MemoryReadChannel::Background,
           "memory batch should use the requested read channel in one transaction");

    const int transactionsAfterSuccess = backend.beginReadTransactionCalls;
    Mem::MemoryBatchReadRequest invalid;
    const auto empty = service.readMemoryBatch(
        service.captureContext(true), invalid);
    expect(!empty.ok() &&
               empty.error().code == Mem::ErrorCode::InvalidArgument &&
               backend.beginReadTransactionCalls == transactionsAfterSuccess,
           "empty memory batch must fail before transaction acquisition");

    invalid.items = {Mem::MemoryReadRequest{0x1000, 0}};
    const auto zeroSize = service.readMemoryBatch(
        service.captureContext(true), invalid);
    expect(!zeroSize.ok() &&
               zeroSize.error().code == Mem::ErrorCode::InvalidArgument &&
               backend.beginReadTransactionCalls == transactionsAfterSuccess,
           "zero-sized memory batch item must fail validation");

    backend.batchReturnPartialSet = true;
    const auto partialSet = service.readMemoryBatch(
        service.captureContext(true), request);
    expect(!partialSet.ok() &&
               partialSet.error().code == Mem::ErrorCode::ProtocolError &&
               !partialSet.error().retryable,
           "incomplete memory batch result set must be rejected");
    backend.batchReturnPartialSet = false;

    backend.batchReturnShortBlock = true;
    const auto shortBlock = service.readMemoryBatch(
        service.captureContext(true), request);
    expect(!shortBlock.ok() &&
               shortBlock.error().code == Mem::ErrorCode::ProtocolError &&
               !shortBlock.error().retryable,
           "short memory batch block must be rejected");
    backend.batchReturnShortBlock = false;

    backend.batchReadSucceeds = false;
    const auto failed = service.readMemoryBatch(
        service.captureContext(true), request);
    expect(!failed.ok() &&
               failed.error().code == Mem::ErrorCode::ProtocolError &&
               failed.error().retryable,
           "batch transport failure may be retried only while context is current");
    backend.batchReadSucceeds = true;

    backend.changeTargetAfterBatch = true;
    const auto targetChanged = service.readMemoryBatch(
        service.captureContext(true), request);
    expect(!targetChanged.ok() &&
               targetChanged.error().code == Mem::ErrorCode::TargetChanged,
           "memory batch must discard results after an in-flight target change");
}

void testMemoryWriteCompletionContract() {
    FakeBackend backend;
    Mem::MemService service(backend);
    Mem::MemoryWriteRequest request;
    request.address = 0x2000;
    request.bytes = {0x90, 0x90, 0xC0, 0x03, 0x5F, 0xD6};

    const auto written = service.writeMemory(
        service.captureContext(true), request);
    expect(written.ok() && written.value().writtenBytes == request.bytes.size(),
           "confirmed memory write should return a receipt");
    expect(backend.lastWriteAddress == request.address &&
               backend.lastWriteBytes == request.bytes,
           "memory write should pass the exact address and bytes to backend");

    const int callsAfterSuccess = backend.writeCalls;
    Mem::OperationContext cancelled = service.captureContext(true);
    cancelled.cancellation = std::make_shared<std::atomic<bool>>(true);
    const auto cancelledBeforeSend = service.writeMemory(cancelled, request);
    expect(!cancelledBeforeSend.ok() &&
               cancelledBeforeSend.error().code ==
                   Mem::ErrorCode::CancelRequested &&
               backend.writeCalls == callsAfterSuccess,
           "cancelled write must stop before backend send");

    Mem::OperationContext cancelledDuringSend = service.captureContext(true);
    cancelledDuringSend.cancellation =
        std::make_shared<std::atomic<bool>>(false);
    backend.cancelDuringWrite = cancelledDuringSend.cancellation;
    const auto completedAfterCancel = service.writeMemory(
        cancelledDuringSend, request);
    expect(completedAfterCancel.ok() &&
               completedAfterCancel.value().completedAfterCancelRequest,
           "confirmed write must report completion after a cancel request");
    backend.cancelDuringWrite.reset();

    Mem::OperationContext completedAfterDeadlineContext =
        service.captureContext(true);
    completedAfterDeadlineContext.deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
    backend.writeDelayMs = 40;
    const auto completedAfterDeadline = service.writeMemory(
        completedAfterDeadlineContext, request);
    expect(completedAfterDeadline.ok() &&
               completedAfterDeadline.value().completedAfterDeadline,
           "confirmed write must report completion after its deadline");
    backend.writeDelayMs = 0;

    backend.writeResponseReceived = false;
    backend.writeRequestStarted = true;
    const auto completionUnknown = service.writeMemory(
        service.captureContext(true), request);
    expect(!completionUnknown.ok() &&
               completionUnknown.error().code ==
                   Mem::ErrorCode::CompletionUnknown &&
               !completionUnknown.error().retryable,
           "sent write without a response must be completion_unknown");

    backend.writeRequestStarted = false;
    const auto notSent = service.writeMemory(
        service.captureContext(true), request);
    expect(!notSent.ok() &&
               notSent.error().code == Mem::ErrorCode::ProtocolError &&
               notSent.error().retryable,
           "write known not to be sent may report a retryable transport error");

    backend.writeRequestStarted = true;
    backend.writeResponseReceived = true;
    backend.writeReportedBytes = 2;
    const auto partial = service.writeMemory(
        service.captureContext(true), request);
    expect(!partial.ok() &&
               partial.error().code == Mem::ErrorCode::ProtocolError &&
               !partial.error().retryable,
           "confirmed partial write must fail without automatic retry");

    backend.writeReportedBytes = -1;
    backend.changeTargetAfterWrite = true;
    const auto targetChanged = service.writeMemory(
        service.captureContext(true), request);
    expect(!targetChanged.ok() &&
               targetChanged.error().code ==
                   Mem::ErrorCode::CompletionUnknown,
           "confirmed write must not be bound to a target that changed in flight");
    backend.changeTargetAfterWrite = false;

    backend.changeGenerationAfterWrite = true;
    const auto connectionChanged = service.writeMemory(
        service.captureContext(true), request);
    expect(!connectionChanged.ok() &&
               connectionChanged.error().code ==
                   Mem::ErrorCode::CompletionUnknown,
           "confirmed write on a replaced connection must be completion_unknown");
    backend.changeGenerationAfterWrite = false;

    request.address = (std::numeric_limits<uint64_t>::max)() - 1;
    request.bytes = {1, 2, 3, 4};
    const auto overflow = service.writeMemory(
        service.captureContext(true), request);
    expect(!overflow.ok() &&
               overflow.error().code == Mem::ErrorCode::InvalidArgument,
           "overflowing memory write range must fail validation");
}

void testFreezeMutationContract() {
    FakeBackend backend;
    Mem::MemService service(backend);
    Mem::FreezeValueRequest valueRequest;
    valueRequest.address = 0x4000;
    valueRequest.bytes = {0x78, 0x56, 0x34, 0x12};

    const auto added = service.freezeAdd(
        service.captureContext(true), valueRequest);
    expect(added.ok() && added.value().action == Mem::FreezeAction::Add &&
               added.value().address == valueRequest.address &&
               added.value().valueSize == valueRequest.bytes.size() &&
               added.value().target == backend.targetSnapshot() &&
               backend.freezeAddCalls == 1 &&
               backend.lastFreezeBytes == valueRequest.bytes,
           "confirmed freeze add should return an exact target-bound receipt");

    valueRequest.bytes = {0xAA, 0xBB};
    const auto updated = service.freezeUpdate(
        service.captureContext(true), valueRequest);
    expect(updated.ok() &&
               updated.value().action == Mem::FreezeAction::Update &&
               updated.value().valueSize == 2 &&
               backend.freezeUpdateCalls == 1 &&
               backend.lastFreezeAction == Mem::FreezeAction::Update,
           "confirmed freeze update should preserve its action and value size");

    const auto removed = service.freezeRemove(
        service.captureContext(true),
        Mem::FreezeAddressRequest{valueRequest.address});
    expect(removed.ok() &&
               removed.value().action == Mem::FreezeAction::Remove &&
               removed.value().valueSize == 0 &&
               backend.freezeRemoveCalls == 1,
           "confirmed freeze remove should return a zero-sized receipt");

    const auto cleared = service.freezeClear(service.captureContext(true));
    expect(cleared.ok() &&
               cleared.value().action == Mem::FreezeAction::Clear &&
               cleared.value().address == 0 &&
               backend.freezeClearCalls == 1,
           "confirmed freeze clear should use the target-bound clear action");

    const int callsBeforeInvalid = backend.freezeAddCalls;
    Mem::FreezeValueRequest invalidValue{0x4000, {}};
    const auto empty = service.freezeAdd(
        service.captureContext(true), invalidValue);
    expect(!empty.ok() &&
               empty.error().code == Mem::ErrorCode::InvalidArgument &&
               backend.freezeAddCalls == callsBeforeInvalid,
           "empty freeze value must fail before backend access");

    invalidValue.bytes.assign(Mem::kMaxFreezeValueBytes + 1, 0x11);
    const auto oversized = service.freezeAdd(
        service.captureContext(true), invalidValue);
    expect(!oversized.ok() &&
               oversized.error().code == Mem::ErrorCode::InvalidArgument &&
               backend.freezeAddCalls == callsBeforeInvalid,
           "oversized freeze value must fail before backend access");

    const int removesBeforeInvalid = backend.freezeRemoveCalls;
    const auto zeroAddress = service.freezeRemove(
        service.captureContext(true), Mem::FreezeAddressRequest{});
    expect(!zeroAddress.ok() &&
               zeroAddress.error().code == Mem::ErrorCode::InvalidArgument &&
               backend.freezeRemoveCalls == removesBeforeInvalid,
           "zero freeze address must fail before backend access");

    backend.freezeResponseReceived = false;
    backend.freezeRequestStarted = true;
    const auto unknown = service.freezeAdd(
        service.captureContext(true), valueRequest);
    expect(!unknown.ok() &&
               unknown.error().code == Mem::ErrorCode::CompletionUnknown &&
               !unknown.error().retryable,
           "sent freeze mutation without response must be completion_unknown");

    backend.freezeRequestStarted = false;
    const auto unsent = service.freezeAdd(
        service.captureContext(true), valueRequest);
    expect(!unsent.ok() &&
               unsent.error().code == Mem::ErrorCode::ProtocolError &&
               unsent.error().retryable,
           "freeze mutation known not to be sent may be retried");

    backend.freezeRequestStarted = true;
    backend.freezeResponseReceived = true;
    backend.freezeApplied = false;
    const auto rejected = service.freezeAdd(
        service.captureContext(true), valueRequest);
    expect(!rejected.ok() &&
               rejected.error().code == Mem::ErrorCode::ProtocolError &&
               !rejected.error().retryable,
           "server-rejected freeze mutation must not be retried automatically");

    backend.freezeApplied = true;
    Mem::OperationContext cancelled = service.captureContext(true);
    cancelled.cancellation = std::make_shared<std::atomic<bool>>(false);
    backend.cancelDuringFreeze = cancelled.cancellation;
    const auto completedAfterCancel = service.freezeAdd(cancelled, valueRequest);
    expect(completedAfterCancel.ok() &&
               completedAfterCancel.value().completedAfterCancelRequest,
           "confirmed freeze mutation should report a late cancel request");
    backend.cancelDuringFreeze.reset();

    Mem::OperationContext deadline = service.captureContext(true);
    deadline.deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(20);
    backend.freezeDelayMs = 40;
    const auto completedAfterDeadline = service.freezeAdd(deadline, valueRequest);
    expect(completedAfterDeadline.ok() &&
               completedAfterDeadline.value().completedAfterDeadline,
           "confirmed freeze mutation should report completion after deadline");
    backend.freezeDelayMs = 0;

    backend.changeTargetAfterFreeze = true;
    const auto targetChanged = service.freezeAdd(
        service.captureContext(true), valueRequest);
    expect(!targetChanged.ok() &&
               targetChanged.error().code == Mem::ErrorCode::CompletionUnknown,
           "confirmed freeze mutation on a replaced target is completion_unknown");

    FakeBackend generationBackend;
    Mem::MemService generationService(generationBackend);
    generationBackend.changeGenerationAfterFreeze = true;
    const auto generationChanged = generationService.freezeClear(
        generationService.captureContext(true));
    expect(!generationChanged.ok() &&
               generationChanged.error().code ==
                   Mem::ErrorCode::CompletionUnknown,
           "confirmed freeze mutation on a replaced connection is completion_unknown");
}

void testTypedMemoryService() {
    FakeBackend backend;
    Mem::MemService service(backend);
    const Mem::OperationContext targetContext = service.captureContext(true);

    backend.memory = {0x78, 0x56, 0x34, 0x12};
    Mem::ValueReadRequest readRequest;
    readRequest.address = 0x3000;
    readRequest.type = Mem::ScalarType::Dword;
    const auto read = service.readValue(targetContext, readRequest);
    expect(read.ok() && read.value().bytes == backend.memory &&
               backend.lastReadAddress == 0x3000 &&
               backend.lastReadSize == 4,
           "typed read should request the exact scalar byte count");

    backend.memory = {0x01, 0x02};
    const auto partial = service.readValue(
        service.captureContext(true), readRequest);
    expect(!partial.ok() &&
               partial.error().code == Mem::ErrorCode::ProtocolError,
           "typed read must reject a short raw response");

    backend.memory = {0x78, 0x56, 0x34, 0x12};
    backend.changeTargetAfterRead = true;
    const auto changed = service.readValue(
        service.captureContext(true), readRequest);
    expect(!changed.ok() &&
               changed.error().code == Mem::ErrorCode::TargetChanged,
           "typed read must retain raw read target validation");
    backend.changeTargetAfterRead = false;

    const auto encoded =
        Mem::encodeScalarValue(Mem::ScalarType::Word, "0xBEEF");
    expect(encoded.ok(), "typed write fixture should encode");
    Mem::ValueWriteRequest writeRequest;
    writeRequest.address = 0x4000;
    writeRequest.type = Mem::ScalarType::Word;
    writeRequest.bytes = encoded.value();
    const auto written = service.writeValue(
        service.captureContext(true), writeRequest);
    expect(written.ok() && written.value().writtenBytes == 2 &&
               backend.lastWriteAddress == 0x4000 &&
               backend.lastWriteBytes ==
                   std::vector<unsigned char>({0xEF, 0xBE}),
           "typed write should delegate exact encoded bytes to raw write");

    const int callsAfterWrite = backend.writeCalls;
    writeRequest.bytes = {0xEF};
    const auto wrongSize = service.writeValue(
        service.captureContext(true), writeRequest);
    expect(!wrongSize.ok() &&
               wrongSize.error().code == Mem::ErrorCode::InvalidArgument &&
               backend.writeCalls == callsAfterWrite,
           "typed write must reject a type/byte-count mismatch before send");

    writeRequest.bytes = encoded.value();
    backend.writeRequestStarted = true;
    backend.writeResponseReceived = false;
    const auto unknown = service.writeValue(
        service.captureContext(true), writeRequest);
    expect(!unknown.ok() &&
               unknown.error().code == Mem::ErrorCode::CompletionUnknown &&
               !unknown.error().retryable,
           "typed write must preserve raw completion_unknown semantics");
}

void testModuleListAndResolve() {
    FakeBackend backend;
    Mem::MemService service(backend);

    Mem::ModuleListRequest listRequest;
    listRequest.filter = "GAME";
    listRequest.limit = 1;
    const auto page = service.listModules(
        service.captureContext(true), listRequest);
    expect(page.ok() && page.value().total == 2 &&
               page.value().items.size() == 1 &&
               page.value().items.front().name ==
                   "/data/app/libgame.so" &&
               page.value().nextOffset == 1 &&
               page.value().target.pid == backend.target.pid,
           "module list should filter case-insensitively and paginate");

    backend.modules.insert(
        backend.modules.begin(),
        {0x8000, 0x1000, 2, 1, "/data/app/libgame.so"});

    Mem::ModuleResolveRequest resolveRequest;
    resolveRequest.name = "libgame.so";
    const auto resolved = service.resolveModule(
        service.captureContext(true), resolveRequest);
    expect(resolved.ok() && resolved.value().module.base == 0x5000 &&
               resolved.value().module.size == 0x3000 &&
               resolved.value().module.name == "/data/app/libgame.so",
           "module resolve should collapse same-path mappings and use the lowest base");

    resolveRequest.name = "/DATA/APP/LIBGAME.SO";
    const auto segmentedFullName = service.resolveModule(
        service.captureContext(true), resolveRequest);
    expect(segmentedFullName.ok() &&
               segmentedFullName.value().module.base == 0x5000,
           "module resolve should collapse same-path mappings for exact full names");

    backend.modules.push_back(
        {0xA000, 0x1000, 2, 1, "/vendor/lib64/libgame.so"});
    resolveRequest.name = "libgame.so";
    const auto duplicateBaseName = service.resolveModule(
        service.captureContext(true), resolveRequest);
    expect(!duplicateBaseName.ok() &&
               duplicateBaseName.error().code ==
                   Mem::ErrorCode::InvalidArgument &&
               duplicateBaseName.error().message.find("ambiguous") !=
                   std::string::npos,
           "module resolve must preserve ambiguity across distinct full paths");
    backend.modules.pop_back();

    resolveRequest.name = "/SYSTEM/LIB64/LIBC.SO";
    const auto fullName = service.resolveModule(
        service.captureContext(true), resolveRequest);
    expect(fullName.ok() && fullName.value().module.base == 0x1000,
           "module resolve should match full names case-insensitively");

    resolveRequest.name = "libgame";
    const auto ambiguous = service.resolveModule(
        service.captureContext(true), resolveRequest);
    expect(!ambiguous.ok() &&
               ambiguous.error().code == Mem::ErrorCode::InvalidArgument &&
               ambiguous.error().message.find("ambiguous") !=
                   std::string::npos,
           "module resolve must reject ambiguous substrings");

    resolveRequest.name = "libmissing.so";
    const auto missing = service.resolveModule(
        service.captureContext(true), resolveRequest);
    expect(!missing.ok() &&
               missing.error().code == Mem::ErrorCode::InvalidArgument,
           "module resolve must reject a missing module");

    const int fetchCallsBeforeInvalidPage = backend.fetchModuleCalls;
    listRequest.limit = 0;
    const auto invalidPage = service.listModules(
        service.captureContext(true), listRequest);
    expect(!invalidPage.ok() &&
               invalidPage.error().code == Mem::ErrorCode::InvalidArgument &&
               backend.fetchModuleCalls == fetchCallsBeforeInvalidPage,
           "invalid module page limits must fail before backend access");

    listRequest = {};
    backend.modules.front().size = 0;
    const auto invalidModule = service.listModules(
        service.captureContext(true), listRequest);
    expect(!invalidModule.ok() &&
               invalidModule.error().code == Mem::ErrorCode::ProtocolError,
           "module service must reject invalid backend ranges");
    backend.modules.front().size = 0x1000;

    backend.changeTargetAfterModuleFetch = true;
    const auto changed = service.listModules(
        service.captureContext(true), listRequest);
    expect(!changed.ok() &&
               changed.error().code == Mem::ErrorCode::TargetChanged,
           "module list must reject results from a changed target");
    backend.changeTargetAfterModuleFetch = false;

    backend.changeGenerationAfterModuleFetch = true;
    const auto reconnected = service.listModules(
        service.captureContext(true), listRequest);
    expect(!reconnected.ok() &&
               reconnected.error().code == Mem::ErrorCode::ConnectionChanged,
           "module list must reject results from a replaced connection");
    backend.changeGenerationAfterModuleFetch = false;

    backend.fetchModulesSucceeds = false;
    const auto failed = service.listModules(
        service.captureContext(true), listRequest);
    expect(!failed.ok() && failed.error().retryable &&
               failed.error().code == Mem::ErrorCode::ProtocolError,
           "module transport failure should be structured and retryable");
}

void testPointerResolveTransaction() {
    FakeBackend backend;
    Mem::MemService service(backend);
    backend.pointerMemory = {
        {0x5010, 0x6000},
        {0x6020, 0x7000},
        {0x7030, 0x8000},
    };

    Mem::PointerResolveRequest request;
    request.moduleName = "libgame.so";
    request.baseOffset = 0x10;
    request.offsets = {0x20, 0x30};
    request.dereferenceFinal = true;
    const auto resolved = service.resolvePointer(
        service.captureContext(true), request);
    expect(resolved.ok() &&
               resolved.value().module.name == "/data/app/libgame.so" &&
               resolved.value().startAddress == 0x5010 &&
               resolved.value().address == 0x8000 &&
               resolved.value().dereferenceCount == 3 &&
               resolved.value().dereferencedFinal &&
               backend.beginReadTransactionCalls == 1 &&
               backend.transactionFetchCalls == 1 &&
               backend.transactionReadCalls == 3 &&
               backend.allTransactionOperationsGuarded &&
               backend.transactionBlockedCompetitor,
           "pointer resolve must keep module lookup and every read in one transaction");

    const int readsBeforeAddressOnly = backend.transactionReadCalls;
    request.offsets.clear();
    request.dereferenceFinal = false;
    const auto addressOnly = service.resolvePointer(
        service.captureContext(true), request);
    expect(addressOnly.ok() && addressOnly.value().address == 0x5010 &&
               addressOnly.value().dereferenceCount == 0 &&
               backend.transactionReadCalls == readsBeforeAddressOnly,
           "pointer resolve should return module_base + base_offset without reads when requested");

    request.dereferenceFinal = true;
    const auto directPointer = service.resolvePointer(
        service.captureContext(true), request);
    expect(directPointer.ok() && directPointer.value().address == 0x6000 &&
               directPointer.value().dereferenceCount == 1,
           "canonical empty pointer chains should honor deref_final");

    request.moduleName = "libgame";
    const int readsBeforeAmbiguous = backend.transactionReadCalls;
    const auto ambiguous = service.resolvePointer(
        service.captureContext(true), request);
    expect(!ambiguous.ok() &&
               ambiguous.error().code == Mem::ErrorCode::InvalidArgument &&
               backend.transactionReadCalls == readsBeforeAmbiguous,
           "pointer resolve must reject ambiguous modules before reading memory");

    request.moduleName = "libgame.so";
    request.baseOffset = 0x10;
    request.offsets.assign(Mem::kMaxPointerOffsetCount + 1, 0);
    const int transactionsBeforeOversize = backend.beginReadTransactionCalls;
    const auto oversize = service.resolvePointer(
        service.captureContext(true), request);
    expect(!oversize.ok() &&
               oversize.error().code == Mem::ErrorCode::InvalidArgument &&
               backend.beginReadTransactionCalls == transactionsBeforeOversize,
           "oversized pointer chains must fail before acquiring a transaction");

    request.offsets = {2};
    request.dereferenceFinal = false;
    backend.pointerMemory[0x5010] =
        (std::numeric_limits<uint64_t>::max)() - 1;
    const auto pointerOverflow = service.resolvePointer(
        service.captureContext(true), request);
    expect(!pointerOverflow.ok() &&
               pointerOverflow.error().code ==
                   Mem::ErrorCode::InvalidArgument,
           "pointer plus offset overflow must fail deterministically");

    FakeBackend baseOverflowBackend;
    Mem::MemService baseOverflowService(baseOverflowBackend);
    baseOverflowBackend.modules[1].base =
        (std::numeric_limits<uint64_t>::max)() - 0x100;
    baseOverflowBackend.modules[1].size = 0x80;
    request.baseOffset = 0x200;
    request.offsets.clear();
    const auto baseOverflow = baseOverflowService.resolvePointer(
        baseOverflowService.captureContext(true), request);
    expect(!baseOverflow.ok() &&
               baseOverflow.error().code ==
                   Mem::ErrorCode::InvalidArgument &&
               baseOverflowBackend.transactionReadCalls == 0,
           "module base plus base_offset overflow must fail before reading");
    request.baseOffset = 0x10;

    FakeBackend changedTargetBackend;
    Mem::MemService changedTargetService(changedTargetBackend);
    changedTargetBackend.pointerMemory[0x5010] = 0x6000;
    changedTargetBackend.changeTargetAfterModuleFetch = true;
    request.offsets.clear();
    request.dereferenceFinal = true;
    const auto changedTarget = changedTargetService.resolvePointer(
        changedTargetService.captureContext(true), request);
    expect(!changedTarget.ok() &&
               changedTarget.error().code == Mem::ErrorCode::TargetChanged,
           "pointer transaction results must be rejected after target change");

    FakeBackend changedGenerationBackend;
    Mem::MemService changedGenerationService(changedGenerationBackend);
    changedGenerationBackend.pointerMemory[0x5010] = 0x6000;
    changedGenerationBackend.changeGenerationAfterModuleFetch = true;
    const auto changedGeneration = changedGenerationService.resolvePointer(
        changedGenerationService.captureContext(true), request);
    expect(!changedGeneration.ok() &&
               changedGeneration.error().code ==
                   Mem::ErrorCode::ConnectionChanged,
           "pointer transaction must stop when its connection generation changes");

    FakeBackend cancelledBackend;
    Mem::MemService cancelledService(cancelledBackend);
    cancelledBackend.pointerMemory = {
        {0x5010, 0x6000},
        {0x6020, 0x7000},
    };
    Mem::OperationContext cancelledContext =
        cancelledService.captureContext(true);
    cancelledContext.cancellation =
        std::make_shared<std::atomic<bool>>(false);
    cancelledBackend.transactionCancellation = cancelledContext.cancellation;
    cancelledBackend.cancelAfterTransactionReads = 1;
    request.offsets = {0x20, 0x30};
    request.dereferenceFinal = false;
    const auto cancelled = cancelledService.resolvePointer(
        cancelledContext, request);
    expect(!cancelled.ok() &&
               cancelled.error().code == Mem::ErrorCode::CancelRequested &&
               cancelledBackend.transactionReadCalls == 1,
           "pointer transaction must observe cancellation between reads");
}

void testDisassemblyService() {
    FakeBackend backend;
    Mem::MemService service(backend);
    backend.memory = {
        0xC0, 0x03, 0x5F, 0xD6,
        0x1F, 0x20, 0x03, 0xD5,
    };

    Mem::DisassemblyRequest request;
    request.address = 0x8000;
    request.instructionCount = 2;
    const auto block = service.disassemble(
        service.captureContext(true), request);
    expect(block.ok() && block.value().bytes == backend.memory &&
               block.value().instructions.size() == 2 &&
               block.value().instructions[0].address == 0x8000 &&
               block.value().instructions[0].encoding == 0xD65F03C0 &&
               block.value().instructions[1].address == 0x8004 &&
               block.value().instructions[1].encoding == 0xD503201F &&
               block.value().target == backend.targetSnapshot(),
           "disassembly service should decode complete little-endian ARM64 words");

    request.instructionCount = 0;
    const int readsBeforeInvalid = backend.readCalls;
    const auto invalid = service.disassemble(
        service.captureContext(true), request);
    expect(!invalid.ok() &&
               invalid.error().code == Mem::ErrorCode::InvalidArgument &&
               backend.readCalls == readsBeforeInvalid,
           "invalid disassembly counts must fail before backend access");

    request.instructionCount = 2;
    backend.memory.resize(4);
    const auto partial = service.disassemble(
        service.captureContext(true), request);
    expect(!partial.ok() &&
               partial.error().code == Mem::ErrorCode::ProtocolError,
           "partial instruction reads must not produce truncated disassembly");
}

void testSymbolSessionService() {
    FakeBackend backend;
    Mem::MemService service(backend);

    Mem::SymbolListRequest listRequest;
    listRequest.moduleName = "libgame.so";
    listRequest.limit = 2;
    const auto firstPage = service.listSymbols(
        service.captureContext(true), listRequest);
    expect(firstPage.ok() && firstPage.value().total == 3 &&
               firstPage.value().items.size() == 2 &&
               firstPage.value().items.front().name == "GameInit" &&
               firstPage.value().nextOffset == 2 &&
               firstPage.value().session.epoch == 1 &&
               firstPage.value().session.module.base == 0x5000 &&
               backend.beginSymbolTransactionCalls == 1 &&
               backend.symbolInitializeCalls == 1 &&
               backend.symbolFetchCalls == 1 &&
               backend.allTransactionOperationsGuarded,
           "symbol list must resolve, initialize, and fetch inside one transaction");

    listRequest.offset = 2;
    listRequest.expectedEpoch = firstPage.value().session.epoch;
    const auto secondPage = service.listSymbols(
        service.captureContext(true), listRequest);
    expect(secondPage.ok() && secondPage.value().items.size() == 1 &&
               secondPage.value().items.front().name == "GameRender" &&
               !secondPage.value().nextOffset &&
               secondPage.value().session.epoch == 2,
           "symbol continuation must consume and advance the expected epoch");

    ++backend.symbolEpochValue;
    listRequest.expectedEpoch = secondPage.value().session.epoch;
    const int initializationsBeforeStale = backend.symbolInitializeCalls;
    const auto stale = service.listSymbols(
        service.captureContext(true), listRequest);
    expect(!stale.ok() &&
               stale.error().code == Mem::ErrorCode::SymbolSessionChanged &&
               backend.symbolInitializeCalls == initializationsBeforeStale,
           "external symbol initialization must invalidate continuation pages");

    Mem::SymbolListRequest unboundContinuation;
    unboundContinuation.moduleName = "libgame.so";
    unboundContinuation.offset = 1;
    const int transactionsBeforeInvalid = backend.beginSymbolTransactionCalls;
    const auto invalidContinuation = service.listSymbols(
        service.captureContext(true), unboundContinuation);
    expect(!invalidContinuation.ok() &&
               invalidContinuation.error().code ==
                   Mem::ErrorCode::InvalidArgument &&
               backend.beginSymbolTransactionCalls == transactionsBeforeInvalid,
           "symbol continuation must require an explicit epoch before backend access");

    FakeBackend tableBackend;
    tableBackend.symbolData.clear();
    for (size_t index = 0; index < 1005; ++index) {
        tableBackend.symbolData.push_back(Mem::SymbolInfo{
            0x5000 + static_cast<uint64_t>(index * 4),
            "Symbol" + std::to_string(index),
        });
    }
    Mem::MemService tableService(tableBackend);
    Mem::SymbolTableRequest tableRequest;
    tableRequest.moduleName = "libgame.so";
    const auto table = tableService.loadSymbolTable(
        tableService.captureContext(true), tableRequest);
    expect(table.ok() && table.value().items.size() == 1005 &&
               table.value().items.front().name == "Symbol0" &&
               table.value().items.back().name == "Symbol1004" &&
               table.value().session.total == 1005 &&
               table.value().session.epoch == 1 &&
               tableBackend.beginSymbolTransactionCalls == 1 &&
               tableBackend.symbolInitializeCalls == 1 &&
               tableBackend.symbolFetchCalls == 2 &&
               tableBackend.allTransactionOperationsGuarded,
           "complete symbol tables must initialize once and fetch every page under one transaction");

    FakeBackend oversizedNameBackend;
    oversizedNameBackend.symbolData = {
        Mem::SymbolInfo{0x5100,
                        std::string(Mem::kMaxSymbolNameBytes + 1, 'x')},
    };
    Mem::MemService oversizedNameService(oversizedNameBackend);
    const auto oversizedNameTable = oversizedNameService.loadSymbolTable(
        oversizedNameService.captureContext(true), tableRequest);
    expect(!oversizedNameTable.ok() &&
               oversizedNameTable.error().code ==
                   Mem::ErrorCode::ProtocolError,
           "complete symbol tables must reject oversized names before caching");

    Mem::SymbolResolveRequest resolveRequest;
    resolveRequest.moduleName = "libgame.so";
    resolveRequest.symbolName = "GameUpdate";
    const auto resolved = service.resolveSymbol(
        service.captureContext(true), resolveRequest);
    expect(resolved.ok() && resolved.value().address == 0x5200 &&
               resolved.value().name == "GameUpdate" &&
               resolved.value().session.module.name ==
                   "/data/app/libgame.so" &&
               resolved.value().session.epoch == backend.symbolEpochValue &&
               backend.symbolFindCalls == 1,
           "symbol resolve must bind module lookup, initialization, and find");

    resolveRequest.moduleName = "libgame";
    const int initializationsBeforeAmbiguous = backend.symbolInitializeCalls;
    const auto ambiguous = service.resolveSymbol(
        service.captureContext(true), resolveRequest);
    expect(!ambiguous.ok() &&
               ambiguous.error().code == Mem::ErrorCode::InvalidArgument &&
               backend.symbolInitializeCalls == initializationsBeforeAmbiguous,
           "symbol resolve must reject ambiguous modules before initialization");

    FakeBackend changedTargetBackend;
    Mem::MemService changedTargetService(changedTargetBackend);
    changedTargetBackend.changeTargetAfterSymbol = true;
    listRequest = {};
    listRequest.moduleName = "libgame.so";
    const auto changedTarget = changedTargetService.listSymbols(
        changedTargetService.captureContext(true), listRequest);
    expect(!changedTarget.ok() &&
               changedTarget.error().code == Mem::ErrorCode::TargetChanged &&
               changedTargetBackend.symbolFetchCalls == 1,
           "symbol list must reject the fetched page after the transaction releases and the target changed");

    FakeBackend changedTableTargetBackend;
    Mem::MemService changedTableTargetService(changedTableTargetBackend);
    changedTableTargetBackend.changeTargetAfterSymbol = true;
    const auto changedTableTarget = changedTableTargetService.loadSymbolTable(
        changedTableTargetService.captureContext(true), tableRequest);
    expect(!changedTableTarget.ok() &&
               changedTableTarget.error().code ==
                   Mem::ErrorCode::TargetChanged,
           "complete symbol tables must reject results after target replacement");

    FakeBackend changedGenerationBackend;
    Mem::MemService changedGenerationService(changedGenerationBackend);
    changedGenerationBackend.changeGenerationAfterSymbol = true;
    resolveRequest.moduleName = "libgame.so";
    const auto changedGeneration = changedGenerationService.resolveSymbol(
        changedGenerationService.captureContext(true), resolveRequest);
    expect(!changedGeneration.ok() &&
               changedGeneration.error().code ==
                   Mem::ErrorCode::ConnectionChanged &&
               changedGenerationBackend.symbolFindCalls == 0,
           "symbol resolve must stop after initialization changes generation");
}

void testBreakpointService() {
    FakeBackend backend;
    Mem::MemService service(backend);

    Mem::BreakpointSetRequest setRequest;
    setRequest.address = 0x7000;
    setRequest.access = Mem::BreakpointAccess::ReadWrite;
    setRequest.size = 8;
    const auto set = service.setBreakpoint(
        service.captureContext(true), setRequest);
    expect(set.ok() && set.value().address == 0x7000 &&
               set.value().action == Mem::BreakpointAction::Set &&
               set.value().access == Mem::BreakpointAccess::ReadWrite &&
               set.value().size == 8 &&
               backend.breakpointSetCalls == 1 &&
               backend.lastBreakpointAccess ==
                   Mem::BreakpointAccess::ReadWrite &&
               backend.lastBreakpointSize == 8,
           "breakpoint set should return a target-bound confirmed receipt");

    setRequest.access = Mem::BreakpointAccess::Execute;
    setRequest.size = 8;
    const int setCallsBeforeInvalid = backend.breakpointSetCalls;
    const auto invalidExecute = service.setBreakpoint(
        service.captureContext(true), setRequest);
    expect(!invalidExecute.ok() &&
               invalidExecute.error().code ==
                   Mem::ErrorCode::InvalidArgument &&
               backend.breakpointSetCalls == setCallsBeforeInvalid,
           "execute breakpoint size must be rejected before backend access");

    setRequest.access = Mem::BreakpointAccess::Execute;
    setRequest.size = 4;
    backend.breakpointRequestStarted = true;
    backend.breakpointResponseReceived = false;
    const auto unknown = service.setBreakpoint(
        service.captureContext(true), setRequest);
    expect(!unknown.ok() &&
               unknown.error().code == Mem::ErrorCode::CompletionUnknown &&
               !unknown.error().retryable,
           "sent breakpoint mutation without a response must be completion_unknown");

    backend.breakpointRequestStarted = false;
    const auto unsent = service.setBreakpoint(
        service.captureContext(true), setRequest);
    expect(!unsent.ok() &&
               unsent.error().code == Mem::ErrorCode::ProtocolError &&
               unsent.error().retryable,
           "unsent breakpoint mutation should remain retryable");

    backend.breakpointRequestStarted = true;
    backend.breakpointResponseReceived = true;
    backend.breakpointApplied = false;
    const auto rejected = service.setBreakpoint(
        service.captureContext(true), setRequest);
    expect(!rejected.ok() &&
               rejected.error().code == Mem::ErrorCode::ProtocolError &&
               !rejected.error().retryable,
           "server-rejected breakpoint mutation must not retry automatically");

    backend.breakpointApplied = true;
    Mem::OperationContext cancelledContext = service.captureContext(true);
    cancelledContext.cancellation =
        std::make_shared<std::atomic<bool>>(false);
    backend.cancelDuringBreakpoint = cancelledContext.cancellation;
    const auto completedAfterCancel = service.setBreakpoint(
        cancelledContext, setRequest);
    expect(completedAfterCancel.ok() &&
               completedAfterCancel.value().completedAfterCancelRequest,
           "confirmed breakpoint set must preserve late cancellation state");
    backend.cancelDuringBreakpoint.reset();

    Mem::OperationContext deadlineContext = service.captureContext(true);
    deadlineContext.deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(5);
    backend.breakpointDelayMs = 15;
    const auto completedAfterDeadline = service.setBreakpoint(
        deadlineContext, setRequest);
    expect(completedAfterDeadline.ok() &&
               completedAfterDeadline.value().completedAfterDeadline,
           "confirmed breakpoint set must preserve late deadline completion");
    backend.breakpointDelayMs = 0;

    FakeBackend changedTargetBackend;
    Mem::MemService changedTargetService(changedTargetBackend);
    changedTargetBackend.changeTargetAfterBreakpoint = true;
    const auto changedTarget = changedTargetService.setBreakpoint(
        changedTargetService.captureContext(true), setRequest);
    expect(!changedTarget.ok() &&
               changedTarget.error().code ==
                   Mem::ErrorCode::CompletionUnknown,
           "confirmed breakpoint mutation on a replaced target must be completion_unknown");

    Mem::BreakpointAddressRequest addressRequest;
    addressRequest.address = 0x7000;
    const auto removed = service.removeBreakpoint(
        service.captureContext(true), addressRequest);
    const auto suspended = service.suspendBreakpoint(
        service.captureContext(true), addressRequest);
    const auto resumed = service.resumeBreakpoint(
        service.captureContext(true), addressRequest);
    expect(removed.ok() && suspended.ok() && resumed.ok() &&
               removed.value().action == Mem::BreakpointAction::Remove &&
               suspended.value().action == Mem::BreakpointAction::Suspend &&
               resumed.value().action == Mem::BreakpointAction::Resume &&
               backend.breakpointRemoveCalls == 1 &&
               backend.breakpointSuspendCalls == 1 &&
               backend.breakpointResumeCalls == 1,
           "all breakpoint mutations should share the receipt contract");

    Mem::BreakpointHitBatchRequest batchRequest;
    batchRequest.address = 0x7000;
    batchRequest.limit = 3;
    const auto fullBatch = service.breakpointHitBatch(
        service.captureContext(true), batchRequest);
    expect(fullBatch.ok() && fullBatch.value().available == 3 &&
               fullBatch.value().dropped == 0 &&
               fullBatch.value().items.size() == 3 &&
               fullBatch.value().items.front().originalX0 == 0x7300 &&
               fullBatch.value().items.front().syscallNumber == 64 &&
               fullBatch.value().items.front().vectorRegisters[0][0] == 0xAB &&
               fullBatch.value().items.front().vectorRegisters[31][15] == 0xCD &&
               fullBatch.value().items.front().fpsr == 0x01000000 &&
               fullBatch.value().items.front().fpcr == 0x00400000,
           "breakpoint hit batches must preserve complete register state");

    batchRequest.limit = 2;
    const auto tailBatch = service.breakpointHitBatch(
        service.captureContext(true), batchRequest);
    expect(tailBatch.ok() && tailBatch.value().available == 3 &&
               tailBatch.value().dropped == 1 &&
               tailBatch.value().items.size() == 2 &&
               tailBatch.value().items.front().hitAddress == 0x7004 &&
               tailBatch.value().items.back().hitAddress == 0x7008,
           "breakpoint hit batches must retain the newest bounded tail");

    batchRequest.limit = Mem::kMaxBreakpointHitBatchSize + 1;
    const int batchCallsBeforeInvalid = backend.breakpointHitBatchCalls;
    const auto invalidBatch = service.breakpointHitBatch(
        service.captureContext(true), batchRequest);
    expect(!invalidBatch.ok() &&
               invalidBatch.error().code == Mem::ErrorCode::InvalidArgument &&
               backend.breakpointHitBatchCalls == batchCallsBeforeInvalid,
           "oversized breakpoint hit batches must fail before backend access");

    FakeBackend changedBatchTargetBackend;
    Mem::MemService changedBatchTargetService(changedBatchTargetBackend);
    changedBatchTargetBackend.changeTargetAfterBreakpointHits = true;
    batchRequest = {};
    batchRequest.address = 0x7000;
    const auto changedBatchTarget = changedBatchTargetService.breakpointHitBatch(
        changedBatchTargetService.captureContext(true), batchRequest);
    expect(!changedBatchTarget.ok() &&
               changedBatchTarget.error().code == Mem::ErrorCode::TargetChanged,
           "breakpoint hit batches must be rejected after target replacement");
}

void testScanSessionService() {
    FakeBackend backend;
    Mem::MemService service(backend);
    const Mem::OperationContext context = service.captureContext(true);

    Mem::ScanStartRequest startRequest;
    startRequest.kind = Mem::ScanStartKind::Value;
    startRequest.dataType = Mem::ScanDataType::Dword;
    startRequest.mode = Mem::ScanMode::Exact;
    startRequest.memoryRegion = Mem::ScanMemoryRegion::CHeap;
    startRequest.start = 0x1000;
    startRequest.end = 0x9000;
    startRequest.value = {42, 0, 0, 0};
    std::vector<Mem::ScanProgressUpdate> progressUpdates;
    const auto started = service.startScan(
        context,
        startRequest,
        [&](const Mem::ScanProgressUpdate& update) {
            progressUpdates.push_back(update);
        });
    expect(started.ok() && started.value().session.epoch == 2 &&
               started.value().session.resultCount == 3 &&
               started.value().session.target == backend.targetSnapshot() &&
               backend.scanSetRangeCalls == 1 &&
               backend.scanStartCalls == 1 &&
               backend.beginScanTransactionCalls == 1 &&
               backend.lastScanStart &&
               backend.lastScanStart->memoryRegion ==
                   Mem::ScanMemoryRegion::CHeap &&
               progressUpdates.size() == 1 &&
               progressUpdates.front().progress == 0.5f &&
               progressUpdates.front().scannedBytes == 4096,
           "scan start should set range and execute inside one tracked session transaction");

    Mem::ScanResultsRequest pageRequest;
    pageRequest.expectedEpoch = started.value().session.epoch;
    pageRequest.limit = 2;
    const auto page = service.scanResults(context, pageRequest);
    expect(page.ok() && page.value().total == 3 &&
               page.value().items.size() == 2 &&
               page.value().items[0].address == 0x1000 &&
               page.value().nextOffset == 2 &&
               page.value().session.epoch ==
                   started.value().session.epoch,
           "scan results should bind count and page to the expected epoch");

    Mem::ScanRefineRequest staleRefine;
    staleRefine.expectedEpoch = started.value().session.epoch - 1;
    staleRefine.mode = Mem::ScanMode::Changed;
    const int refineCallsBeforeStale = backend.scanRefineCalls;
    const auto stale = service.refineScan(context, staleRefine);
    expect(!stale.ok() &&
               stale.error().code == Mem::ErrorCode::ScanSessionChanged &&
               backend.scanRefineCalls == refineCallsBeforeStale,
           "stale scan epochs must fail before backend access");

    Mem::ScanRefineRequest refine;
    refine.expectedEpoch = started.value().session.epoch;
    refine.mode = Mem::ScanMode::Changed;
    const auto refined = service.refineScan(context, refine);
    expect(refined.ok() && refined.value().session.epoch == 3 &&
               backend.lastScanRefine &&
               backend.lastScanRefine->value ==
                   std::vector<unsigned char>({0, 0, 0, 0}),
           "valueless refine modes should use a typed protocol placeholder");

    pageRequest.expectedEpoch = started.value().session.epoch;
    const auto oldPage = service.scanResults(context, pageRequest);
    expect(!oldPage.ok() &&
               oldPage.error().code == Mem::ErrorCode::ScanSessionChanged,
           "refine must invalidate prior scan epoch receipts");

    pageRequest.expectedEpoch = refined.value().session.epoch;
    ++backend.scanEpochValue;
    const auto externallyChanged = service.scanResults(context, pageRequest);
    expect(!externallyChanged.ok() &&
               externallyChanged.error().code ==
                   Mem::ErrorCode::ScanSessionChanged,
           "legacy GUI or IPC scan mutations must invalidate native sessions");

    const auto restarted = service.startScan(context, startRequest);
    expect(restarted.ok(), "scan start should replace an invalidated session");

    Mem::ScanRemoveRequest removeRequest;
    removeRequest.expectedEpoch = restarted.value().session.epoch;
    removeRequest.addresses = {0x2000, 0x2000};
    const auto removed = service.removeScanResults(context, removeRequest);
    expect(removed.ok() && removed.value().requested == 1 &&
               removed.value().previousCount == 3 &&
               removed.value().currentCount == 2 &&
               removed.value().previousEpoch ==
                   restarted.value().session.epoch &&
               removed.value().currentEpoch == backend.scanEpochValue &&
               backend.scanRemoveCalls == 1,
           "scan result removal must deduplicate addresses and confirm count plus epoch in one transaction");

    Mem::ScanClearRequest clearRequest;
    clearRequest.expectedEpoch = removed.value().currentEpoch;
    const auto cleared = service.clearScan(context, clearRequest);
    expect(cleared.ok() &&
               cleared.value().clearedEpoch ==
                   removed.value().currentEpoch &&
               backend.scanResultsData.empty() &&
               backend.scanClearCalls == 1,
           "scan clear should verify the remote result set is empty");
    const auto afterClear = service.scanResults(context, pageRequest);
    expect(!afterClear.ok() &&
               afterClear.error().code == Mem::ErrorCode::NoScanSession,
           "cleared sessions must not remain readable");

    FakeBackend unknownBackend;
    Mem::MemService unknownService(unknownBackend);
    Mem::ScanStartRequest unknownRequest;
    unknownRequest.kind = Mem::ScanStartKind::Unknown;
    unknownRequest.dataType = Mem::ScanDataType::Qword;
    unknownRequest.mode = Mem::ScanMode::Unknown;
    const auto unknown = unknownService.startScan(
        unknownService.captureContext(true), unknownRequest);
    expect(unknown.ok() && unknownBackend.lastScanStart &&
               unknownBackend.lastScanStart->value.empty(),
           "unknown scan starts should not require a comparison value");

    Mem::ScanStartRequest patternRequest;
    patternRequest.kind = Mem::ScanStartKind::BytePattern;
    patternRequest.dataType = Mem::ScanDataType::Bytes;
    patternRequest.mode = Mem::ScanMode::Exact;
    patternRequest.value = {0xDE, 0xAD};
    const auto pattern = unknownService.startScan(
        unknownService.captureContext(true), patternRequest);
    expect(pattern.ok(), "byte-pattern scan start should be supported");

    Mem::ScanStartRequest combinedRegion = startRequest;
    combinedRegion.memoryRegion = static_cast<Mem::ScanMemoryRegion>(
        static_cast<int32_t>(Mem::ScanMemoryRegion::CHeap) |
        static_cast<int32_t>(Mem::ScanMemoryRegion::CData) |
        static_cast<int32_t>(Mem::ScanMemoryRegion::Anonymous));
    const auto combined = unknownService.startScan(
        unknownService.captureContext(true), combinedRegion);
    expect(combined.ok() && unknownBackend.lastScanStart &&
               unknownBackend.lastScanStart->memoryRegion ==
                   combinedRegion.memoryRegion,
           "GUI scan memory-region combinations must remain valid service requests");

    Mem::ScanStartRequest invalidBetween = startRequest;
    invalidBetween.mode = Mem::ScanMode::Between;
    const int beginBeforeInvalid = backend.beginScanTransactionCalls;
    const auto invalid = service.startScan(context, invalidBetween);
    expect(!invalid.ok() &&
               invalid.error().code == Mem::ErrorCode::InvalidArgument &&
               backend.beginScanTransactionCalls == beginBeforeInvalid,
           "invalid between values must fail before acquiring a transaction");

    FakeBackend unknownCompletionBackend;
    Mem::MemService unknownCompletionService(unknownCompletionBackend);
    unknownCompletionBackend.scanResponseReceived = false;
    const auto completionUnknown = unknownCompletionService.startScan(
        unknownCompletionService.captureContext(true), startRequest);
    expect(!completionUnknown.ok() &&
               completionUnknown.error().code ==
                   Mem::ErrorCode::CompletionUnknown &&
               !completionUnknown.error().retryable,
           "sent scans without a terminal count must be completion_unknown");

    FakeBackend cancelledBackend;
    Mem::MemService cancelledService(cancelledBackend);
    Mem::OperationContext cancelledContext =
        cancelledService.captureContext(true);
    cancelledContext.cancellation =
        std::make_shared<std::atomic<bool>>(false);
    cancelledBackend.cancelDuringScan = cancelledContext.cancellation;
    const auto completedAfterCancel = cancelledService.startScan(
        cancelledContext, startRequest);
    expect(completedAfterCancel.ok() &&
               completedAfterCancel.value().completedAfterCancelRequest,
           "confirmed scan completion must remain visible after cancellation request");

    FakeBackend targetChangedBackend;
    Mem::MemService targetChangedService(targetChangedBackend);
    targetChangedBackend.changeTargetAfterScan = true;
    const auto targetChanged = targetChangedService.startScan(
        targetChangedService.captureContext(true), startRequest);
    expect(!targetChanged.ok() &&
               targetChanged.error().code == Mem::ErrorCode::TargetChanged,
           "scan completion must be rejected when the target changes");

    FakeBackend generationChangedBackend;
    Mem::MemService generationChangedService(generationChangedBackend);
    generationChangedBackend.changeGenerationAfterScan = true;
    const auto generationChanged = generationChangedService.startScan(
        generationChangedService.captureContext(true), startRequest);
    expect(!generationChanged.ok() &&
               generationChanged.error().code ==
                   Mem::ErrorCode::ConnectionChanged,
           "scan completion must be rejected after reconnect");
}

void testHiddenToolRegistration() {
    auto& registry = AI::ToolExecutor::getInstance();
    registry.registerTool("test_visible_tool", "visible", "{}",
                          AI::ToolSafety::ReadOnly,
                          [](const std::string&) { return std::string("{}"); });
    registry.registerTool("test_hidden_alias", "hidden", "{}",
                          AI::ToolSafety::ReadOnly,
                          [](const std::string&) { return std::string("{}"); },
                          false);
    registry.registerTool(
        "test_completion_unknown", "completion", "{}",
        AI::ToolSafety::Write,
        [](const std::string&) {
            return std::string(
                R"({"success":false,"error":{"code":"completion_unknown","message":"receipt lost","retryable":false}})");
        });

    const auto definitions = registry.getToolDefinitions();
    bool foundVisible = false;
    bool foundHidden = false;
    for (const auto& definition : definitions) {
        foundVisible = foundVisible || definition.name == "test_visible_tool";
        foundHidden = foundHidden || definition.name == "test_hidden_alias";
    }
    expect(foundVisible, "advertised tool must be returned to providers");
    expect(!foundHidden, "hidden compatibility alias must not be advertised");

    AI::ToolCall call;
    call.name = "test_hidden_alias";
    call.arguments = "{}";
    const AI::ToolResult executed = registry.execute(call);
    expect(executed.success,
           "hidden compatibility alias must remain executable for old sessions");

    call.name = "test_completion_unknown";
    const AI::ToolResult unknown = registry.execute(call);
    expect(!unknown.success &&
               unknown.completion ==
                   AI::ToolCompletionState::CompletionUnknown,
           "structured completion_unknown must survive result normalization");
}

void testToolSchemaValidationMatrix() {
    auto& registry = AI::ToolExecutor::getInstance();
    int executionCount = 0;
    const std::string schema = R"JSON({
      "type": "object",
      "required": ["address", "mode", "count", "offsets", "choice"],
      "additionalProperties": false,
      "properties": {
        "address": {
          "type": "string",
          "pattern": "^0[xX][0-9A-Fa-f]+$",
          "minLength": 3,
          "maxLength": 18
        },
        "mode": {
          "type": "string",
          "enum": ["read", "write"]
        },
        "count": {
          "type": "integer",
          "minimum": 1,
          "maximum": 8
        },
        "offsets": {
          "type": "array",
          "minItems": 1,
          "maxItems": 2,
          "items": {
            "type": "string",
            "pattern": "^0[xX][0-9A-Fa-f]+$"
          }
        },
        "choice": {
          "anyOf": [
            {"type": "string", "enum": ["auto"]},
            {"type": "integer", "minimum": 1, "maximum": 2}
          ]
        },
        "exclusive": {
          "oneOf": [
            {"type": "string", "minLength": 1},
            {"type": "integer", "minimum": 1}
          ]
        }
      }
    })JSON";
    registry.registerTool(
        "test_schema_matrix", "schema matrix", schema,
        AI::ToolSafety::ReadOnly,
        [&executionCount](const std::string&) {
            ++executionCount;
            return std::string(R"({"success":true})");
        });

    const auto execute = [&](const std::string& arguments) {
        return registry.execute(
            toolCall("schema-matrix", "test_schema_matrix", arguments));
    };
    const auto expectRejected = [&](const std::string& arguments,
                                    const std::string& marker) {
        const int before = executionCount;
        const AI::ToolResult result = execute(arguments);
        expect(!result.success && executionCount == before &&
                   result.errorMessage.find(marker) != std::string::npos,
               "schema validation should reject before execution: " + marker);
    };

    const AI::ToolResult valid = execute(
        R"({"address":"0x1234","mode":"read","count":2,"offsets":["0x4"],"choice":"auto","exclusive":1})");
    expect(valid.success && executionCount == 1,
           "a value satisfying the complete schema subset should execute once");

    expectRejected(R"({"mode":"read","count":2,"offsets":["0x4"],"choice":"auto"})",
                   "missing required property");
    expectRejected(R"({"address":1234,"mode":"read","count":2,"offsets":["0x4"],"choice":"auto"})",
                   "expected string");
    expectRejected(R"({"address":"1234","mode":"read","count":2,"offsets":["0x4"],"choice":"auto"})",
                   "required pattern");
    expectRejected(R"({"address":"0x1234","mode":"execute","count":2,"offsets":["0x4"],"choice":"auto"})",
                   "enum values");
    expectRejected(R"({"address":"0x1234","mode":"read","count":0,"offsets":["0x4"],"choice":"auto"})",
                   "below minimum");
    expectRejected(R"({"address":"0x1234","mode":"read","count":9,"offsets":["0x4"],"choice":"auto"})",
                   "above maximum");
    expectRejected(R"({"address":"0x1234","mode":"read","count":2,"offsets":[],"choice":"auto"})",
                   "fewer items");
    expectRejected(R"({"address":"0x1234","mode":"read","count":2,"offsets":["0x1","0x2","0x3"],"choice":"auto"})",
                   "more items");
    expectRejected(R"({"address":"0x1234","mode":"read","count":2,"offsets":["4"],"choice":"auto"})",
                   "required pattern");
    expectRejected(R"({"address":"0x1234","mode":"read","count":2,"offsets":["0x4"],"choice":false})",
                   "did not match any");
    expectRejected(R"({"address":"0x1234","mode":"read","count":2,"offsets":["0x4"],"choice":"auto","exclusive":false})",
                   "exactly one");
    expectRejected(R"({"address":"0x1234","mode":"read","count":2,"offsets":["0x4"],"choice":"auto","unknown":1})",
                   "unexpected property");

    int malformedExecutions = 0;
    registry.registerTool(
        "test_invalid_schema", "invalid schema",
        R"({"type":"object","properties":{},"unsupported":true})",
        AI::ToolSafety::ReadOnly,
        [&malformedExecutions](const std::string&) {
            ++malformedExecutions;
            return std::string("{}");
        });
    const AI::ToolResult invalidSchema = registry.execute(
        toolCall("invalid-schema", "test_invalid_schema", "{}"));
    const auto definitions = registry.getToolDefinitions();
    const bool invalidAdvertised = std::any_of(
        definitions.begin(), definitions.end(),
        [](const AI::ToolDefinition& definition) {
            return definition.name == "test_invalid_schema";
        });
    expect(!invalidSchema.success && malformedExecutions == 0 &&
               invalidSchema.errorMessage.find("unsupported keyword") !=
                   std::string::npos &&
               !invalidAdvertised,
           "unsupported schema keywords must fail closed before advertisement and execution");
}

void testAgentRunnerApprovalMatrix() {
    auto& registry = AI::ToolExecutor::getInstance();
    registry.registerTool(
        "test_runner_read", "runner read", "{}", AI::ToolSafety::ReadOnly,
        [](const std::string&) { return std::string(R"({"success":true})"); });
    registry.registerTool(
        "test_runner_write", "runner write", "{}", AI::ToolSafety::Write,
        [](const std::string&) { return std::string(R"({"success":true})"); });

    const auto hasTrace = [](const AI::AgentRunner::Outcome& outcome,
                             AI::AgentTraceType type,
                             const std::string& tool = {}) {
        return std::any_of(
            outcome.traceEvents.begin(), outcome.traceEvents.end(),
            [&](const AI::AgentTraceEvent& event) {
                return event.type == type &&
                       (tool.empty() || event.tool == tool);
            });
    };
    const auto successfulResult = [] {
        AI::ToolResult result;
        result.success = true;
        result.resultJson = R"({"success":true})";
        return result;
    };
    const auto messagePayload = [](const AI::ChatMessage& message) {
        return json::parse(message.content);
    };

    const AI::ToolCall firstRead =
        toolCall("runner-read-1", "test_runner_read");
    const AI::ToolCall write =
        toolCall("runner-write", "test_runner_write");
    const AI::ToolCall secondRead =
        toolCall("runner-read-2", "test_runner_read");

    {
        AI::AgentRunner runner;
        AI::AgentRunner::Config config;
        auto readReady = runner.beginToolCalls(
            {firstRead, write, secondRead}, config);
        expect(readReady.kind == AI::AgentRunner::OutcomeKind::NeedsExecution &&
                   readReady.toolCallToExecute &&
                   readReady.toolCallToExecute->id == firstRead.id &&
                   !runner.hasPendingConfirmation(),
               "read-only tools must execute without approval");

        auto waiting = runner.completeToolExecution(
            firstRead, successfulResult(), 2, config);
        expect(waiting.kind ==
                   AI::AgentRunner::OutcomeKind::NeedsConfirmation &&
                   waiting.pendingToolCall &&
                   waiting.pendingToolCall->id == write.id &&
                   runner.hasPendingConfirmation() &&
                   waiting.messages.size() == 1 &&
                   waiting.messages.front().toolCallId == firstRead.id,
               "a successful read must advance to manual write approval with a paired result");

        auto denied = runner.resumeDenied(config);
        expect(denied.kind ==
                   AI::AgentRunner::OutcomeKind::ReadyForFollowUp &&
                   !runner.hasPendingConfirmation() &&
                   denied.messages.size() == 2 &&
                   denied.messages[0].toolCallId == write.id &&
                   denied.messages[1].toolCallId == secondRead.id &&
                   !messagePayload(denied.messages[0]).at("success").get<bool>() &&
                   messagePayload(denied.messages[1]).at("skipped").get<bool>() &&
                   hasTrace(denied, AI::AgentTraceType::Denied,
                            "test_runner_write") &&
                   hasTrace(denied, AI::AgentTraceType::ToolSkipped,
                            "test_runner_read") &&
                   hasTrace(denied, AI::AgentTraceType::ToolBatchComplete),
               "manual denial must pair the denied result and skip every later call");
    }

    {
        AI::AgentRunner runner;
        AI::AgentRunner::Config config;
        auto waiting = runner.beginToolCalls({write, firstRead}, config);
        expect(waiting.kind ==
                   AI::AgentRunner::OutcomeKind::NeedsConfirmation,
               "manual mode must block a write before execution");
        auto approved = runner.resumeApproved(config);
        expect(approved.kind == AI::AgentRunner::OutcomeKind::NeedsExecution &&
                   approved.toolCallToExecute &&
                   approved.toolCallToExecute->id == write.id &&
                   hasTrace(approved, AI::AgentTraceType::Approved,
                            "test_runner_write"),
               "manual approval must release exactly the pending write");
        auto readReady = runner.completeToolExecution(
            write, successfulResult(), 3, config);
        expect(readReady.kind == AI::AgentRunner::OutcomeKind::NeedsExecution &&
                   readReady.toolCallToExecute &&
                   readReady.toolCallToExecute->id == firstRead.id,
               "a successful approved write must advance to the next read");
        auto complete = runner.completeToolExecution(
            firstRead, successfulResult(), 1, config);
        expect(complete.kind ==
                   AI::AgentRunner::OutcomeKind::ReadyForFollowUp &&
                   hasTrace(complete, AI::AgentTraceType::ToolBatchComplete),
               "an approved write/read batch must finish normally");
    }

    {
        AI::AgentRunner runner;
        AI::AgentRunner::Config config;
        config.autoApproveWrites = true;
        auto writeReady = runner.beginToolCalls({write, firstRead}, config);
        expect(writeReady.kind ==
                   AI::AgentRunner::OutcomeKind::NeedsExecution &&
                   writeReady.toolCallToExecute &&
                   writeReady.toolCallToExecute->id == write.id &&
                   !runner.hasPendingConfirmation() &&
                   hasTrace(writeReady, AI::AgentTraceType::AutoApproved,
                            "test_runner_write") &&
                   hasTrace(writeReady, AI::AgentTraceType::ToolStarted,
                            "test_runner_write"),
               "auto-approve must skip only the dialog and still expose an execution step");

        auto readReady = runner.completeToolExecution(
            write, successfulResult(), 4, config);
        expect(readReady.kind == AI::AgentRunner::OutcomeKind::NeedsExecution &&
                   readReady.toolCallToExecute &&
                   readReady.toolCallToExecute->id == firstRead.id &&
                   readReady.messages.size() == 1 &&
                   readReady.messages.front().toolCallId == write.id,
               "an auto-approved write must still produce a paired result before the next call");
        auto complete = runner.completeToolExecution(
            firstRead, successfulResult(), 1, config);
        expect(complete.kind ==
                   AI::AgentRunner::OutcomeKind::ReadyForFollowUp &&
                   complete.messages.size() == 1 &&
                   complete.messages.front().toolCallId == firstRead.id,
               "auto-approved batches must preserve normal result ordering");
    }

    {
        AI::AgentRunner runner;
        AI::AgentRunner::Config config;
        config.autoApproveWrites = true;
        auto writeReady = runner.beginToolCalls({write, firstRead}, config);
        expect(writeReady.kind ==
                   AI::AgentRunner::OutcomeKind::NeedsExecution,
               "auto-approved failure case must release the write for execution");

        AI::ToolResult failure;
        failure.success = false;
        failure.errorMessage = "simulated write failure";
        failure.resultJson =
            R"({"success":false,"error":{"code":"failed","message":"simulated write failure"}})";
        failure.completion = AI::ToolCompletionState::RejectedBeforeStart;
        auto failed = runner.completeToolExecution(write, failure, 5, config);
        expect(failed.kind ==
                   AI::AgentRunner::OutcomeKind::ReadyForFollowUp &&
                   failed.messages.size() == 2 &&
                   failed.messages[0].toolCallId == write.id &&
                   failed.messages[1].toolCallId == firstRead.id &&
                   !messagePayload(failed.messages[0]).at("success").get<bool>() &&
                   messagePayload(failed.messages[1]).at("skipped").get<bool>() &&
                   hasTrace(failed, AI::AgentTraceType::ToolFailed,
                            "test_runner_write") &&
                   hasTrace(failed, AI::AgentTraceType::ToolSkipped,
                            "test_runner_read"),
               "auto-approval must not suppress failure propagation or execute later calls");
    }
}

void testToolJsonComplexityLimits() {
    auto denseNestedArrays = [](size_t arrayCount, size_t itemsPerArray) {
        std::string serialized;
        serialized.reserve(arrayCount * (itemsPerArray * 2u + 3u) + 2u);
        serialized.push_back('[');
        for (size_t arrayIndex = 0; arrayIndex < arrayCount; ++arrayIndex) {
            if (arrayIndex != 0) {
                serialized.push_back(',');
            }
            serialized.push_back('[');
            for (size_t item = 0; item < itemsPerArray; ++item) {
                if (item != 0) {
                    serialized.push_back(',');
                }
                serialized.push_back('0');
            }
            serialized.push_back(']');
        }
        serialized.push_back(']');
        return serialized;
    };

    auto& registry = AI::ToolExecutor::getInstance();
    int argumentExecutions = 0;
    registry.registerTool(
        "test_dense_arguments", "dense arguments",
        R"({"type":"object","additionalProperties":true})",
        AI::ToolSafety::ReadOnly,
        [&argumentExecutions](const std::string&) {
            ++argumentExecutions;
            return std::string(R"({"success":true})");
        });
    const std::string denseArguments =
        std::string("{\"items\":") +
        denseNestedArrays(3u, 3000u) + "}";
    const AI::ToolResult rejectedArguments = registry.execute(
        toolCall("dense-arguments", "test_dense_arguments", denseArguments));
    expect(!rejectedArguments.success && argumentExecutions == 0 &&
               rejectedArguments.errorMessage.find("node limit") !=
                   std::string::npos,
           "dense tool arguments must be rejected before executor invocation");

    const std::string denseResult =
        std::string("{\"success\":true,\"items\":") +
        denseNestedArrays(17u, 4000u) + "}";
    registry.registerTool(
        "test_dense_read_result", "dense read result", "{}",
        AI::ToolSafety::ReadOnly,
        [denseResult](const std::string&) { return denseResult; });
    const AI::ToolResult rejectedRead = registry.execute(
        toolCall("dense-read", "test_dense_read_result"));
    expect(!rejectedRead.success &&
               rejectedRead.errorMessage.find("node limit") !=
                   std::string::npos &&
               rejectedRead.completion == AI::ToolCompletionState::Completed,
           "dense read results must fail without inventing mutation completion semantics");

    registry.registerTool(
        "test_dense_write_result", "dense write result", "{}",
        AI::ToolSafety::Write,
        [denseResult](const std::string&) { return denseResult; });
    const AI::ToolResult rejectedWrite = registry.execute(
        toolCall("dense-write", "test_dense_write_result"));
    expect(!rejectedWrite.success &&
               rejectedWrite.errorMessage.find("node limit") !=
                   std::string::npos &&
               rejectedWrite.completion ==
                   AI::ToolCompletionState::CompletionUnknown,
           "dense write results must retain completion_unknown after execution");

    registry.registerTool(
        "test_invalid_result_json", "invalid result", "{}",
        AI::ToolSafety::ReadOnly,
        [](const std::string&) { return std::string("not-json"); });
    const AI::ToolResult invalidResult = registry.execute(
        toolCall("invalid-result", "test_invalid_result_json"));
    expect(!invalidResult.success &&
               invalidResult.errorMessage.find("invalid JSON syntax") !=
                   std::string::npos,
           "invalid executor JSON must never be normalized as success");
}

void testToolResultLimit() {
    auto& registry = AI::ToolExecutor::getInstance();
    registry.registerTool(
        "test_oversized_tool_result", "oversized result", "{}",
        AI::ToolSafety::Write,
        [](const std::string&) {
            return std::string(AI::Limits::kMaxToolResultBytes + 1u, 'x');
        });

    const AI::ToolResult result = registry.execute(
        toolCall("oversized-result", "test_oversized_tool_result"));
    expect(!result.success && result.resultJson.empty() &&
               result.completion ==
                   AI::ToolCompletionState::CompletionUnknown &&
               result.errorMessage.find("4 MiB") != std::string::npos,
           "oversized mutation result must be discarded with unknown completion");
}

void testRetiredToolHistoryDowngrade() {
    const std::vector<std::string> retiredNames = {
        "get_status", "get_server_version", "get_architecture",
        "init_driver", "read_memory", "read_value", "write_bytes",
        "write_value", "scan_set_range", "scan_value", "scan_next",
        "scan_fuzzy", "scan_hex", "get_scan_count", "get_scan_results",
        "clear_scan", "get_module_list", "list_modules", "get_module_base",
        "get_process_list", "list_processes", "open_process",
        "resolve_offset_chain", "read_disassembly", "set_breakpoint",
        "remove_breakpoint", "read_breakpoint_info", "suspend_breakpoint",
        "resume_breakpoint", "resolve_symbol", "symbol_init", "symbol_find",
        "execute_lua",
    };

    for (const auto& name : retiredNames) {
        AI::ChatSession session;
        AI::ChatMessage assistant;
        assistant.role = AI::Role::Assistant;
        assistant.toolCalls.push_back(toolCall("legacy-call", name));
        session.addMessage(std::move(assistant));

        AI::ChatMessage result;
        result.role = AI::Role::Tool;
        result.toolCallId = "legacy-call";
        result.name = name;
        result.content = R"({"success":true})";
        session.addMessage(std::move(result));

        const auto outgoing = session.getMessagesForRequest();
        expect(outgoing.size() == 1 &&
                   outgoing.front().role == AI::Role::Assistant &&
                   outgoing.front().toolCalls.empty() &&
                   outgoing.front().content.find(name) != std::string::npos &&
                   outgoing.front().content.find(R"({"success":true})") !=
                       std::string::npos,
               "retired tool history must become non-executable assistant text: " +
                   name);
    }

    AI::ChatSession mixed;
    AI::ChatMessage mixedAssistant;
    mixedAssistant.role = AI::Role::Assistant;
    mixedAssistant.toolCalls.push_back(
        toolCall("legacy", "scan_value", R"({"value":42})"));
    mixedAssistant.toolCalls.push_back(
        toolCall("canonical", "memory_read", R"({"address":"0x1000","size":4})"));
    mixed.addMessage(std::move(mixedAssistant));
    for (const auto& id : {std::string("legacy"), std::string("canonical")}) {
        AI::ChatMessage result;
        result.role = AI::Role::Tool;
        result.toolCallId = id;
        result.content = id + "-result";
        mixed.addMessage(std::move(result));
    }
    const auto mixedOutgoing = mixed.getMessagesForRequest();
    expect(mixedOutgoing.size() == 1 &&
               mixedOutgoing.front().toolCalls.empty() &&
               mixedOutgoing.front().content.find("scan_value") !=
                   std::string::npos &&
               mixedOutgoing.front().content.find("memory_read") !=
                   std::string::npos,
           "a mixed historical tool group must downgrade atomically");

    AI::ChatSession incomplete;
    AI::ChatMessage incompleteAssistant;
    incompleteAssistant.role = AI::Role::Assistant;
    incompleteAssistant.toolCalls.push_back(
        toolCall("missing", "read_disassembly", R"({"address":4096})"));
    incomplete.addMessage(std::move(incompleteAssistant));
    const auto incompleteOutgoing = incomplete.getMessagesForRequest();
    expect(incompleteOutgoing.size() == 1 &&
               incompleteOutgoing.front().toolCalls.empty() &&
               incompleteOutgoing.front().content.find("[not recorded]") !=
                   std::string::npos,
           "incomplete retired tool history must remain visible as text");

    AI::ChatSession sensitive;
    AI::ChatMessage sensitiveAssistant;
    sensitiveAssistant.role = AI::Role::Assistant;
    AI::ToolCall sensitiveCall = toolCall(
        "driver", "init_driver", R"({"card_name":"secret-card"})");
    sensitiveCall.redactedArguments = R"({"card_name":"[REDACTED]"})";
    sensitiveAssistant.toolCalls.push_back(std::move(sensitiveCall));
    sensitive.addMessage(std::move(sensitiveAssistant));
    const auto sensitiveOutgoing = sensitive.getMessagesForRequest();
    expect(sensitiveOutgoing.size() == 1 &&
               sensitiveOutgoing.front().content.find("secret-card") ==
                   std::string::npos &&
               sensitiveOutgoing.front().content.find("[REDACTED]") !=
                   std::string::npos,
           "retired sensitive tool history must use redacted arguments");

    AI::ChatSession canonical;
    AI::ChatMessage canonicalAssistant;
    canonicalAssistant.role = AI::Role::Assistant;
    canonicalAssistant.toolCalls.push_back(
        toolCall("canonical", "memory_read", R"({"address":"0x1000","size":4})"));
    canonical.addMessage(std::move(canonicalAssistant));
    AI::ChatMessage canonicalResult;
    canonicalResult.role = AI::Role::Tool;
    canonicalResult.toolCallId = "canonical";
    canonicalResult.name = "memory_read";
    canonicalResult.content = R"({"success":true})";
    canonical.addMessage(std::move(canonicalResult));
    const auto canonicalOutgoing = canonical.getMessagesForRequest();
    expect(canonicalOutgoing.size() == 2 &&
               canonicalOutgoing.front().toolCalls.size() == 1 &&
               canonicalOutgoing.back().role == AI::Role::Tool,
           "canonical tool history must retain provider tool-call structure");
}

void testToolHistoryPairing() {
    const auto makeMessage = [](AI::Role role, std::string content) {
        AI::ChatMessage message;
        message.role = role;
        message.content = std::move(content);
        return message;
    };

    AI::ChatSession complete;
    complete.setSystemPrompt("system prompt");
    complete.addMessage(makeMessage(AI::Role::Tool, "orphan-before"));
    complete.addMessage(makeMessage(AI::Role::User, "first turn"));

    AI::ChatMessage assistant;
    assistant.role = AI::Role::Assistant;
    assistant.content = "calling tools";
    assistant.toolCalls.push_back(toolCall("call-a", "status", "{}"));
    assistant.toolCalls.push_back(
        toolCall("call-b", "memory_read",
                 R"({"address":"0x1000","size":4})"));
    complete.addMessage(std::move(assistant));

    AI::ChatMessage resultB = makeMessage(AI::Role::Tool, "result-b");
    resultB.toolCallId = "call-b";
    complete.addMessage(std::move(resultB));
    complete.addMessage(makeMessage(AI::Role::System, "runtime notice"));
    AI::ChatMessage unknown = makeMessage(AI::Role::Tool, "unknown-result");
    unknown.toolCallId = "unknown";
    complete.addMessage(std::move(unknown));
    AI::ChatMessage resultA = makeMessage(AI::Role::Tool, "result-a");
    resultA.toolCallId = "call-a";
    complete.addMessage(std::move(resultA));
    AI::ChatMessage duplicateResult =
        makeMessage(AI::Role::Tool, "duplicate-result");
    duplicateResult.toolCallId = "call-a";
    complete.addMessage(std::move(duplicateResult));
    complete.addMessage(makeMessage(AI::Role::User, "second turn"));

    const auto paired = complete.getMessagesForRequest();
    expect(paired.size() == 6 &&
               paired[0].role == AI::Role::System &&
               paired[1].role == AI::Role::User &&
               paired[2].role == AI::Role::Assistant &&
               paired[2].toolCalls.size() == 2 &&
               paired[3].role == AI::Role::Tool &&
               paired[3].toolCallId == "call-b" &&
               paired[4].role == AI::Role::Tool &&
               paired[4].toolCallId == "call-a" &&
               paired[5].role == AI::Role::User &&
               paired[5].content == "second turn",
           "complete tool groups must retain only matched results and notices must stay local");

    AI::ChatSession duplicateIds;
    AI::ChatMessage duplicateCalls;
    duplicateCalls.role = AI::Role::Assistant;
    duplicateCalls.content = "duplicate fallback";
    duplicateCalls.toolCalls.push_back(toolCall("same", "status", "{}"));
    duplicateCalls.toolCalls.push_back(
        toolCall("same", "module_list", R"({"offset":0,"limit":1})"));
    duplicateIds.addMessage(std::move(duplicateCalls));
    AI::ChatMessage duplicateTool = makeMessage(AI::Role::Tool, "ignored");
    duplicateTool.toolCallId = "same";
    duplicateIds.addMessage(std::move(duplicateTool));
    const auto duplicateOutgoing = duplicateIds.getMessagesForRequest();
    expect(duplicateOutgoing.size() == 1 &&
               duplicateOutgoing.front().role == AI::Role::Assistant &&
               duplicateOutgoing.front().toolCalls.empty() &&
               duplicateOutgoing.front().content == "duplicate fallback",
           "duplicate tool-call ids must downgrade to assistant text");

    AI::ChatSession incomplete;
    AI::ChatMessage interrupted;
    interrupted.role = AI::Role::Assistant;
    interrupted.content = "interrupted fallback";
    interrupted.toolCalls.push_back(toolCall("done", "status", "{}"));
    interrupted.toolCalls.push_back(
        toolCall("missing", "memory_read",
                 R"({"address":"0x2000","size":4})"));
    incomplete.addMessage(std::move(interrupted));
    AI::ChatMessage onlyResult = makeMessage(AI::Role::Tool, "done-result");
    onlyResult.toolCallId = "done";
    incomplete.addMessage(std::move(onlyResult));
    const auto incompleteOutgoing = incomplete.getMessagesForRequest();
    expect(incompleteOutgoing.size() == 1 &&
               incompleteOutgoing.front().toolCalls.empty() &&
               incompleteOutgoing.front().content == "interrupted fallback",
           "incomplete canonical groups must not emit partial tool protocol");

    AI::ChatSession invalidCall;
    AI::ChatMessage partiallyInvalid;
    partiallyInvalid.role = AI::Role::Assistant;
    partiallyInvalid.toolCalls.push_back(toolCall({}, "status", "{}"));
    partiallyInvalid.toolCalls.push_back(toolCall("valid", {}, "{}"));
    partiallyInvalid.toolCalls.push_back(toolCall("valid", "status", "{}"));
    invalidCall.addMessage(std::move(partiallyInvalid));
    AI::ChatMessage validResult = makeMessage(AI::Role::Tool, "valid-result");
    validResult.toolCallId = "valid";
    invalidCall.addMessage(std::move(validResult));
    const auto cleaned = invalidCall.getMessagesForRequest();
    expect(cleaned.size() == 2 && cleaned.front().toolCalls.size() == 1 &&
               cleaned.front().toolCalls.front().id == "valid" &&
               cleaned.back().toolCallId == "valid",
           "invalid call fields must be removed without discarding a valid pair");
}

void testDeviceSessionLifecycle() {
    auto& session = DeviceSession::GetInstance();
    {
        auto lifecycle = session.AcquireLifecycle();
        session.Disconnect();
        session.BeginConnect();
        session.FinishConnect(true);
    }

    std::atomic<bool> lifecycleStarted{false};
    std::atomic<bool> lifecycleAcquired{false};
    std::thread lifecycleThread;
    {
        auto request = session.AcquireRequest();
        expect(request && request.isCurrent(),
               "connected session should grant a current request lease");
        auto nestedRequest = session.AcquireRequest();
        expect(nestedRequest && nestedRequest.isCurrent() &&
                   nestedRequest.generation() == request.generation(),
               "nested commands should reuse the active request lease");
        lifecycleThread = std::thread([&] {
            lifecycleStarted.store(true, std::memory_order_release);
            auto lifecycle = session.AcquireLifecycle();
            lifecycleAcquired.store(true, std::memory_order_release);
            session.Disconnect();
        });

        while (!lifecycleStarted.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        expect(!lifecycleAcquired.load(std::memory_order_acquire),
               "exclusive lifecycle transition must wait for request lease");
    }
    lifecycleThread.join();
    expect(lifecycleAcquired.load(std::memory_order_acquire),
           "lifecycle transition should proceed after request lease release");

    {
        auto lifecycle = session.AcquireLifecycle();
        session.BeginConnect();
        session.FinishConnect(true);
    }
    {
        auto request = session.AcquireRequest();
        const uint64_t generationBeforePoison = session.GetGeneration();
        session.MarkPoisoned();
        expect(session.IsPoisoned(), "I/O failure should poison the session");
        expect(session.GetGeneration() == generationBeforePoison + 1,
               "first poison event should invalidate connection generation");
        expect(!request.isCurrent(),
               "poisoned generation should invalidate an in-flight request lease");
    }
    {
        auto rejected = session.AcquireRequest();
        expect(!rejected, "poisoned session must reject new requests");
    }
    {
        auto lifecycle = session.AcquireLifecycle();
        session.Disconnect();
    }
}

void testAgentTaskExecutorLifecycle() {
    auto& registry = AI::ToolExecutor::getInstance();
    std::mutex stateMutex;
    std::condition_variable stateCv;
    std::vector<AI::AgentToolTaskOutcome> outcomes;
    bool blockerStarted = false;
    bool releaseBlocker = false;
    bool cancellableStarted = false;
    bool cancellableFinished = false;
    int immediateExecutions = 0;
    std::thread::id executorThread;
    std::thread::id callbackThread;

    registry.registerTool(
        "test_task_immediate", "immediate", "{}",
        AI::ToolSafety::ReadOnly,
        [&](const std::string&) {
            ++immediateExecutions;
            executorThread = std::this_thread::get_id();
            return std::string(R"({"success":true})");
        });
    registry.registerTool(
        "test_task_blocker", "blocker", "{}",
        AI::ToolSafety::ReadOnly,
        [&](const std::string&, const Mem::OperationContext& context) {
            std::unique_lock<std::mutex> lock(stateMutex);
            blockerStarted = true;
            stateCv.notify_all();
            while (!releaseBlocker &&
                   !(context.cancellation &&
                     context.cancellation->load(std::memory_order_acquire))) {
                stateCv.wait_for(lock, std::chrono::milliseconds(5));
            }
            return std::string(R"({"success":true})");
        },
        AI::ToolTargetPolicy::None);
    registry.registerTool(
        "test_task_cancellable", "cancellable", "{}",
        AI::ToolSafety::ReadOnly,
        [&](const std::string&, const Mem::OperationContext& context) {
            {
                std::lock_guard<std::mutex> lock(stateMutex);
                cancellableStarted = true;
                stateCv.notify_all();
            }
            while (!(context.cancellation &&
                     context.cancellation->load(std::memory_order_acquire))) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            {
                std::lock_guard<std::mutex> lock(stateMutex);
                cancellableFinished = true;
                stateCv.notify_all();
            }
            return std::string(
                R"({"success":false,"error":{"code":"cancel_requested","message":"cancel observed","retryable":false}})");
        },
        AI::ToolTargetPolicy::None);
    registry.registerTool(
        "test_task_slow_read", "slow read", "{}",
        AI::ToolSafety::ReadOnly,
        [&](const std::string&) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            return std::string(R"({"success":true,"value":1})");
        });

    AI::AgentTaskExecutor executor(registry);
    const auto completion = [&](AI::AgentToolTaskOutcome outcome) {
        std::lock_guard<std::mutex> lock(stateMutex);
        callbackThread = std::this_thread::get_id();
        outcomes.push_back(std::move(outcome));
        stateCv.notify_all();
    };
    const auto waitFor = [&](const std::function<bool()>& predicate,
                             const char* message) {
        std::unique_lock<std::mutex> lock(stateMutex);
        if (!stateCv.wait_for(lock, std::chrono::seconds(2), predicate)) {
            throw std::runtime_error(message);
        }
    };
    const auto completionFor = [&](const std::string& runId) {
        std::lock_guard<std::mutex> lock(stateMutex);
        for (const auto& outcome : outcomes) {
            if (outcome.runId == runId) {
                return outcome.result.completion;
            }
        }
        throw std::runtime_error("missing task outcome for " + runId);
    };
    const auto enqueue = [&](const std::string& runId,
                             const std::string& tool,
                             Mem::OperationContext context = {}) {
        AI::AgentToolTask task;
        task.runId = runId;
        task.call = toolCall(runId + "-call", tool);
        task.context = std::move(context);
        return executor.enqueue(std::move(task), completion);
    };

    expect(enqueue("task-basic", "test_task_immediate"),
           "owned task executor should accept work");
    waitFor([&] { return outcomes.size() >= 1; },
            "basic task did not complete");
    expect(immediateExecutions == 1 && executorThread == callbackThread &&
               executorThread != std::this_thread::get_id(),
           "tool and callback should run on the same owned worker thread");

    {
        std::lock_guard<std::mutex> lock(stateMutex);
        blockerStarted = false;
        releaseBlocker = false;
    }
    expect(enqueue("task-block-timeout", "test_task_blocker"),
           "blocker should enqueue for queue-timeout test");
    waitFor([&] { return blockerStarted; }, "blocker did not start");
    Mem::OperationContext shortDeadline;
    shortDeadline.deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(30);
    expect(enqueue("task-queued-timeout", "test_task_immediate",
                   shortDeadline),
           "queued timeout task should enqueue");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        releaseBlocker = true;
        stateCv.notify_all();
    }
    waitFor([&] { return outcomes.size() >= 3; },
            "queue-timeout outcomes did not complete");
    expect(completionFor("task-queued-timeout") ==
               AI::ToolCompletionState::TimedOutBeforeStart &&
               immediateExecutions == 1,
           "expired queued task must not invoke its tool executor");

    {
        std::lock_guard<std::mutex> lock(stateMutex);
        blockerStarted = false;
        releaseBlocker = false;
    }
    expect(enqueue("task-block-cancel", "test_task_blocker"),
           "blocker should enqueue for cancellation test");
    waitFor([&] { return blockerStarted; },
            "cancellation blocker did not start");
    expect(enqueue("task-queued-cancel", "test_task_immediate"),
           "queued cancellation task should enqueue");
    executor.cancelRun("task-queued-cancel");
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        releaseBlocker = true;
        stateCv.notify_all();
    }
    waitFor([&] { return outcomes.size() >= 5; },
            "queued cancellation outcomes did not complete");
    expect(completionFor("task-queued-cancel") ==
               AI::ToolCompletionState::CancelledBeforeStart &&
               immediateExecutions == 1,
           "cancelled queued task must not invoke its tool executor");

    {
        std::lock_guard<std::mutex> lock(stateMutex);
        cancellableStarted = false;
        cancellableFinished = false;
    }
    expect(enqueue("task-active-cancel", "test_task_cancellable"),
           "active cancellation task should enqueue");
    waitFor([&] { return cancellableStarted; },
            "cancellable task did not start");
    executor.cancelRun("task-active-cancel");
    waitFor([&] { return outcomes.size() >= 6; },
            "active cancellation outcome did not complete");
    expect(cancellableFinished &&
               completionFor("task-active-cancel") ==
                   AI::ToolCompletionState::CancelRequested,
           "active task should observe cancellation without being detached");

    Mem::OperationContext slowDeadline;
    slowDeadline.deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    expect(enqueue("task-slow-timeout", "test_task_slow_read", slowDeadline),
           "slow read should enqueue");
    waitFor([&] { return outcomes.size() >= 7; },
            "slow timeout outcome did not complete");
    expect(completionFor("task-slow-timeout") ==
               AI::ToolCompletionState::TimedOut,
           "late read must finish on the owned worker and report timed_out");

    {
        std::lock_guard<std::mutex> lock(stateMutex);
        cancellableStarted = false;
        cancellableFinished = false;
    }
    expect(enqueue("task-shutdown", "test_task_cancellable"),
           "shutdown task should enqueue");
    waitFor([&] { return cancellableStarted; },
            "shutdown task did not start");
    expect(enqueue("task-shutdown-queued", "test_task_immediate"),
           "queued shutdown task should enqueue");
    executor.shutdown();
    expect(cancellableFinished && !executor.isAccepting() &&
               executor.pendingCount() == 0 &&
               completionFor("task-shutdown") ==
                   AI::ToolCompletionState::CancelRequested &&
               completionFor("task-shutdown-queued") ==
                   AI::ToolCompletionState::CancelledBeforeStart &&
               immediateExecutions == 1,
           "shutdown must cancel and join the active worker");
    expect(!enqueue("task-after-shutdown", "test_task_immediate"),
           "executor must reject work after shutdown");
}

void testMutationAuditPersistence() {
    const auto unique = std::chrono::steady_clock::now()
                            .time_since_epoch()
                            .count();
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        ("amem_mutation_audit_" + std::to_string(unique));
    std::filesystem::create_directories(directory);
    const std::filesystem::path filepath = directory / "mutations.jsonl";

    AI::AgentMutationAuditLog audit(filepath.string(), 64u * 1024u, 3);
    const std::string cardSecret = "audit-card-secret-91a2";
    AI::AgentMutationAuditEvent driver;
    driver.runId = "audit-driver-run";
    driver.call = toolCall(
        "audit-driver-call", "driver_initialize",
        std::string("{\"card\":\"") + cardSecret + "\"}");
    AI::applyToolCallRedaction(driver.call);
    driver.result.success = false;
    driver.result.errorMessage =
        "driver receipt was lost for " + cardSecret;
    driver.result.resultJson = json{
        {"success", false},
        {"error", {
            {"code", "completion_unknown"},
            {"message", "driver receipt was lost for " + cardSecret},
            {"retryable", false},
        }},
        {"completion", "completion_unknown"},
    }.dump();
    driver.result.completion = AI::ToolCompletionState::CompletionUnknown;
    driver.context.connectionGeneration = 7;
    driver.context.target = Mem::TargetSnapshot{42, 420, 8, 7};
    driver.safety = AI::ToolSafety::Write;
    driver.targetPolicy = AI::ToolTargetPolicy::None;
    driver.approval = AI::MutationApproval::Approved;
    driver.durationMs = 12;

    std::string appendError;
    expect(audit.append(driver, &appendError),
           "driver mutation audit should persist: " + appendError);

    const std::string luaSecret = "print('audit-lua-secret-4b6c')";
    AI::AgentMutationAuditEvent lua = driver;
    lua.runId = "audit-lua-run";
    lua.call = toolCall(
        "audit-lua-call", "lua_execute",
        std::string("{\"code\":\"") + luaSecret + "\"}");
    lua.result.success = false;
    lua.result.errorMessage = "Lua error near " + luaSecret;
    lua.result.resultJson = json{
        {"success", false},
        {"error", {
            {"code", "internal_error"},
            {"message", "Lua error near " + luaSecret},
            {"retryable", false},
        }},
        {"output", "audit-output-secret"},
        {"completion", "completed_after_cancel_request"},
    }.dump();
    lua.result.completion =
        AI::ToolCompletionState::CompletedAfterCancelRequest;
    lua.targetPolicy = AI::ToolTargetPolicy::Bound;
    lua.approval = AI::MutationApproval::AutoApproved;
    expect(audit.append(lua, &appendError),
           "Lua mutation audit should persist: " + appendError);

    AI::AgentMutationAuditEvent denied = driver;
    denied.runId = "audit-denied-run";
    denied.approval = AI::MutationApproval::Denied;
    denied.result.errorMessage = "tool execution denied by user";
    denied.result.resultJson =
        R"({"success":false,"error":{"code":"permission_denied","message":"tool execution denied by user","retryable":false},"completion":"rejected_before_start"})";
    denied.result.completion =
        AI::ToolCompletionState::RejectedBeforeStart;
    expect(audit.append(denied, &appendError),
           "denied mutation audit should persist: " + appendError);

    AI::AgentMutationAuditEvent symbol = driver;
    symbol.runId = "audit-symbol-run";
    symbol.call = toolCall(
        "audit-symbol-call", "symbol_resolve",
        R"({"module_name":"libgame.so","symbol_name":"GameInit"})");
    symbol.result.success = true;
    symbol.result.errorMessage.clear();
    symbol.result.resultJson =
        R"({"success":true,"address":"0x5100","symbol_epoch":3})";
    symbol.result.completion = AI::ToolCompletionState::Completed;
    symbol.safety = AI::ToolSafety::ReadOnly;
    symbol.targetPolicy = AI::ToolTargetPolicy::Bound;
    symbol.approval = AI::MutationApproval::NotRequired;
    expect(audit.append(symbol, &appendError) &&
               audit.recent().back().effect == "session_mutation" &&
               audit.recent().back().resourceDomain == "symbol" &&
               audit.recent().back().approval == "not_required",
           "symbol session mutation should be audited without write approval");

    AI::AgentMutationAuditEvent readOnly = driver;
    readOnly.runId = "audit-read-run";
    readOnly.safety = AI::ToolSafety::ReadOnly;
    expect(audit.append(readOnly, &appendError) &&
               audit.recent().size() == 3,
           "read-only tools must not enter the mutation audit");

    std::ifstream input(filepath, std::ios::binary);
    std::ostringstream contents;
    contents << input.rdbuf();
    const std::string persisted = contents.str();
    input.close();
    expect(!persisted.empty() &&
               persisted.find(cardSecret) == std::string::npos &&
               persisted.find(luaSecret) == std::string::npos &&
               persisted.find("audit-output-secret") == std::string::npos,
           "mutation audit must redact card, Lua code, and bulk output");

    std::istringstream lines(persisted);
    std::string firstLine;
    std::getline(lines, firstLine);
    const json firstRecord = json::parse(firstLine);
    expect(firstRecord.at("run_id") == "audit-driver-run" &&
               firstRecord.at("completion") == "completion_unknown" &&
               firstRecord.at("effect") == "connection_mutation" &&
               firstRecord.at("resource_domain") == "connection" &&
               firstRecord.at("approval") == "approved" &&
               firstRecord.at("error") == "[REDACTED]" &&
               firstRecord.at("connection_generation") == 7 &&
               firstRecord.at("target").at("process_revision") == 8 &&
               firstRecord.at("arguments").at("card").at("omitted") == true,
           "mutation audit should preserve run, target, completion, and redaction metadata");

    for (int index = 0; index < 180; ++index) {
        AI::AgentMutationAuditEvent rotated = driver;
        rotated.runId = "audit-rotation-" + std::to_string(index);
        rotated.result.errorMessage = std::string(600, 'x');
        const bool appended = audit.append(rotated, &appendError);
        expect(appended,
               "mutation audit rotation append should succeed: " + appendError);
    }
    expect(std::filesystem::exists(filepath.string() + ".1") &&
               audit.recent().size() == 3,
           "mutation audit should rotate at its bound and cap in-memory history");

    AI::AgentMutationAuditLog reloaded(filepath.string(), 64u * 1024u, 3);
    expect(!reloaded.recent().empty() && reloaded.recent().size() <= 3,
           "mutation audit should reload bounded valid JSONL records");

    const std::filesystem::path hostilePath =
        directory / "hostile_mutations.jsonl";
    {
        std::ofstream hostile(hostilePath, std::ios::binary);
        hostile << std::string(80u * 1024u, 'x') << '\n';
        hostile << "{\"schema_version\":1,\"dense\":[";
        for (size_t item = 0; item < 3000u; ++item) {
            if (item != 0) hostile << ',';
            hostile << '0';
        }
        hostile << "]}\n" << firstLine << '\n';
    }
    AI::AgentMutationAuditLog hostileReload(
        hostilePath.string(), 256u * 1024u, 3);
    expect(hostileReload.recent().size() == 1 &&
               hostileReload.recent().front().runId ==
                   "audit-driver-run",
           "mutation audit reload must skip oversized/dense lines and retain later valid records");

    auto& registry = AI::ToolExecutor::getInstance();
    std::mutex stateMutex;
    std::condition_variable stateCv;
    bool executionStarted = false;
    bool callbackReceived = false;
    bool auditVisibleBeforeCallback = false;
    AI::AgentToolTaskOutcome finalOutcome;
    registry.registerTool(
        "test_audited_write", "audited write", "{}",
        AI::ToolSafety::Write,
        [&](const std::string&, const Mem::OperationContext& context) {
            {
                std::lock_guard<std::mutex> lock(stateMutex);
                executionStarted = true;
                stateCv.notify_all();
            }
            while (!(context.cancellation &&
                     context.cancellation->load(std::memory_order_acquire))) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            return std::string(
                R"({"success":true,"address":"0x1000","written_bytes":4,"completion":"completed"})");
        },
        AI::ToolTargetPolicy::Bound);

    const std::filesystem::path executorPath =
        directory / "executor_mutations.jsonl";
    AI::AgentMutationAuditLog executorAudit(
        executorPath.string(), 64u * 1024u, 10);
    AI::AgentTaskExecutor executor(registry, &executorAudit);
    AI::AgentToolTask task;
    task.runId = "stopped-agent-run";
    task.call = toolCall(
        "stopped-agent-call", "test_audited_write",
        R"({"address":"0x1000","data_hex":"DE AD BE EF"})");
    task.context.connectionGeneration = 11;
    task.context.target = Mem::TargetSnapshot{77, 770, 12, 11};
    task.approval = AI::MutationApproval::Approved;
    expect(executor.enqueue(
               std::move(task),
               [&](AI::AgentToolTaskOutcome outcome) {
                   std::lock_guard<std::mutex> lock(stateMutex);
                   auditVisibleBeforeCallback = !executorAudit.recent().empty();
                   finalOutcome = std::move(outcome);
                   callbackReceived = true;
                   stateCv.notify_all();
               }),
           "audited write should enqueue");
    {
        std::unique_lock<std::mutex> lock(stateMutex);
        expect(stateCv.wait_for(
                   lock, std::chrono::seconds(2),
                   [&] { return executionStarted; }),
               "audited write did not start");
    }
    executor.cancelRun("stopped-agent-run");
    {
        std::unique_lock<std::mutex> lock(stateMutex);
        expect(stateCv.wait_for(
                   lock, std::chrono::seconds(2),
                   [&] { return callbackReceived; }),
               "audited write callback did not complete");
    }
    executor.shutdown();

    const auto executorEntries = executorAudit.recent();
    expect(auditVisibleBeforeCallback && finalOutcome.auditPersisted &&
               finalOutcome.result.completion ==
                   AI::ToolCompletionState::CompletedAfterCancelRequest &&
               executorEntries.size() == 1 &&
               executorEntries.front().runId == "stopped-agent-run" &&
               executorEntries.front().approval == "approved" &&
               executorEntries.front().effect == "target_mutation" &&
               executorEntries.front().completion ==
                   "completed_after_cancel_request",
           "stopped write must be durably audited before UI callback delivery");

    std::ifstream executorInput(executorPath, std::ios::binary);
    std::ostringstream executorContents;
    executorContents << executorInput.rdbuf();
    expect(executorContents.str().find("DE AD BE EF") == std::string::npos,
           "raw write bytes must not be copied into the mutation audit");
    executorInput.close();

    std::error_code removeError;
    std::filesystem::remove_all(directory, removeError);
}

void testAgentAdapter() {
    FakeBackend backend;
    Mem::MemService service(backend);
    AI::AgentMemTools tools(service);
    const Mem::OperationContext connectionContext =
        service.captureContext(false);
    const Mem::OperationContext targetContext = service.captureContext(true);

    const json status = json::parse(tools.status("{}", connectionContext));
    expect(status.at("success").get<bool>() &&
               status.at("connection_generation").get<uint64_t>() == 1,
           "status adapter should expose structured success and generation");

    const json processes = json::parse(
        tools.processList(
            R"({"filter":"game","count":1})", connectionContext));
    expect(processes.at("success").get<bool>() &&
               processes.at("truncated").get<bool>(),
           "process adapter should expose pagination metadata");

    const json strictAddress = json::parse(
        tools.memoryRead(
            R"({"address":"1000","size":4})", targetContext));
    expect(!strictAddress.at("success").get<bool>() &&
               strictAddress.at("error").at("code") == "invalid_argument",
           "canonical memory_read should reject missing 0x prefix");

    const json read = json::parse(
        tools.memoryRead(
            R"({"address":"0x1000","size":4})", targetContext));
    expect(read.at("success").get<bool>() &&
               read.at("hex") == "DEADBEEF" &&
               read.at("data") == "DE AD BE EF",
           "memory adapter should expose compact and spaced hex");

    const json integerRead = json::parse(
        tools.memoryRead(
            R"({"address":4096,"size":4})", targetContext));
    expect(!integerRead.at("success").get<bool>() &&
               integerRead.at("error").at("code") == "invalid_argument",
           "memory_read should reject integer addresses");

    const json strictValueAddress = json::parse(
        tools.memoryReadValue(
            R"({"address":"1000","data_type":"dword"})",
            targetContext));
    expect(!strictValueAddress.at("success").get<bool>() &&
               strictValueAddress.at("error").at("code") ==
                   "invalid_argument",
           "canonical memory_read_value should require a 0x address");

    backend.memory = {0x78, 0x56, 0x34, 0x12};
    const json typedRead = json::parse(
        tools.memoryReadValue(
            R"({"address":"0x1000","data_type":"int32"})",
            targetContext));
    expect(typedRead.at("success").get<bool>() &&
               typedRead.at("data_type") == "dword" &&
               typedRead.at("value").get<uint64_t>() == 0x12345678 &&
               typedRead.at("value_text") == "305419896" &&
               typedRead.at("value_hex") == "0x12345678",
           "typed read adapter should expose normalized exact values");

    const json integerTypedRead = json::parse(
        tools.memoryReadValue(
            R"({"address":4096,"data_type":"dword"})",
            targetContext));
    expect(!integerTypedRead.at("success").get<bool>() &&
               integerTypedRead.at("error").at("code") ==
                   "invalid_argument",
           "memory_read_value should reject integer addresses");

    const json strictWriteAddress = json::parse(
        tools.memoryWrite(
            R"({"address":"2000","data_hex":"90 90"})",
            targetContext));
    expect(!strictWriteAddress.at("success").get<bool>() &&
               strictWriteAddress.at("error").at("code") ==
                   "invalid_argument",
           "canonical memory_write should require a 0x-prefixed address");

    const json write = json::parse(
        tools.memoryWrite(
            R"({"address":"0x2000","data_hex":"90 90"})",
            targetContext));
    expect(write.at("success").get<bool>() &&
               write.at("written_bytes") == 2 &&
               write.at("completion") == "completed",
           "canonical memory_write should expose a confirmed write receipt");

    const json legacyShapeWrite = json::parse(
        tools.memoryWrite(
            R"({"address":8192,"hex_string":"C0 03 5F D6"})",
            targetContext));
    expect(!legacyShapeWrite.at("success").get<bool>() &&
               legacyShapeWrite.at("error").at("code") ==
                   "invalid_argument",
           "memory_write should reject legacy integer and hex_string arguments");

    const json strictTypedWriteAddress = json::parse(
        tools.memoryWriteValue(
            R"({"address":"2000","value":-1,"data_type":"word"})",
            targetContext));
    expect(!strictTypedWriteAddress.at("success").get<bool>() &&
               strictTypedWriteAddress.at("error").at("code") ==
                   "invalid_argument",
           "canonical memory_write_value should require a 0x address");

    const json typedWrite = json::parse(
        tools.memoryWriteValue(
            R"({"address":"0x2000","value":-1,"data_type":"int16"})",
            targetContext));
    expect(typedWrite.at("success").get<bool>() &&
               typedWrite.at("data_type") == "word" &&
               typedWrite.at("hex") == "FFFF" &&
               typedWrite.at("completion") == "completed" &&
               backend.lastWriteBytes ==
                   std::vector<unsigned char>({0xFF, 0xFF}),
           "typed write adapter should normalize and encode scalar values");

    const json legacyShapeTypedWrite = json::parse(
        tools.memoryWriteValue(
            R"({"address":8192,"value":"0x7F","value_type":"int8"})",
            targetContext));
    expect(!legacyShapeTypedWrite.at("success").get<bool>() &&
               legacyShapeTypedWrite.at("error").at("code") ==
                   "invalid_argument",
           "memory_write_value should reject legacy integer and value_type arguments");

    const json modulePage = json::parse(
        tools.moduleList(
            R"({"filter":"game","count":1})",
            targetContext));
    expect(modulePage.at("success").get<bool>() &&
               modulePage.at("total") == 2 &&
               modulePage.at("count") == 1 &&
               modulePage.at("truncated").get<bool>() &&
               modulePage.at("next_cursor") == 1 &&
               modulePage.at("modules").at(0).at("base") == "0x5000",
           "module_list adapter should expose structured pagination");

    backend.modules.insert(
        backend.modules.begin(),
        {0x8000, 0x1000, 2, 1, "/data/app/libgame.so"});

    const json module = json::parse(
        tools.moduleResolve(
            R"({"module_name":"libgame.so"})",
            targetContext));
    expect(module.at("success").get<bool>() &&
               module.at("module") == "/data/app/libgame.so" &&
               module.at("base") == "0x5000" &&
               module.at("size") == 0x3000,
           "module_resolve adapter should collapse same-path mappings and return the lowest base");

    const json legacyShapeModule = json::parse(
        tools.moduleResolve(
            R"({"name":"libc.so"})",
            targetContext));
    expect(!legacyShapeModule.at("success").get<bool>() &&
               legacyShapeModule.at("error").at("code") ==
                   "invalid_argument",
           "module_resolve should reject the legacy name argument");

    const json ambiguousModule = json::parse(
        tools.moduleResolve(
            R"({"module_name":"libgame"})",
            targetContext));
    expect(!ambiguousModule.at("success").get<bool>() &&
               ambiguousModule.at("error").at("code") ==
                   "invalid_argument",
           "module_resolve adapter should preserve ambiguity errors");

    backend.pointerMemory = {
        {0x5010, 0x6000},
        {0x6020, 0x7000},
    };
    const json pointer = json::parse(
        tools.pointerResolve(
            R"({"module_name":"libgame.so","base_offset":"0x10","offsets":["0x20"],"deref_final":true})",
            targetContext));
    expect(pointer.at("success").get<bool>() &&
               pointer.at("module") == "/data/app/libgame.so" &&
               pointer.at("start_address") == "0x5010" &&
               pointer.at("address") == "0x7000" &&
               pointer.at("dereference_count") == 2 &&
               pointer.at("dereferenced_final").get<bool>(),
           "pointer_resolve adapter should expose the transactional result");

    const json strictPointerOffset = json::parse(
        tools.pointerResolve(
            R"({"module_name":"libgame.so","base_offset":"10","offsets":[]})",
            targetContext));
    expect(!strictPointerOffset.at("success").get<bool>() &&
               strictPointerOffset.at("error").at("code") ==
                   "invalid_argument",
           "canonical pointer_resolve should require 0x-prefixed offsets");

    const json legacyShapePointer = json::parse(
        tools.pointerResolve(
            R"({"module":"libgame.so","base_offset":16,"offsets":[32],"deref_final":false})",
            targetContext));
    expect(!legacyShapePointer.at("success").get<bool>() &&
               legacyShapePointer.at("error").at("code") ==
                   "invalid_argument",
           "pointer_resolve should reject legacy module and integer offsets");

    const json legacyEmptyPointer = json::parse(
        tools.pointerResolve(
            R"({"module":"libgame.so","base_offset":16,"offsets":[],"deref_final":true})",
            targetContext));
    expect(!legacyEmptyPointer.at("success").get<bool>() &&
               legacyEmptyPointer.at("error").at("code") ==
                   "invalid_argument",
           "pointer_resolve should reject the legacy empty-chain shape");

    const json strictDisassemblyAddress = json::parse(
        tools.disassemble(
            R"({"address":"8000","count":1})", targetContext));
    expect(!strictDisassemblyAddress.at("success").get<bool>() &&
               strictDisassemblyAddress.at("error").at("code") ==
                   "invalid_argument",
           "canonical disassemble should require a 0x-prefixed address");

    backend.memory = {
        0xC0, 0x03, 0x5F, 0xD6,
        0x1F, 0x20, 0x03, 0xD5,
    };
    const json disassembly = json::parse(
        tools.disassemble(
            R"({"address":"0x8000","count":2})", targetContext));
    expect(disassembly.at("success").get<bool>() &&
               disassembly.at("architecture") == "arm64" &&
               disassembly.at("count") == 2 &&
               !disassembly.at("decoded").get<bool>() &&
               disassembly.at("raw_bytes") == "C0035FD61F2003D5" &&
               disassembly.at("instructions").at(0).at("encoding") ==
                   "0xD65F03C0",
           "disassemble adapter should expose bounded canonical encodings");

    const json symbolPage = json::parse(
        tools.symbolList(
            R"({"module_name":"libgame.so","count":2})",
            targetContext));
    expect(symbolPage.at("success").get<bool>() &&
               symbolPage.at("module") == "/data/app/libgame.so" &&
               symbolPage.at("symbol_epoch") == 1 &&
               symbolPage.at("total") == 3 &&
               symbolPage.at("count") == 2 &&
               symbolPage.at("truncated").get<bool>() &&
               symbolPage.at("next_cursor") == 2 &&
               symbolPage.at("symbols").at(0).at("address") == "0x5100",
           "symbol_list adapter should expose a module-bound session page");

    const json missingSymbolEpoch = json::parse(
        tools.symbolList(
            R"({"module_name":"libgame.so","offset":2,"count":2})",
            targetContext));
    expect(!missingSymbolEpoch.at("success").get<bool>() &&
               missingSymbolEpoch.at("error").at("code") ==
                   "invalid_argument",
           "symbol_list continuation should require symbol_epoch");

    const json symbolContinuation = json::parse(
        tools.symbolList(
            R"({"module_name":"libgame.so","symbol_epoch":1,"offset":2,"count":2})",
            targetContext));
    expect(symbolContinuation.at("success").get<bool>() &&
               symbolContinuation.at("symbol_epoch") == 2 &&
               symbolContinuation.at("count") == 1 &&
               !symbolContinuation.at("truncated").get<bool>(),
           "symbol_list adapter should advance the continuation epoch");

    const json symbol = json::parse(
        tools.symbolResolve(
            R"({"module_name":"libgame.so","symbol_name":"GameUpdate"})",
            targetContext));
    expect(symbol.at("success").get<bool>() &&
               symbol.at("module") == "/data/app/libgame.so" &&
               symbol.at("symbol") == "GameUpdate" &&
               symbol.at("address") == "0x5200" &&
               symbol.at("symbol_epoch") == 3,
           "symbol_resolve adapter should expose canonical module and address data");

    const json strictBreakpointAddress = json::parse(
        tools.breakpointSet(
            R"({"address":"7000","access":"write","size":4})",
            targetContext));
    expect(!strictBreakpointAddress.at("success").get<bool>() &&
               strictBreakpointAddress.at("error").at("code") ==
                   "invalid_argument",
           "canonical breakpoint_set should require a 0x-prefixed address");

    const json breakpointSet = json::parse(
        tools.breakpointSet(
            R"({"address":"0x7000","access":"read_write","size":8})",
            targetContext));
    expect(breakpointSet.at("success").get<bool>() &&
               breakpointSet.at("action") == "set" &&
               breakpointSet.at("confirmed").get<bool>() &&
               breakpointSet.at("access") == "read_write" &&
               breakpointSet.at("size") == 8 &&
               breakpointSet.at("completion") == "completed",
           "breakpoint_set adapter should expose a confirmed canonical receipt");

    const json invalidExecuteBreakpoint = json::parse(
        tools.breakpointSet(
            R"({"address":"0x7000","access":"execute","size":8})",
            targetContext));
    expect(!invalidExecuteBreakpoint.at("success").get<bool>() &&
               invalidExecuteBreakpoint.at("error").at("code") ==
                   "invalid_argument",
           "breakpoint_set adapter should reject ignored execute sizes");

    const json breakpointHits = json::parse(
        tools.breakpointHits(
            R"({"address":"0x7000","count":2})",
            targetContext));
    expect(breakpointHits.at("success").get<bool>() &&
               breakpointHits.at("available") == 3 &&
               breakpointHits.at("count") == 2 &&
               breakpointHits.at("dropped") == 1 &&
               breakpointHits.at("truncated").get<bool>() &&
               !breakpointHits.contains("next_cursor") &&
               breakpointHits.at("hits").at(0).at("hit_time") == "200" &&
               breakpointHits.at("hits").at(0).at("registers").at(1) ==
                   "0xB1",
           "breakpoint_hits adapter should expose the newest precision-safe batch without a false cursor");

    const int batchCallsBeforeOffset = backend.breakpointHitBatchCalls;
    const json invalidBreakpointOffset = json::parse(
        tools.breakpointHits(
            R"({"address":"0x7000","offset":1,"count":2})",
            targetContext));
    expect(!invalidBreakpointOffset.at("success").get<bool>() &&
               invalidBreakpointOffset.at("error").at("code") ==
                   "invalid_argument" &&
               backend.breakpointHitBatchCalls == batchCallsBeforeOffset,
           "breakpoint_hits must reject false continuation offsets before backend access");

    const json breakpointRemove = json::parse(
        tools.breakpointRemove(
            R"({"address":"0x7000"})", targetContext));
    const json breakpointSuspend = json::parse(
        tools.breakpointSuspend(
            R"({"address":"0x7000"})", targetContext));
    const json breakpointResume = json::parse(
        tools.breakpointResume(
            R"({"address":"0x7000"})", targetContext));
    expect(breakpointRemove.at("action") == "remove" &&
               breakpointSuspend.at("action") == "suspend" &&
               breakpointResume.at("action") == "resume",
           "breakpoint adapters should preserve canonical action receipts");

    const json strictScanRange = json::parse(
        tools.scanStart(
            R"({"mode":"exact","data_type":"dword","value":42,"start":"1000"})",
            targetContext));
    expect(!strictScanRange.at("success").get<bool>() &&
               strictScanRange.at("error").at("code") ==
                   "invalid_argument",
           "canonical scan_start should require 0x-prefixed ranges");

    const json ambiguousUnknownScan = json::parse(
        tools.scanStart(
            R"({"mode":"unknown","data_type":"dword","value":42})",
            targetContext));
    expect(!ambiguousUnknownScan.at("success").get<bool>() &&
               ambiguousUnknownScan.at("error").at("code") ==
                   "invalid_argument",
           "scan_start should reject ignored values in unknown mode");

    const json scanStart = json::parse(
        tools.scanStart(
            R"({"mode":"exact","data_type":"dword","value":42,"memory_type":"c_heap","start":"0x1000","end":"0x9000"})",
            targetContext));
    const uint64_t scanEpoch = scanStart.at("scan_epoch").get<uint64_t>();
    expect(scanStart.at("success").get<bool>() && scanEpoch == 2 &&
               scanStart.at("result_count") == 3 &&
               scanStart.at("completion") == "completed" &&
               backend.lastScanStart &&
               backend.lastScanStart->value ==
                   std::vector<unsigned char>({42, 0, 0, 0}),
           "scan_start adapter should normalize one complete scan request");

    const json scanPage = json::parse(
        tools.scanResults(
            std::string("{\"scan_epoch\":") +
                std::to_string(scanEpoch) + ",\"count\":2}",
            targetContext));
    expect(scanPage.at("success").get<bool>() &&
               scanPage.at("total") == 3 && scanPage.at("count") == 2 &&
               scanPage.at("next_cursor") == 2 &&
               scanPage.at("items").at(0).at("address") == "0x1000" &&
               scanPage.at("items").at(0).at("value_text") == "10",
           "scan_results adapter should expose bounded structured pages");

    const json staleScanRefine = json::parse(
        tools.scanRefine(
            std::string("{\"scan_epoch\":") +
                std::to_string(scanEpoch - 1) +
                ",\"mode\":\"changed\",\"data_type\":\"dword\"}",
            targetContext));
    expect(!staleScanRefine.at("success").get<bool>() &&
               staleScanRefine.at("error").at("code") ==
                   "scan_session_changed",
           "scan_refine adapter should reject stale epochs");

    const json scanRefine = json::parse(
        tools.scanRefine(
            std::string("{\"scan_epoch\":") +
                std::to_string(scanEpoch) +
                ",\"mode\":\"changed\",\"data_type\":\"dword\"}",
            targetContext));
    const uint64_t refinedEpoch =
        scanRefine.at("scan_epoch").get<uint64_t>();
    expect(scanRefine.at("success").get<bool>() &&
               refinedEpoch == scanEpoch + 1 &&
               scanRefine.at("mode") == "changed",
           "scan_refine adapter should return the replacement epoch");

    const json scanClear = json::parse(
        tools.scanClear(
            std::string("{\"scan_epoch\":") +
                std::to_string(refinedEpoch) + "}",
            targetContext));
    expect(scanClear.at("success").get<bool>() &&
               scanClear.at("cleared").get<bool>() &&
               scanClear.at("cleared_epoch") == refinedEpoch,
           "scan_clear adapter should confirm the expected session was cleared");

    backend.connected = false;
    const json disconnected = json::parse(
        tools.processList("{}", connectionContext));
    expect(!disconnected.at("success").get<bool>() &&
               disconnected.at("error").at("code") == "not_connected",
           "adapter should preserve structured service errors");
}

AI::ToolCall toolCall(std::string id,
                      std::string name,
                      std::string arguments) {
    AI::ToolCall call;
    call.id = std::move(id);
    call.name = std::move(name);
    call.arguments = std::move(arguments);
    return call;
}

void registerContextTestTools(AI::AgentMemTools& tools) {
    auto& registry = AI::ToolExecutor::getInstance();
    registry.registerTool(
        "test_context_status", "status", "{}", AI::ToolSafety::ReadOnly,
        [&tools](const std::string& args,
                 const Mem::OperationContext& context) {
            return tools.status(args, context);
        },
        AI::ToolTargetPolicy::None);
    registry.registerTool(
        "test_context_process_list", "process list", "{}",
        AI::ToolSafety::ReadOnly,
        [&tools](const std::string& args,
                 const Mem::OperationContext& context) {
            return tools.processList(args, context);
        },
        AI::ToolTargetPolicy::None);
    registry.registerTool(
        "test_context_process_open", "process open", "{}",
        AI::ToolSafety::Write,
        [&tools](const std::string& args,
                 const Mem::OperationContext& context) {
            return tools.processOpen(args, context);
        },
        AI::ToolTargetPolicy::Selection);
    registry.registerTool(
        "test_context_memory_read", "memory read", "{}",
        AI::ToolSafety::ReadOnly,
        [&tools](const std::string& args,
                 const Mem::OperationContext& context) {
            return tools.memoryRead(args, context);
        },
        AI::ToolTargetPolicy::Bound);
    registry.registerTool(
        "test_context_memory_write", "memory write", "{}",
        AI::ToolSafety::Write,
        [&tools](const std::string& args,
                 const Mem::OperationContext& context) {
            return tools.memoryWrite(args, context);
        },
        AI::ToolTargetPolicy::Bound);
}

bool outcomeContains(const AI::AgentRunner::Outcome& outcome,
                     const std::string& text) {
    for (const auto& message : outcome.messages) {
        if (message.content.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

void testApprovalContextInvalidation() {
    AI::AgentController::ToolConfig config;

    {
        FakeBackend backend;
        Mem::MemService service(backend);
        AI::AgentMemTools tools(service);
        registerContextTestTools(tools);
        AI::AgentController controller(service);
        controller.resetForNewRun();

        const AI::ToolCall open = toolCall(
            "open-target-switch", "test_context_process_open", R"({"pid":84})");
        auto waiting = controller.beginToolCalls({open}, config);
        expect(waiting.kind == AI::AgentRunner::OutcomeKind::NeedsConfirmation,
               "process open should wait for approval");

        backend.target.processRevision += 2;
        auto rejected = controller.approvePendingTool(config);
        expect(rejected.kind == AI::AgentRunner::OutcomeKind::ReadyForFollowUp &&
                   outcomeContains(rejected, "target_changed"),
               "target switch while awaiting approval must reject execution");
    }

    {
        FakeBackend backend;
        Mem::MemService service(backend);
        AI::AgentMemTools tools(service);
        registerContextTestTools(tools);
        AI::AgentController controller(service);
        controller.resetForNewRun();

        const AI::ToolCall open = toolCall(
            "open-reconnect", "test_context_process_open", R"({"pid":84})");
        auto waiting = controller.beginToolCalls({open}, config);
        expect(waiting.kind == AI::AgentRunner::OutcomeKind::NeedsConfirmation,
               "process open should wait for approval before reconnect test");

        ++backend.generation;
        auto rejected = controller.approvePendingTool(config);
        expect(rejected.kind == AI::AgentRunner::OutcomeKind::ReadyForFollowUp &&
                   outcomeContains(rejected, "connection_changed"),
               "reconnect while awaiting approval must reject execution");
    }

    {
        FakeBackend backend;
        Mem::MemService service(backend);
        AI::AgentMemTools tools(service);
        registerContextTestTools(tools);
        AI::AgentController controller(service);
        controller.resetForNewRun();

        const AI::ToolCall open = toolCall(
            "open-before-send", "test_context_process_open", R"({"pid":84})");
        auto waiting = controller.beginToolCalls({open}, config);
        expect(waiting.kind == AI::AgentRunner::OutcomeKind::NeedsConfirmation,
               "process open should wait for approval before send-time test");
        auto approved = controller.approvePendingTool(config);
        expect(approved.kind == AI::AgentRunner::OutcomeKind::NeedsExecution,
               "stable approved target should be released for execution");

        backend.target.processRevision += 2;
        const AI::ToolResult rejected =
            AI::ToolExecutor::getInstance().execute(
                open, controller.operationContext());
        expect(!rejected.success &&
                   rejected.resultJson.find("target_changed") !=
                       std::string::npos,
               "target switch after approval but before send must be rejected");
    }

    {
        FakeBackend backend;
        Mem::MemService service(backend);
        AI::AgentMemTools tools(service);
        registerContextTestTools(tools);
        AI::AgentController controller(service);
        controller.resetForNewRun();

        const AI::ToolCall write = toolCall(
            "write-before-send",
            "test_context_memory_write",
            R"({"address":"0x2000","data_hex":"90 90"})");
        auto waiting = controller.beginToolCalls({write}, config);
        expect(waiting.kind == AI::AgentRunner::OutcomeKind::NeedsConfirmation,
               "memory write should wait for approval");
        auto approved = controller.approvePendingTool(config);
        expect(approved.kind == AI::AgentRunner::OutcomeKind::NeedsExecution,
               "approved memory write should be released while target is current");

        backend.target.processRevision += 2;
        const AI::ToolResult rejected =
            AI::ToolExecutor::getInstance().execute(
                write, controller.operationContext());
        expect(!rejected.success && backend.writeCalls == 0 &&
                   rejected.resultJson.find("target_changed") !=
                       std::string::npos,
               "memory write must revalidate target before backend send");
    }
}

void testProcessOpenAdvancesRunTarget() {
    FakeBackend backend;
    Mem::MemService service(backend);
    AI::AgentMemTools tools(service);
    registerContextTestTools(tools);
    AI::AgentController controller(service);
    AI::AgentController::ToolConfig config;
    controller.resetForNewRun();

    const AI::ToolCall open = toolCall(
        "open-batch", "test_context_process_open", R"({"pid":84})");
    const AI::ToolCall read = toolCall(
        "read-batch", "test_context_memory_read",
        R"({"address":"0x1000","size":4})");

    auto waiting = controller.beginToolCalls({open, read}, config);
    expect(waiting.kind == AI::AgentRunner::OutcomeKind::NeedsConfirmation,
           "target selection should block the tool batch for approval");

    auto openReady = controller.approvePendingTool(config);
    expect(openReady.kind == AI::AgentRunner::OutcomeKind::NeedsExecution &&
               openReady.toolCallToExecute &&
               openReady.toolCallToExecute->name == open.name,
           "approved process open should be released for execution");

    AI::ToolResult openResult = AI::ToolExecutor::getInstance().execute(
        open, controller.operationContext());
    expect(openResult.success && openResult.selectedTarget &&
               openResult.selectedTarget->pid == 84,
           "process open should return a structured selected target");

    auto readReady = controller.completeToolExecution(
        open, openResult, 1, config);
    expect(readReady.kind == AI::AgentRunner::OutcomeKind::NeedsExecution &&
               readReady.toolCallToExecute &&
               readReady.toolCallToExecute->name == read.name,
           "successful process open should release the next bound tool");
    expect(controller.snapshot().context.operation.target &&
               controller.snapshot().context.operation.target->pid == 84,
           "process open must explicitly advance the run target snapshot");

    AI::ToolResult readResult = AI::ToolExecutor::getInstance().execute(
        read, controller.operationContext());
    expect(readResult.success,
           "memory read should use the target selected earlier in the batch");
    auto complete = controller.completeToolExecution(
        read, readResult, 1, config);
    expect(complete.kind == AI::AgentRunner::OutcomeKind::ReadyForFollowUp,
           "successful open/read batch should complete normally");
}

void testNonTargetToolsAndStaleResult() {
    AI::AgentController::ToolConfig config;

    {
        FakeBackend backend;
        Mem::MemService service(backend);
        AI::AgentMemTools tools(service);
        registerContextTestTools(tools);
        AI::AgentController controller(service);
        controller.resetForNewRun();

        backend.target.processRevision += 2;
        const AI::ToolCall status =
            toolCall("status-non-target", "test_context_status");
        const AI::ToolCall list =
            toolCall("list-non-target", "test_context_process_list");
        auto statusReady = controller.beginToolCalls({status, list}, config);
        expect(statusReady.kind == AI::AgentRunner::OutcomeKind::NeedsExecution &&
                   statusReady.toolCallToExecute &&
                   statusReady.toolCallToExecute->name == status.name,
               "non-target status should ignore a target-only change");

        const AI::ToolResult statusResult =
            AI::ToolExecutor::getInstance().execute(
                status, controller.operationContext());
        expect(statusResult.success, "status should execute on the run generation");
        auto listReady = controller.completeToolExecution(
            status, statusResult, 1, config);
        expect(listReady.kind == AI::AgentRunner::OutcomeKind::NeedsExecution &&
                   listReady.toolCallToExecute &&
                   listReady.toolCallToExecute->name == list.name,
               "process list should follow status despite target change");

        const AI::ToolResult listResult =
            AI::ToolExecutor::getInstance().execute(
                list, controller.operationContext());
        expect(listResult.success, "process list should remain connection-bound only");
        auto complete = controller.completeToolExecution(
            list, listResult, 1, config);
        expect(complete.kind == AI::AgentRunner::OutcomeKind::ReadyForFollowUp,
               "non-target tool batch should complete normally");
    }

    {
        FakeBackend backend;
        Mem::MemService service(backend);
        AI::AgentMemTools tools(service);
        registerContextTestTools(tools);
        AI::AgentController controller(service);
        controller.resetForNewRun();

        const AI::ToolCall read = toolCall(
            "read-stale", "test_context_memory_read",
            R"({"address":"0x1000","size":4})");
        auto ready = controller.beginToolCalls({read}, config);
        expect(ready.kind == AI::AgentRunner::OutcomeKind::NeedsExecution,
               "bound read should be released while its target is current");

        const AI::ToolResult successfulRead =
            AI::ToolExecutor::getInstance().execute(
                read, controller.operationContext());
        expect(successfulRead.success,
               "read should initially complete against the expected target");
        backend.target.processRevision += 2;

        auto rejected = controller.completeToolExecution(
            read, successfulRead, 1, config);
        expect(rejected.kind == AI::AgentRunner::OutcomeKind::ReadyForFollowUp &&
                   outcomeContains(rejected, "target_changed") &&
                   outcomeContains(rejected, "stale_result_was_success"),
               "late success must be rejected but retained for audit after target change");
    }
}

} // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"address contract", &testAddressContract},
        {"scalar value codec", &testScalarValueCodec},
        {"status and generation", &testStatusAndConnectionGeneration},
        {"connection lifecycle service", &testConnectionLifecycleService},
        {"driver initialization and secret redaction",
         &testDriverInitializationAndSecretRedaction},
        {"process pagination and open", &testProcessPaginationAndOpen},
        {"memory target validation", &testMemoryReadTargetValidation},
        {"memory batch service", &testMemoryBatchService},
        {"memory write completion contract", &testMemoryWriteCompletionContract},
        {"freeze mutation contract", &testFreezeMutationContract},
        {"typed memory service", &testTypedMemoryService},
        {"module list and resolve", &testModuleListAndResolve},
        {"pointer resolve transaction", &testPointerResolveTransaction},
        {"disassembly service", &testDisassemblyService},
        {"symbol session service", &testSymbolSessionService},
        {"breakpoint service", &testBreakpointService},
        {"scan session service", &testScanSessionService},
        {"agent adapter", &testAgentAdapter},
        {"hidden tool registration", &testHiddenToolRegistration},
        {"tool schema validation matrix", &testToolSchemaValidationMatrix},
        {"agent runner approval matrix", &testAgentRunnerApprovalMatrix},
        {"tool JSON complexity limits", &testToolJsonComplexityLimits},
        {"tool result output limit", &testToolResultLimit},
        {"retired tool history downgrade", &testRetiredToolHistoryDowngrade},
        {"tool history pairing", &testToolHistoryPairing},
        {"device session lifecycle", &testDeviceSessionLifecycle},
        {"agent task executor lifecycle", &testAgentTaskExecutorLifecycle},
        {"mutation audit persistence", &testMutationAuditPersistence},
        {"approval context invalidation", &testApprovalContextInvalidation},
        {"process open advances run target", &testProcessOpenAdvancesRunTarget},
        {"non-target and stale result handling", &testNonTargetToolsAndStaleResult},
    };

    int failed = 0;
    for (const auto& test : tests) {
        try {
            test.second();
            std::cout << "[PASS] " << test.first << '\n';
        } catch (const std::exception& error) {
            ++failed;
            std::cerr << "[FAIL] " << test.first << ": " << error.what()
                      << '\n';
        }
    }

    if (failed != 0) {
        std::cerr << failed << " test group(s) failed\n";
        return 1;
    }
    std::cout << tests.size() << " test groups passed\n";
    return 0;
}
