#include "../gui/ai/AgentMemTools.h"
#include "../gui/ai/ToolExecutor.h"
#include "../mem/Address.h"
#include "../mem/MemService.h"
#include "../socket/DeviceSession.h"
#include "../third_party/nlohmann/json.hpp"

#include <functional>
#include <atomic>
#include <chrono>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using nlohmann::json;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class FakeBackend final : public Mem::IMemBackend {
public:
    bool connected = true;
    bool poisoned = false;
    uint64_t generation = 1;
    Mem::TargetSnapshot target{42, 420, 2, 1};
    std::string selectedName = "com.example.game";
    std::vector<Mem::ProcessInfo> processes = {
        {10, "android.system"},
        {42, "com.example.game"},
        {84, "com.example.game.helper"},
    };
    std::vector<unsigned char> memory = {0xDE, 0xAD, 0xBE, 0xEF};
    bool fetchProcessesSucceeds = true;
    bool openProcessSucceeds = true;
    bool readMemorySucceeds = true;
    bool changeTargetAfterRead = false;
    bool changeGenerationAfterRead = false;
    bool changeGenerationDuringOpen = false;

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

    bool fetchProcesses(std::vector<Mem::ProcessInfo>& output) override {
        if (!fetchProcessesSucceeds) {
            return false;
        }
        output = processes;
        return true;
    }

    bool openProcess(int pid, const std::string& name) override {
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

    bool readMemory(uint64_t,
                    uint32_t,
                    std::vector<unsigned char>& output) override {
        if (!readMemorySucceeds) {
            return false;
        }
        output = memory;
        if (changeTargetAfterRead) {
            target.processRevision += 2;
        }
        if (changeGenerationAfterRead) {
            ++generation;
        }
        return true;
    }
};

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

    const auto legacy = Mem::parseAddress("7FF0", false);
    expect(legacy.ok() && legacy.value() == 0x7ff0,
           "legacy address mode should remain available to hidden aliases");

    const auto overflow = Mem::parseAddress("0x10000000000000000");
    expect(!overflow.ok(), "uint64 address overflow must fail");
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

void testHiddenToolRegistration() {
    auto& registry = AI::ToolExecutor::getInstance();
    registry.registerTool("test_visible_tool", "visible", "{}",
                          AI::ToolSafety::ReadOnly,
                          [](const std::string&) { return std::string("{}"); });
    registry.registerTool("test_hidden_alias", "hidden", "{}",
                          AI::ToolSafety::ReadOnly,
                          [](const std::string&) { return std::string("{}"); },
                          false);

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

void testAgentAdapter() {
    FakeBackend backend;
    Mem::MemService service(backend);
    AI::AgentMemTools tools(service);

    const json status = json::parse(tools.status("{}"));
    expect(status.at("success").get<bool>() &&
               status.at("connection_generation").get<uint64_t>() == 1,
           "status adapter should expose structured success and generation");

    const json processes = json::parse(
        tools.processList(R"({"filter":"game","count":1})"));
    expect(processes.at("success").get<bool>() &&
               processes.at("truncated").get<bool>(),
           "process adapter should expose pagination metadata");

    const json strictAddress = json::parse(
        tools.memoryRead(R"({"address":"1000","size":4})", false));
    expect(!strictAddress.at("success").get<bool>() &&
               strictAddress.at("error").at("code") == "invalid_argument",
           "canonical memory_read should reject missing 0x prefix");

    const json read = json::parse(
        tools.memoryRead(R"({"address":"0x1000","size":4})", false));
    expect(read.at("success").get<bool>() &&
               read.at("hex") == "DEADBEEF" &&
               read.at("data") == "DE AD BE EF",
           "memory adapter should expose compact and spaced hex");

    const json legacyRead = json::parse(
        tools.memoryRead(R"({"address":4096,"size":4})", true));
    expect(legacyRead.at("success").get<bool>(),
           "hidden legacy memory alias should accept integer addresses");

    backend.connected = false;
    const json disconnected = json::parse(tools.processList("{}"));
    expect(!disconnected.at("success").get<bool>() &&
               disconnected.at("error").at("code") == "not_connected",
           "adapter should preserve structured service errors");
}

} // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"address contract", &testAddressContract},
        {"status and generation", &testStatusAndConnectionGeneration},
        {"process pagination and open", &testProcessPaginationAndOpen},
        {"memory target validation", &testMemoryReadTargetValidation},
        {"agent adapter", &testAgentAdapter},
        {"hidden tool registration", &testHiddenToolRegistration},
        {"device session lifecycle", &testDeviceSessionLifecycle},
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
