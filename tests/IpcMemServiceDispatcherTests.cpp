#include "ipc/IpcMemServiceDispatcher.h"
#include "ipc/IpcApprovalBroker.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
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

template <typename Predicate>
bool waitUntil(Predicate predicate,
               std::chrono::milliseconds timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

class StubMemService final : public Mem::IMemService {
public:
    Mem::OperationContext current{
        7, Mem::TargetSnapshot{42, 420, 2, 7}, nullptr,
        (std::chrono::steady_clock::time_point::max)()};
    std::unordered_map<std::string, int> calls;
    Mem::OperationContext lastContext;

    Mem::OperationContext captureContext(bool includeTarget) const override {
      std::lock_guard<std::mutex> lock(currentMutex_);
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

    Mem::Result<Mem::DriverInitializationReceipt>
    initializeDriver(const Mem::OperationContext &context,
                     const Mem::DriverInitializeRequest &) override {
      record("initializeDriver", context);
      if (const auto error = validateContext(context, false)) {
        return Mem::Result<Mem::DriverInitializationReceipt>::failure(
            error->code, error->message, error->retryable, 1);
      }
      Mem::DriverInitializationReceipt value;
      value.message = "initialized";
      value.connectionGeneration = context.connectionGeneration;
      return Mem::Result<Mem::DriverInitializationReceipt>::success(
          std::move(value), 2);
    }

    Mem::Result<Mem::OpenProcessResult>
    openProcess(const Mem::OperationContext &context,
                const Mem::OpenProcessRequest &request) override {
      record("openProcess", context);
      if (const auto error = validateContext(context, true)) {
        return Mem::Result<Mem::OpenProcessResult>::failure(
            error->code, error->message, error->retryable, 1);
      }
      Mem::TargetSnapshot selected;
      {
        std::lock_guard<std::mutex> lock(currentMutex_);
        selected = {request.pid, request.pid * 10,
                    current.target->processRevision + 1,
                    current.connectionGeneration};
        current.target = selected;
      }
      openTargetChanged_.store(true, std::memory_order_release);
      while (blockOpenAfterTarget_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(1ms);
      }
      Mem::OpenProcessResult value;
      value.target = selected;
      if (returnMismatchedSelection_.load(std::memory_order_acquire)) {
        ++value.target.processRevision;
      }
      value.name = "com.example.selected";
      return Mem::Result<Mem::OpenProcessResult>::success(std::move(value), 3);
    }

#define STUB_METHOD(method, resultType, requestType)                         \
    Mem::Result<Mem::resultType> method(                                    \
        const Mem::OperationContext& context,                               \
        const Mem::requestType&) override {                                 \
        return unsupported<Mem::resultType>(#method, context);              \
    }

    STUB_METHOD(listModules, ModulePage, ModuleListRequest)
    STUB_METHOD(resolveModule, ResolvedModule, ModuleResolveRequest)
    STUB_METHOD(resolvePointer, PointerResolution, PointerResolveRequest)
    STUB_METHOD(disassemble, DisassemblyBlock, DisassemblyRequest)
    STUB_METHOD(resolveSymbol, ResolvedSymbol, SymbolResolveRequest)
    STUB_METHOD(listSymbols, SymbolPage, SymbolListRequest)
    STUB_METHOD(loadSymbolTable, SymbolTable, SymbolTableRequest)
    STUB_METHOD(breakpointHitBatch, BreakpointHitBatch,
                BreakpointHitBatchRequest)
    STUB_METHOD(scanResults, ScanResultPage, ScanResultsRequest)
    STUB_METHOD(removeScanResults, ScanRemoveResult, ScanRemoveRequest)

#undef STUB_METHOD

    Mem::Result<Mem::ScanSummary>
    startScan(const Mem::OperationContext &context,
              const Mem::ScanStartRequest &request,
              const Mem::ScanProgressSink & = {}) override {
      return scanSummary("startScan", context, request.kind, request.dataType,
                         request.mode);
    }

    Mem::Result<Mem::ScanSummary>
    refineScan(const Mem::OperationContext &context,
               const Mem::ScanRefineRequest &request,
               const Mem::ScanProgressSink & = {}) override {
      return scanSummary("refineScan", context, Mem::ScanStartKind::Value,
                         request.dataType.value_or(Mem::ScanDataType::Dword),
                         request.mode);
    }

    Mem::Result<Mem::ScanClearResult>
    clearScan(const Mem::OperationContext &context,
              const Mem::ScanClearRequest &request) override {
      record("clearScan", context);
      if (const auto error = validateContext(context, true)) {
        return Mem::Result<Mem::ScanClearResult>::failure(
            error->code, error->message, error->retryable, 1);
      }
      Mem::ScanClearResult value;
      value.clearedEpoch = request.expectedEpoch.value_or(1);
      value.currentEpoch = value.clearedEpoch + 1;
      value.target = *context.target;
      return Mem::Result<Mem::ScanClearResult>::success(std::move(value), 3);
    }

    Mem::Result<Mem::WriteReceipt>
    writeMemory(const Mem::OperationContext &context,
                const Mem::MemoryWriteRequest &request) override {
      record("writeMemory", context);
      return writeReceipt(context, request.address,
                          static_cast<uint32_t>(request.bytes.size()));
    }

    Mem::Result<Mem::WriteReceipt>
    writeValue(const Mem::OperationContext &context,
               const Mem::ValueWriteRequest &request) override {
      record("writeValue", context);
      return writeReceipt(context, request.address,
                          static_cast<uint32_t>(request.bytes.size()));
    }

    Mem::Result<Mem::BreakpointMutationReceipt>
    setBreakpoint(const Mem::OperationContext &context,
                  const Mem::BreakpointSetRequest &request) override {
      return breakpointReceipt("setBreakpoint", context, request.address,
                               Mem::BreakpointAction::Set);
    }

    Mem::Result<Mem::BreakpointMutationReceipt>
    removeBreakpoint(const Mem::OperationContext &context,
                     const Mem::BreakpointAddressRequest &request) override {
      return breakpointReceipt("removeBreakpoint", context, request.address,
                               Mem::BreakpointAction::Remove);
    }

    Mem::Result<Mem::BreakpointMutationReceipt>
    suspendBreakpoint(const Mem::OperationContext &context,
                      const Mem::BreakpointAddressRequest &request) override {
      return breakpointReceipt("suspendBreakpoint", context, request.address,
                               Mem::BreakpointAction::Suspend);
    }

    Mem::Result<Mem::BreakpointMutationReceipt>
    resumeBreakpoint(const Mem::OperationContext &context,
                     const Mem::BreakpointAddressRequest &request) override {
      return breakpointReceipt("resumeBreakpoint", context, request.address,
                               Mem::BreakpointAction::Resume);
    }

    void invalidateGeneration() {
      std::lock_guard<std::mutex> lock(currentMutex_);
      ++current.connectionGeneration;
      current.target->connectionGeneration = current.connectionGeneration;
    }

    void invalidateTarget() {
      std::lock_guard<std::mutex> lock(currentMutex_);
      ++current.target->processRevision;
    }

    void setBlockOpenAfterTarget(bool value) {
      blockOpenAfterTarget_.store(value, std::memory_order_release);
      if (value) {
        openTargetChanged_.store(false, std::memory_order_release);
      }
    }

    bool openTargetChanged() const {
      return openTargetChanged_.load(std::memory_order_acquire);
    }

    void setReturnMismatchedSelection(bool value) {
      returnMismatchedSelection_.store(value, std::memory_order_release);
    }

    void setWriteCompletedAfterDeadline(bool value) {
      writeCompletedAfterDeadline_ = value;
    }

    int totalCalls() const {
        int total = 0;
        for (const auto& call : calls) {
            total += call.second;
        }
        return total;
    }

private:
  std::optional<Mem::Error>
  validateContext(const Mem::OperationContext &context,
                  bool requireTarget) const {
    const Mem::OperationContext actual = captureContext(true);
    if (context.cancellation &&
        context.cancellation->load(std::memory_order_acquire)) {
      return Mem::Error{Mem::ErrorCode::CancelRequested,
                        "cancelled before test send", false};
    }
    if (std::chrono::steady_clock::now() >= context.deadline) {
      return Mem::Error{Mem::ErrorCode::Timeout,
                        "deadline expired before test send", true};
    }
    if (actual.connectionGeneration != context.connectionGeneration) {
      return Mem::Error{Mem::ErrorCode::ConnectionChanged,
                        "generation changed before test send", true};
    }
    if (requireTarget && actual.target != context.target) {
      return Mem::Error{Mem::ErrorCode::TargetChanged,
                        "target changed before test send", false};
    }
    return std::nullopt;
  }

  Mem::Result<Mem::WriteReceipt>
  writeReceipt(const Mem::OperationContext &context, uint64_t address,
               uint32_t size) {
    if (const auto error = validateContext(context, true)) {
      return Mem::Result<Mem::WriteReceipt>::failure(
          error->code, error->message, error->retryable, 1);
    }
    Mem::WriteReceipt value;
    value.address = address;
    value.requestedBytes = size;
    value.writtenBytes = size;
    value.completedAfterDeadline = writeCompletedAfterDeadline_;
    value.target = *context.target;
    return Mem::Result<Mem::WriteReceipt>::success(std::move(value), 3);
  }

  Mem::Result<Mem::BreakpointMutationReceipt>
  breakpointReceipt(const char *method, const Mem::OperationContext &context,
                    uint64_t address, Mem::BreakpointAction action) {
    record(method, context);
    if (const auto error = validateContext(context, true)) {
      return Mem::Result<Mem::BreakpointMutationReceipt>::failure(
          error->code, error->message, error->retryable, 1);
    }
    Mem::BreakpointMutationReceipt value;
    value.address = address;
    value.action = action;
    value.target = *context.target;
    return Mem::Result<Mem::BreakpointMutationReceipt>::success(
        std::move(value), 3);
  }

  Mem::Result<Mem::ScanSummary>
  scanSummary(const char *method, const Mem::OperationContext &context,
              Mem::ScanStartKind kind, Mem::ScanDataType dataType,
              Mem::ScanMode mode) {
    record(method, context);
    if (const auto error = validateContext(context, true)) {
      return Mem::Result<Mem::ScanSummary>::failure(error->code, error->message,
                                                    error->retryable, 1);
    }
    Mem::ScanSummary value;
    value.session.epoch = 1;
    value.session.kind = kind;
    value.session.dataType = dataType;
    value.session.mode = mode;
    value.session.target = *context.target;
    return Mem::Result<Mem::ScanSummary>::success(std::move(value), 3);
  }

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

    mutable std::mutex currentMutex_;
    std::atomic<bool> blockOpenAfterTarget_{false};
    std::atomic<bool> openTargetChanged_{false};
    std::atomic<bool> returnMismatchedSelection_{false};
    bool writeCompletedAfterDeadline_ = false;
};

class FakeHostExecutor final : public NativeIpc::IIpcHostMethodExecutor {
public:
  bool supports(const std::string &method) const override {
    return method == "lua_execute";
  }

  std::string execute(const std::string &method, const std::string &paramsJson,
                      const Mem::OperationContext &context) override {
    ++calls;
    lastMethod = method;
    lastParamsJson = paramsJson;
    lastContext = context;
    return json({{"success", true},
                 {"output", "host-ok"},
                 {"completion", "completed"}})
        .dump();
  }

  int calls = 0;
  std::string lastMethod;
  std::string lastParamsJson;
  Mem::OperationContext lastContext;
};

class FailConsumedAuditSink final : public NativeIpc::IIpcApprovalAuditSink {
public:
  bool recordApproval(const NativeIpc::IpcApprovalRecord &record,
                      std::string *error) override {
    if (error != nullptr) {
      error->clear();
    }
    if (record.state != NativeIpc::IpcApprovalState::Consumed) {
      return true;
    }
    if (error != nullptr) {
      *error = "injected dispatcher audit failure";
    }
    return false;
  }
};

class BlockingConsumedAuditSink final
    : public NativeIpc::IIpcApprovalAuditSink {
public:
  bool recordApproval(const NativeIpc::IpcApprovalRecord &record,
                      std::string *error) override {
    if (error != nullptr) {
      error->clear();
    }
    if (record.state != NativeIpc::IpcApprovalState::Consumed) {
      return true;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    entered_ = true;
    condition_.notify_all();
    condition_.wait(lock, [&] { return released_; });
    return true;
  }

  bool waitUntilEntered() {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, 2s, [&] { return entered_; });
  }

  void release() {
    std::lock_guard<std::mutex> lock(mutex_);
    released_ = true;
    condition_.notify_all();
  }

private:
  std::mutex mutex_;
  std::condition_variable condition_;
  bool entered_ = false;
  bool released_ = false;
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
    FakeHostExecutor host;
    NativeIpc::IpcMemServiceDispatcher dispatcher(service, nullptr, {}, &host);
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
    expect(host.calls == 0,
           "privileged rejection must happen before host execution");
}

void testApprovedMemServiceMethodsExecuteOnce() {
  StubMemService service;
  NativeIpc::IpcApprovalBroker broker;
  NativeIpc::IpcMemServiceDispatcher dispatcher(
      service, &broker, {90, "AMem.DispatcherTests", "1.0"});
  const std::vector<std::tuple<std::string, json, std::string>> methods = {
      {"driver_initialize", {{"card", "driver-card"}}, "initializeDriver"},
      {"memory_write",
       {{"address", "0x1000"}, {"data_hex", "90 90"}},
       "writeMemory"},
      {"memory_write_value",
       {{"address", "0x1000"}, {"data_type", "dword"}, {"value", "7"}},
       "writeValue"},
      {"scan_start", {{"mode", "exact"}, {"pattern_hex", "00"}}, "startScan"},
      {"scan_refine",
       {{"scan_epoch", 1},
        {"mode", "exact"},
        {"data_type", "dword"},
        {"value", "1"}},
       "refineScan"},
      {"scan_clear", {{"scan_epoch", 1}}, "clearScan"},
      {"breakpoint_set", {{"address", "0x1000"}}, "setBreakpoint"},
      {"breakpoint_remove", {{"address", "0x1000"}}, "removeBreakpoint"},
      {"breakpoint_suspend", {{"address", "0x1000"}}, "suspendBreakpoint"},
      {"breakpoint_resume", {{"address", "0x1000"}}, "resumeBreakpoint"},
  };

  uint64_t requestId = 1;
  for (const auto &item : methods) {
    const std::string method = std::get<0>(item);
    const json params = std::get<1>(item);
    const std::string serviceMethod = std::get<2>(item);
    auto requestContext = context();
    auto future = std::async(std::launch::async, [&, id = requestId, method,
                                                  params] {
      return dispatcher.execute(request(id, method, params), requestContext);
    });
    expect(waitUntil([&] { return broker.snapshot().size() == requestId; }),
           "privileged method should reach approval: " + method);
    const auto pending = broker.snapshot().back();
    expect(broker
               .decide(pending.approvalId,
                       NativeIpc::IpcApprovalDecision::Approve,
                       service.captureContext(true))
               .ok,
           "privileged method should approve: " + method);
    const auto result = future.get();
    expect(result.ok &&
               broker.find(pending.approvalId)->state ==
                   NativeIpc::IpcApprovalState::Consumed &&
               service.calls[serviceMethod] == 1,
           "approved method should execute exactly once: " + method);
    ++requestId;
  }
  expect(service.totalCalls() == static_cast<int>(methods.size()),
         "each approved MemService method should execute once");
}

void testPrivilegedDecisionsAndCancellation() {
  StubMemService service;
  NativeIpc::IpcApprovalBroker broker;
  NativeIpc::IpcMemServiceDispatcher dispatcher(
      service, &broker, {91, "AMem.DispatcherTests", "1.0"});
  expect(dispatcher.canSubmitForApproval("memory_write") &&
             !dispatcher.canSubmitForApproval("status"),
         "only privileged catalog methods should enter approval");

  auto deniedContext = context();
  auto deniedFuture =
      std::async(std::launch::async, [&] {
        return dispatcher.execute(
            request(10, "memory_write",
                    {{"address", "0x1000"}, {"data", "SECRET_BYTES"}}),
            deniedContext);
      });
  expect(waitUntil([&] { return broker.snapshot().size() == 1; }),
         "privileged worker should submit a bounded approval record");
  const auto pending = broker.snapshot().front();
  expect(pending.sessionId == 91 && pending.requestId == 10 &&
             pending.method == "memory_write" && service.totalCalls() == 0,
         "submission must bind server identity without executing service");
  expect(broker
             .decide(pending.approvalId, NativeIpc::IpcApprovalDecision::Deny,
                     service.captureContext(true))
             .ok,
         "test decision should deny pending approval");
  const auto denied = deniedFuture.get();
  expect(denied.errorCode == "approval_denied" &&
             denied.completion ==
                 NativeIpc::RequestCompletion::RejectedBeforeStart &&
             service.totalCalls() == 0,
         "denial must return without parsing or executing privileged params");

  auto approvedContext = context();
  auto approvedFuture = std::async(std::launch::async, [&] {
    return dispatcher.execute(
        request(11, "memory_write",
                {{"address", "0x1000"}, {"data_hex", "90 90"}}),
        approvedContext);
  });
  expect(waitUntil([&] { return broker.snapshot().size() == 2; }),
         "second privileged request should submit independently");
  const auto approvedRecord = broker.snapshot().back();
  expect(broker
             .decide(approvedRecord.approvalId,
                     NativeIpc::IpcApprovalDecision::Approve,
                     service.captureContext(true))
             .ok,
         "test decision should approve matching context");
  const auto approved = approvedFuture.get();
  expect(approved.ok &&
             broker.find(approvedRecord.approvalId)->state ==
                 NativeIpc::IpcApprovalState::Consumed &&
             service.calls["writeMemory"] == 1,
         "durable approval should execute exactly once");

  auto cancelledContext = context();
  auto cancelledFuture = std::async(std::launch::async, [&] {
    return dispatcher.execute(request(12, "memory_write"), cancelledContext);
  });
  expect(waitUntil([&] { return broker.snapshot().size() == 3; }),
         "cancellable privileged request should reach broker");
  cancelledContext.cancellation->request(
      NativeIpc::RequestCancelReason::Client);
  const auto cancelled = cancelledFuture.get();
  expect(cancelled.errorCode == "cancelled" &&
             cancelled.completion ==
                 NativeIpc::RequestCompletion::CancelledBeforeStart &&
             broker.snapshot().back().state ==
                 NativeIpc::IpcApprovalState::Cancelled &&
             service.calls["writeMemory"] == 1,
         "request cancellation must revoke approval without execution");
}

void testConsumedAuditFailurePreventsExecution() {
  StubMemService service;
  FailConsumedAuditSink sink;
  NativeIpc::IpcApprovalBroker broker({}, &sink);
  NativeIpc::IpcMemServiceDispatcher dispatcher(
      service, &broker, {92, "AMem.DispatcherTests", "1.0"});
  auto requestContext = context();
  auto future = std::async(std::launch::async, [&] {
    return dispatcher.execute(
        request(20, "memory_write",
                {{"address", "0x1000"}, {"data_hex", "90"}}),
        requestContext);
  });
  expect(waitUntil([&] { return broker.snapshot().size() == 1; }),
         "audit failure request should reach approval");
  const auto pending = broker.snapshot().front();
  expect(broker
             .decide(pending.approvalId,
                     NativeIpc::IpcApprovalDecision::Approve,
                     service.captureContext(true))
             .ok,
         "audit failure fixture should approve");
  const auto result = future.get();
  expect(!result.ok && result.errorCode == "approval_audit_failed" &&
             broker.find(pending.approvalId)->state ==
                 NativeIpc::IpcApprovalState::Consumed &&
             service.calls["writeMemory"] == 0,
         "failed consumed audit must burn grant before adapter execution");
}

void testCancellationAfterConsumeStopsBeforeSend() {
  StubMemService service;
  BlockingConsumedAuditSink sink;
  NativeIpc::IpcApprovalBroker broker({}, &sink);
  NativeIpc::IpcMemServiceDispatcher dispatcher(
      service, &broker, {93, "AMem.DispatcherTests", "1.0"});
  auto requestContext = context();
  auto future = std::async(std::launch::async, [&] {
    return dispatcher.execute(
        request(21, "memory_write",
                {{"address", "0x1000"}, {"data_hex", "90"}}),
        requestContext);
  });
  expect(waitUntil([&] { return broker.snapshot().size() == 1; }),
         "post-consume cancellation should reach approval");
  const auto pending = broker.snapshot().front();
  expect(broker
             .decide(pending.approvalId,
                     NativeIpc::IpcApprovalDecision::Approve,
                     service.captureContext(true))
             .ok,
         "post-consume cancellation fixture should approve");
  const bool auditEntered = sink.waitUntilEntered();
  requestContext.cancellation->request(NativeIpc::RequestCancelReason::Client);
  sink.release();
  const auto result = future.get();
  expect(auditEntered && !result.ok && result.errorCode == "cancelled" &&
             result.completion ==
                 NativeIpc::RequestCompletion::CancelledBeforeSend &&
             service.calls["writeMemory"] == 0,
         "cancellation after consume must stop before the service boundary");
}

void testApprovedHostMethodUsesInjectedExecutor() {
  StubMemService service;
  FakeHostExecutor host;
  NativeIpc::IpcApprovalBroker broker;
  NativeIpc::IpcMemServiceDispatcher dispatcher(
      service, &broker, {94, "AMem.DispatcherTests", "1.0"}, &host);
  auto requestContext = context();
  auto future = std::async(std::launch::async, [&] {
    return dispatcher.execute(
        request(30, "lua_execute", {{"code", "print('secret')"}}),
        requestContext);
  });
  expect(waitUntil([&] { return broker.snapshot().size() == 1; }),
         "host method should reach approval");
  const auto pending = broker.snapshot().front();
  expect(broker
             .decide(pending.approvalId,
                     NativeIpc::IpcApprovalDecision::Approve,
                     service.captureContext(true))
             .ok,
         "host method should approve");
  const auto result = future.get();
  expect(result.ok && host.calls == 1 && host.lastMethod == "lua_execute" &&
             json::parse(host.lastParamsJson)["code"] == "print('secret')" &&
             host.lastContext.target == service.captureContext(true).target &&
             broker.find(pending.approvalId)->state ==
                 NativeIpc::IpcApprovalState::Consumed,
         "approved host method should execute once with the grant context");
}

void testProcessSelectionAdvancesBaselineSafely() {
  StubMemService service;
  NativeIpc::IpcApprovalBroker broker;
  NativeIpc::IpcMemServiceDispatcher dispatcher(
      service, &broker, {95, "AMem.DispatcherTests", "1.0"});
  service.setBlockOpenAfterTarget(true);
  auto requestContext = context();
  auto future = std::async(std::launch::async, [&] {
    return dispatcher.execute(request(40, "process_open", {{"pid", 84}}),
                              requestContext);
  });
  expect(waitUntil([&] { return broker.snapshot().size() == 1; }),
         "process selection should reach approval");
  const auto pending = broker.snapshot().front();
  expect(broker
             .decide(pending.approvalId,
                     NativeIpc::IpcApprovalDecision::Approve,
                     service.captureContext(true))
             .ok,
         "process selection should approve");
  const bool targetChanged =
      waitUntil([&] { return service.openTargetChanged(); });
  const auto transitionValidation = dispatcher.validateSession();
  service.setBlockOpenAfterTarget(false);
  const auto selected = future.get();
  const auto baseline = dispatcher.baselineContext();
  expect(
      targetChanged && transitionValidation.valid && selected.ok &&
          baseline.target && baseline.target->pid == 84 &&
          dispatcher.validateSession().valid,
      "selection transition should tolerate only its controlled target change");

  const auto read = dispatcher.execute(
      request(41, "memory_read", {{"address", "0x1000"}, {"size", 4}}),
      context());
  expect(read.ok && service.calls["readMemory"] == 1,
         "Observe requests should continue on the advanced baseline");
}

void testSelectionRejectsMismatchedReceipt() {
  StubMemService service;
  service.setReturnMismatchedSelection(true);
  NativeIpc::IpcApprovalBroker broker;
  NativeIpc::IpcMemServiceDispatcher dispatcher(
      service, &broker, {96, "AMem.DispatcherTests", "1.0"});
  auto requestContext = context();
  auto future = std::async(std::launch::async, [&] {
    return dispatcher.execute(request(42, "process_open", {{"pid", 84}}),
                              requestContext);
  });
  expect(waitUntil([&] { return broker.snapshot().size() == 1; }),
         "mismatched selection should reach approval");
  const auto pending = broker.snapshot().front();
  expect(broker
             .decide(pending.approvalId,
                     NativeIpc::IpcApprovalDecision::Approve,
                     service.captureContext(true))
             .ok,
         "mismatched selection fixture should approve");
  const auto result = future.get();
  expect(!result.ok && result.errorCode == "selection_result_invalid" &&
             result.completion ==
                 NativeIpc::RequestCompletion::CompletionUnknown &&
             !dispatcher.validateSession().valid,
         "selection must reject a receipt that is not the current target");
}

void testSelectionReportsCompletionAfterCancellation() {
  StubMemService service;
  NativeIpc::IpcApprovalBroker broker;
  NativeIpc::IpcMemServiceDispatcher dispatcher(
      service, &broker, {97, "AMem.DispatcherTests", "1.0"});
  service.setBlockOpenAfterTarget(true);
  auto requestContext = context();
  auto future = std::async(std::launch::async, [&] {
    return dispatcher.execute(request(43, "process_open", {{"pid", 84}}),
                              requestContext);
  });
  expect(waitUntil([&] { return broker.snapshot().size() == 1; }),
         "cancelled selection should reach approval");
  const auto pending = broker.snapshot().front();
  expect(broker.decide(pending.approvalId,
                       NativeIpc::IpcApprovalDecision::Approve,
                       service.captureContext(true))
                 .ok &&
             waitUntil([&] { return service.openTargetChanged(); }),
         "cancelled selection should start after approval");
  requestContext.cancellation->request(NativeIpc::RequestCancelReason::Client);
  service.setBlockOpenAfterTarget(false);
  const auto result = future.get();
  expect(
      result.ok &&
          result.completion ==
              NativeIpc::RequestCompletion::CompletedAfterCancelRequest &&
          json::parse(result.resultJson)["completion"] ==
              "completed_after_cancel_request" &&
          dispatcher.baselineContext().target->pid == 84,
      "selection completed after Cancel must retain receipt and new baseline");
}

void testCompletedAfterDeadlineRemainsSuccessful() {
  StubMemService service;
  service.setWriteCompletedAfterDeadline(true);
  NativeIpc::IpcApprovalBroker broker;
  NativeIpc::IpcMemServiceDispatcher dispatcher(
      service, &broker, {98, "AMem.DispatcherTests", "1.0"});
  auto requestContext = context();
  auto future = std::async(std::launch::async, [&] {
    return dispatcher.execute(
        request(44, "memory_write",
                {{"address", "0x1000"}, {"data_hex", "90"}}),
        requestContext);
  });
  expect(waitUntil([&] { return broker.snapshot().size() == 1; }),
         "late completion should reach approval");
  const auto pending = broker.snapshot().front();
  expect(broker
             .decide(pending.approvalId,
                     NativeIpc::IpcApprovalDecision::Approve,
                     service.captureContext(true))
             .ok,
         "late completion should approve");
  const auto result = future.get();
  std::string payload;
  std::string error;
  expect(result.ok &&
             result.completion ==
                 NativeIpc::RequestCompletion::CompletedAfterDeadline &&
             NativeIpc::BuildResponsePayload(result, payload, error) &&
             json::parse(payload)["completion"] == "completed_after_deadline",
         "confirmed mutation after deadline must remain a successful receipt");
}

void testConnectionAndTargetInvalidateBaseline() {
    {
        StubMemService service;
        NativeIpc::IpcMemServiceDispatcher dispatcher(service);
        service.invalidateGeneration();
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
        service.invalidateTarget();
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
      {"approved MemService methods execute once",
       &testApprovedMemServiceMethodsExecuteOnce},
      {"privileged decisions and cancellation",
       &testPrivilegedDecisionsAndCancellation},
      {"consumed audit failure prevents execution",
       &testConsumedAuditFailurePreventsExecution},
      {"cancellation after consume stops before send",
       &testCancellationAfterConsumeStopsBeforeSend},
      {"approved host method uses injected executor",
       &testApprovedHostMethodUsesInjectedExecutor},
      {"process selection advances baseline safely",
       &testProcessSelectionAdvancesBaselineSafely},
      {"selection rejects mismatched receipt",
       &testSelectionRejectsMismatchedReceipt},
      {"selection reports completion after cancellation",
       &testSelectionReportsCompletionAfterCancellation},
      {"completed after deadline remains successful",
       &testCompletedAfterDeadlineRemainsSuccessful},
      {"connection and target invalidate baseline",
       &testConnectionAndTargetInvalidateBaseline},
      {"cancellation and deadline propagation",
       &testCancellationAndDeadlineReachOperationContext},
  };

  int failures = 0;
  for (const auto &test : tests) {
    try {
      test.second();
      std::cout << "[PASS] " << test.first << '\n';
    } catch (const std::exception &exception) {
      ++failures;
      std::cerr << "[FAIL] " << test.first << ": " << exception.what() << '\n';
    }
    }
    if (failures != 0) {
        std::cerr << failures << " test group(s) failed\n";
        return 1;
    }
    std::cout << tests.size() << " test groups passed\n";
    return 0;
}
