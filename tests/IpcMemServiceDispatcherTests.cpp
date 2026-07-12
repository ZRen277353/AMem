#include "ipc/IpcMemServiceDispatcher.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using json = nlohmann::json;
using namespace std::chrono_literals;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class StubMemService final : public Mem::IMemService {
public:
    Mem::OperationContext current{
        7, Mem::TargetSnapshot{42, 420, 2, 7}, nullptr,
        (std::chrono::steady_clock::time_point::max)()};
    std::unordered_map<std::string, int> calls;
    Mem::OperationContext lastContext;

    Mem::OperationContext captureContext(bool includeTarget) const override {
        Mem::OperationContext result = current;
        if (!includeTarget) {
            result.target.reset();
        }
        result.cancellation.reset();
        result.deadline =
            (std::chrono::steady_clock::time_point::max)();
        return result;
    }

    Mem::Result<Mem::Status> status(
        const Mem::OperationContext& context) override {
        record("status", context);
        Mem::Status value;
        value.connected = true;
        value.connectionGeneration = current.connectionGeneration;
        value.target = *current.target;
        value.processName = "com.example.game";
        return Mem::Result<Mem::Status>::success(std::move(value), 2);
    }

    Mem::Result<Mem::ProcessPage> listProcesses(
        const Mem::OperationContext& context,
        const Mem::ProcessListRequest&) override {
        record("listProcesses", context);
        Mem::ProcessPage value;
        value.items.push_back({42, "com.example.game"});
        value.total = 1;
        return Mem::Result<Mem::ProcessPage>::success(std::move(value), 3);
    }

    Mem::Result<Mem::MemoryBlock> readMemory(
        const Mem::OperationContext& context,
        const Mem::MemoryReadRequest& request) override {
        record("readMemory", context);
        if (context.cancellation &&
            context.cancellation->load(std::memory_order_acquire)) {
            return Mem::Result<Mem::MemoryBlock>::failure(
                Mem::ErrorCode::CancelRequested,
                "read cancellation reached MemService", false, 1);
        }
        if (std::chrono::steady_clock::now() >= context.deadline) {
            return Mem::Result<Mem::MemoryBlock>::failure(
                Mem::ErrorCode::Timeout, "read deadline expired", true, 1);
        }
        Mem::MemoryBlock value;
        value.address = request.address;
        value.bytes.assign(request.size, 0x5a);
        value.target = *current.target;
        return Mem::Result<Mem::MemoryBlock>::success(std::move(value), 4);
    }

    Mem::Result<Mem::ScalarValue> readValue(
        const Mem::OperationContext& context,
        const Mem::ValueReadRequest& request) override {
        record("readValue", context);
        Mem::ScalarValue value;
        value.address = request.address;
        value.type = request.type;
        value.bytes = {1, 0, 0, 0};
        value.target = *current.target;
        return Mem::Result<Mem::ScalarValue>::success(std::move(value), 4);
    }

#define STUB_METHOD(method, resultType, requestType)                         \
    Mem::Result<Mem::resultType> method(                                    \
        const Mem::OperationContext& context,                               \
        const Mem::requestType&) override {                                 \
        return unsupported<Mem::resultType>(#method, context);              \
    }

    STUB_METHOD(initializeDriver, DriverInitializationReceipt,
                DriverInitializeRequest)
    STUB_METHOD(openProcess, OpenProcessResult, OpenProcessRequest)
    STUB_METHOD(listModules, ModulePage, ModuleListRequest)
    STUB_METHOD(resolveModule, ResolvedModule, ModuleResolveRequest)
    STUB_METHOD(resolvePointer, PointerResolution, PointerResolveRequest)
    STUB_METHOD(disassemble, DisassemblyBlock, DisassemblyRequest)
    STUB_METHOD(resolveSymbol, ResolvedSymbol, SymbolResolveRequest)
    STUB_METHOD(listSymbols, SymbolPage, SymbolListRequest)
    STUB_METHOD(loadSymbolTable, SymbolTable, SymbolTableRequest)
    STUB_METHOD(setBreakpoint, BreakpointMutationReceipt,
                BreakpointSetRequest)
    STUB_METHOD(removeBreakpoint, BreakpointMutationReceipt,
                BreakpointAddressRequest)
    STUB_METHOD(suspendBreakpoint, BreakpointMutationReceipt,
                BreakpointAddressRequest)
    STUB_METHOD(resumeBreakpoint, BreakpointMutationReceipt,
                BreakpointAddressRequest)
    STUB_METHOD(breakpointHitBatch, BreakpointHitBatch,
                BreakpointHitBatchRequest)
    STUB_METHOD(scanResults, ScanResultPage, ScanResultsRequest)
    STUB_METHOD(clearScan, ScanClearResult, ScanClearRequest)
    STUB_METHOD(removeScanResults, ScanRemoveResult, ScanRemoveRequest)
    STUB_METHOD(writeMemory, WriteReceipt, MemoryWriteRequest)
    STUB_METHOD(writeValue, WriteReceipt, ValueWriteRequest)

#undef STUB_METHOD

    Mem::Result<Mem::ScanSummary> startScan(
        const Mem::OperationContext& context,
        const Mem::ScanStartRequest&,
        const Mem::ScanProgressSink& = {}) override {
        return unsupported<Mem::ScanSummary>("startScan", context);
    }

    Mem::Result<Mem::ScanSummary> refineScan(
        const Mem::OperationContext& context,
        const Mem::ScanRefineRequest&,
        const Mem::ScanProgressSink& = {}) override {
        return unsupported<Mem::ScanSummary>("refineScan", context);
    }

    int totalCalls() const {
        int total = 0;
        for (const auto& call : calls) {
            total += call.second;
        }
        return total;
    }

private:
    void record(const std::string& method,
                const Mem::OperationContext& context) {
        ++calls[method];
        lastContext = context;
    }

    template <typename T>
    Mem::Result<T> unsupported(const char* method,
                               const Mem::OperationContext& context) {
        record(method, context);
        return Mem::Result<T>::failure(
            Mem::ErrorCode::Unsupported,
            std::string(method) + " unsupported by test service", true, 5);
    }
};

NativeIpc::IpcRequestDto request(uint64_t id,
                                 std::string method,
                                 const json& params = json::object()) {
    NativeIpc::IpcRequestDto value;
    value.requestId = id;
    value.method = std::move(method);
    value.paramsJson = params.dump();
    value.timeout = 5s;
    return value;
}

NativeIpc::IpcRequestContext context() {
    NativeIpc::IpcRequestContext value;
    value.deadline = std::chrono::steady_clock::now() + 5s;
    value.cancellation =
        std::make_shared<NativeIpc::RequestCancellation>();
    return value;
}

void testAllObserveMethodsReachCanonicalAdapter() {
    StubMemService service;
    NativeIpc::IpcMemServiceDispatcher dispatcher(service);
    const std::vector<std::pair<std::string, json>> requests = {
        {"status", json::object()},
        {"process_list", json::object()},
        {"module_list", json::object()},
        {"module_resolve", {{"module_name", "libgame.so"}}},
        {"pointer_resolve",
         {{"module_name", "libgame.so"},
          {"base_offset", "0x0"},
          {"offsets", json::array()},
          {"deref_final", false}}},
        {"disassemble", {{"address", "0x1000"}, {"count", 1}}},
        {"symbol_resolve",
         {{"module_name", "libgame.so"}, {"symbol_name", "GameInit"}}},
        {"symbol_list", {{"module_name", "libgame.so"}}},
        {"breakpoint_hits", {{"address", "0x1000"}, {"count", 1}}},
        {"scan_results", {{"scan_epoch", 0}, {"count", 1}}},
        {"memory_read", {{"address", "0x1000"}, {"size", 4}}},
        {"memory_read_value",
         {{"address", "0x1000"}, {"data_type", "dword"}}},
    };
    const std::vector<std::string> serviceMethods = {
        "status",          "listProcesses",    "listModules",
        "resolveModule",   "resolvePointer",   "disassemble",
        "resolveSymbol",   "listSymbols",      "breakpointHitBatch",
        "scanResults",     "readMemory",       "readValue",
    };

    uint64_t id = 1;
    for (size_t i = 0; i < requests.size(); ++i) {
        NativeIpc::IpcCapability capability =
            NativeIpc::IpcCapability::HostExecution;
        expect(dispatcher.resolveCapability(requests[i].first, capability) &&
                   capability == NativeIpc::IpcCapability::Observe,
               "Observe method capability mismatch: " + requests[i].first);
        const auto result = dispatcher.execute(
            request(id++, requests[i].first, requests[i].second), context());
        expect(result.errorCode != "approval_required" &&
                   service.calls[serviceMethods[i]] == 1,
               "Observe method did not reach canonical adapter: " +
                   requests[i].first);
    }
    expect(service.totalCalls() == 12,
           "each Observe request should make one service call");
}

void testSuccessAndErrorEnvelopeMapping() {
    StubMemService service;
    NativeIpc::IpcMemServiceDispatcher dispatcher(service);
    const auto status = dispatcher.execute(request(1, "status"), context());
    expect(status.ok && json::parse(status.resultJson)["connected"] == true,
           "status should preserve canonical JSON in the IPC result");

    const auto module = dispatcher.execute(
        request(2, "module_list"), context());
    expect(!module.ok && module.errorCode == "unsupported" &&
               module.retryable &&
               module.completion == NativeIpc::RequestCompletion::Completed,
           "MemService errors should preserve code/retryable/completion");

    std::string payload;
    std::string error;
    expect(NativeIpc::BuildResponsePayload(module, payload, error) &&
               json::parse(payload)["error"]["retryable"] == true,
           "wire error envelope should retain retryable");
}

void testPrivilegedMethodsCannotBypassApproval() {
    StubMemService service;
    NativeIpc::IpcMemServiceDispatcher dispatcher(service);
    uint64_t id = 1;
    for (auto descriptor = NativeIpc::IpcMethodCatalogBegin();
         descriptor != NativeIpc::IpcMethodCatalogEnd(); ++descriptor) {
        if (descriptor->executableWithoutApproval) {
            continue;
        }
        const auto result = dispatcher.execute(
            request(id++, descriptor->name), context());
        expect(!result.ok && result.errorCode == "approval_required" &&
                   result.completion ==
                       NativeIpc::RequestCompletion::RejectedBeforeStart,
               std::string("privileged method bypassed approval: ") +
                   descriptor->name);
    }
    expect(service.totalCalls() == 0,
           "privileged rejection must happen before MemService");
}

void testConnectionAndTargetInvalidateBaseline() {
    {
        StubMemService service;
        NativeIpc::IpcMemServiceDispatcher dispatcher(service);
        ++service.current.connectionGeneration;
        service.current.target->connectionGeneration =
            service.current.connectionGeneration;
        const auto validation = dispatcher.validateSession();
        expect(!validation.valid && validation.code == "connection_changed",
               "connection generation must invalidate the IPC session");
        const auto result = dispatcher.execute(request(1, "status"), context());
        expect(result.errorCode == "connection_changed" &&
                   service.totalCalls() == 0,
               "invalid connection must reject before service dispatch");
    }
    {
        StubMemService service;
        NativeIpc::IpcMemServiceDispatcher dispatcher(service);
        ++service.current.target->processRevision;
        const auto validation = dispatcher.validateSession();
        expect(!validation.valid && validation.code == "target_changed",
               "target revision must invalidate the IPC session");
    }
}

void testCancellationAndDeadlineReachOperationContext() {
    StubMemService service;
    NativeIpc::IpcMemServiceDispatcher dispatcher(service);

    auto cancelledContext = context();
    cancelledContext.cancellation->request(
        NativeIpc::RequestCancelReason::Client);
    const auto cancelled = dispatcher.execute(
        request(1, "memory_read", {{"address", "0x1000"}, {"size", 4}}),
        cancelledContext);
    expect(cancelled.errorCode == "cancel_requested" &&
               cancelled.completion ==
                   NativeIpc::RequestCompletion::CancelRequested &&
               service.lastContext.cancellation &&
               service.lastContext.cancellation->load(
                   std::memory_order_acquire),
           "client cancellation must reach MemService context");

    auto expiredContext = context();
    expiredContext.deadline = std::chrono::steady_clock::now() - 1ms;
    const auto expired = dispatcher.execute(
        request(2, "memory_read", {{"address", "0x1000"}, {"size", 4}}),
        expiredContext);
    expect(expired.errorCode == "timeout" && expired.retryable &&
               service.lastContext.deadline == expiredContext.deadline,
           "absolute request deadline must reach MemService context");
}

} // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"all Observe methods use canonical adapter",
         &testAllObserveMethodsReachCanonicalAdapter},
        {"success and error envelope mapping",
         &testSuccessAndErrorEnvelopeMapping},
        {"privileged methods require approval",
         &testPrivilegedMethodsCannotBypassApproval},
        {"connection and target invalidate baseline",
         &testConnectionAndTargetInvalidateBaseline},
        {"cancellation and deadline propagation",
         &testCancellationAndDeadlineReachOperationContext},
    };

    int failures = 0;
    for (const auto& test : tests) {
        try {
            test.second();
            std::cout << "[PASS] " << test.first << '\n';
        } catch (const std::exception& exception) {
            ++failures;
            std::cerr << "[FAIL] " << test.first << ": "
                      << exception.what() << '\n';
        }
    }
    if (failures != 0) {
        std::cerr << failures << " test group(s) failed\n";
        return 1;
    }
    std::cout << tests.size() << " test groups passed\n";
    return 0;
}
