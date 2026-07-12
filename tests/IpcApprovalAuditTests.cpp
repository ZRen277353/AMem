#include "ipc/IpcApprovalAudit.h"

#include <nlohmann/json.hpp>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using json = nlohmann::json;
using namespace std::chrono_literals;

void expect(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

class TempDirectory final {
public:
  explicit TempDirectory(const char *suffix) {
    path_ = std::filesystem::temp_directory_path() /
            ("AMem.IpcApprovalAuditTests." +
             std::to_string(::GetCurrentProcessId()) + "." + suffix);
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
    std::filesystem::create_directories(path_);
  }

  ~TempDirectory() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

Mem::OperationContext context() {
  Mem::OperationContext value;
  value.connectionGeneration = 7;
  value.target = Mem::TargetSnapshot{42, 420, 2, 7};
  value.deadline = (std::chrono::steady_clock::time_point::max)();
  return value;
}

NativeIpc::IpcApprovalSubmission submission(uint64_t sessionId,
                                             uint64_t requestId) {
  NativeIpc::IpcApprovalSubmission value;
  value.sessionId = sessionId;
  value.requestId = requestId;
  value.clientName = "AMem.AuditTests";
  value.clientVersion = "1.0";
  value.method = "memory_write";
  value.expected = context();
  value.deadline = std::chrono::steady_clock::now() + 5s;
  return value;
}

NativeIpc::IpcApprovalRecord record(uint64_t id) {
  NativeIpc::IpcApprovalRecord value;
  value.approvalId = id;
  value.sessionId = 100 + id;
  value.requestId = 200 + id;
  value.clientName = "AMem.AuditTests";
  value.clientVersion = "1.0";
  value.method = "memory_write";
  value.capability = NativeIpc::IpcCapability::TargetMutation;
  value.targetPolicy = NativeIpc::IpcMethodTargetPolicy::Bound;
  value.connectionGeneration = 7;
  value.target = Mem::TargetSnapshot{42, 420, 2, 7};
  value.state = NativeIpc::IpcApprovalState::Pending;
  value.createdAt = std::chrono::steady_clock::now();
  value.deadline = value.createdAt + 5s;
  return value;
}

NativeIpc::IpcExecutionAuditRecord executionRecord(
    uint64_t id, bool success = true,
    NativeIpc::RequestCompletion completion =
        NativeIpc::RequestCompletion::Completed) {
  NativeIpc::IpcExecutionAuditRecord value;
  value.approvalId = id;
  value.sessionId = 100 + id;
  value.requestId = 200 + id;
  value.clientName = "AMem.AuditTests";
  value.clientVersion = "1.0";
  value.method = "memory_write";
  value.capability = NativeIpc::IpcCapability::TargetMutation;
  value.targetPolicy = NativeIpc::IpcMethodTargetPolicy::Bound;
  value.authorizedConnectionGeneration = 7;
  value.authorizedTarget = Mem::TargetSnapshot{42, 420, 2, 7};
  value.observedConnectionGeneration = 7;
  value.observedTarget = Mem::TargetSnapshot{42, 420, 2, 7};
  value.success = success;
  value.completion = completion;
  if (!success) {
    value.errorCode = "completion_unknown";
  }
  return value;
}

std::vector<json> readValidLines(const std::filesystem::path &path) {
  std::vector<json> values;
  std::ifstream input(path, std::ios::binary);
  std::string line;
  while (std::getline(input, line)) {
    const json parsed = json::parse(line, nullptr, false);
    if (!parsed.is_discarded()) {
      values.push_back(parsed);
    }
  }
  return values;
}

void testBrokerTransitionsPersistBoundedMetadata() {
  TempDirectory directory("transitions");
  const auto path = directory.path() / "approvals.jsonl";
  NativeIpc::IpcApprovalAuditLog audit(path.string(), 64u * 1024u, 10);
  NativeIpc::IpcApprovalBroker broker({}, &audit);

  const auto submitted = broker.submit(submission(9, 7));
  expect(submitted.ok &&
             broker
                 .decide(submitted.record->approvalId,
                         NativeIpc::IpcApprovalDecision::Deny, context())
                 .ok,
         "broker should emit pending and denied transitions");

  const auto snapshot = audit.snapshot();
  expect(snapshot.successfulWrites == 2 && snapshot.failedWrites == 0 &&
             snapshot.lastError.empty() && snapshot.recent.size() == 2 &&
             snapshot.recent[0].state == "pending" &&
             snapshot.recent[1].state == "denied",
         "audit snapshot should retain ordered broker transitions");
  const auto lines = readValidLines(path);
  expect(lines.size() == 2 && lines[0]["schema_version"] == 2 &&
             lines[0]["event_type"] == "approval_transition" &&
             lines[0]["session_id"] == 9 &&
             lines[0]["request_id"] == 7 &&
             lines[0]["method"] == "memory_write" &&
             lines[0]["capability"] == "TargetMutation" &&
             lines[0]["target"]["pid"] == 42 &&
             !lines[0].contains("params") &&
             !lines[0].contains("result") &&
             !lines[0].contains("result_json"),
         "persistent audit must contain bounded metadata without raw data");
}

void testExecutionOutcomesPersistWithoutRawData() {
  TempDirectory directory("execution");
  const auto path = directory.path() / "security.jsonl";
  NativeIpc::IpcApprovalAuditLog audit(path.string(), 64u * 1024u, 10);

  expect(audit.recordExecution(executionRecord(20)),
         "successful execution outcome should persist");
  expect(audit.recordExecution(
             executionRecord(
                 21, false,
                 NativeIpc::RequestCompletion::CompletionUnknown)),
         "uncertain execution outcome should persist");

  const auto snapshot = audit.snapshot();
  expect(snapshot.successfulWrites == 2 && snapshot.failedWrites == 0 &&
             snapshot.recent.size() == 2 &&
             snapshot.recent[0].eventType == "execution_outcome" &&
             snapshot.recent[0].success == std::optional<bool>{true} &&
             snapshot.recent[0].completion == "completed" &&
             snapshot.recent[1].success == std::optional<bool>{false} &&
             snapshot.recent[1].errorCode == "completion_unknown",
         "snapshot should distinguish execution summaries from approvals");

  const auto lines = readValidLines(path);
  expect(lines.size() == 2 && lines[0]["schema_version"] == 2 &&
             lines[0]["event_type"] == "execution_outcome" &&
             lines[0]["success"] == true &&
             lines[0]["completion"] == "completed" &&
             lines[0]["target"]["pid"] == 42 &&
             lines[0]["observed_target"]["pid"] == 42 &&
             !lines[0].contains("params") &&
             !lines[0].contains("result") &&
             !lines[0].contains("result_json") &&
             !lines[0].contains("error_message"),
         "execution audit must exclude request and result content");
}

void testRotationReloadAndMalformedLinesStayBounded() {
  TempDirectory directory("rotation");
  const auto path = directory.path() / "approvals.jsonl";
  {
    NativeIpc::IpcApprovalAuditLog audit(path.string(), 16u * 1024u, 5);
    for (uint64_t id = 1; id <= 100; ++id) {
      expect(audit.append(record(id)), "rotation fixture append failed");
    }
    const auto snapshot = audit.snapshot();
    expect(snapshot.recent.size() == 5 &&
               snapshot.recent.back().approvalId == 100 &&
               std::filesystem::exists(path.string() + ".1") &&
               std::filesystem::file_size(path) <= 16u * 1024u &&
               std::filesystem::file_size(path.string() + ".1") <=
                   16u * 1024u,
           "audit should rotate once and retain bounded recent entries");
  }

  {
    std::ofstream output(path, std::ios::binary | std::ios::app);
    output << "{malformed-json}\n" << std::string(20u * 1024u, 'x') << '\n'
           << R"({"schema_version":1,"timestamp_ms":1,"approval_id":777,"session_id":9,"request_id":7,"client_name":"legacy","method":"memory_write","state":"pending"})"
           << '\n';
  }
  NativeIpc::IpcApprovalAuditLog reloaded(path.string(), 16u * 1024u, 5);
  const auto snapshot = reloaded.snapshot();
  expect(!snapshot.recent.empty() && snapshot.recent.size() <= 5 &&
             snapshot.recent.back().approvalId == 777 &&
             snapshot.recent.back().eventType == "approval_transition",
         "reload should accept schema 1 and ignore malformed oversized lines");
  expect(reloaded.append(record(101)) &&
             std::filesystem::file_size(path) <= 16u * 1024u,
         "append should replace an externally oversized active log");
}

void testConcurrentAppendsAreSerialized() {
  TempDirectory directory("concurrent");
  const auto path = directory.path() / "approvals.jsonl";
  NativeIpc::IpcApprovalAuditLog audit(path.string(), 4u * 1024u * 1024u,
                                       32);
  std::vector<std::thread> threads;
  std::atomic<uint64_t> nextId{1};
  std::atomic<size_t> appendFailures{0};
  for (size_t threadIndex = 0; threadIndex < 8; ++threadIndex) {
    threads.emplace_back([&] {
      for (size_t item = 0; item < 25; ++item) {
        const uint64_t id = nextId.fetch_add(1, std::memory_order_relaxed);
        if (!audit.append(record(id))) {
          appendFailures.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }
  const auto snapshot = audit.snapshot();
  expect(appendFailures.load(std::memory_order_relaxed) == 0 &&
             snapshot.successfulWrites == 200 && snapshot.failedWrites == 0 &&
             snapshot.recent.size() == 32 &&
             readValidLines(path).size() == 200,
         "concurrent audit writes must serialize without loss");
}

void testWriteFailuresRemainVisible() {
  TempDirectory directory("failure");
  const auto path = directory.path() / "not-a-file";
  std::filesystem::create_directories(path);
  NativeIpc::IpcApprovalAuditLog audit(path.string());
  std::string error;
  expect(!audit.append(record(1), &error),
         "directory path should reject audit append");
  const auto snapshot = audit.snapshot();
  expect(snapshot.successfulWrites == 0 && snapshot.failedWrites == 1 &&
             !snapshot.lastError.empty() && snapshot.lastError == error,
         "audit write failure must remain visible to the control surface");
}

void testConsumedAuditFailureWithholdsAndBurnsGrant() {
  TempDirectory directory("consume-failure");
  const auto path = directory.path() / "not-a-file";
  std::filesystem::create_directories(path);
  NativeIpc::IpcApprovalAuditLog audit(path.string());
  NativeIpc::IpcApprovalBroker broker({}, &audit);

  const auto submitted = broker.submit(submission(40, 50));
  expect(submitted.ok &&
             broker
                 .decide(submitted.record->approvalId,
                         NativeIpc::IpcApprovalDecision::Approve, context())
                 .ok,
         "failed audit fixture should still reach approved state");
  const auto consumed =
      broker.consume(submitted.record->approvalId, 40, 50, context());
  expect(!consumed.ok && !consumed.grant &&
             consumed.code == "approval_audit_failed" && consumed.record &&
             consumed.record->state == NativeIpc::IpcApprovalState::Consumed,
         "real persistent failure must withhold a consumed grant");
  expect(broker.consume(submitted.record->approvalId, 40, 50, context()).code ==
                 "approval_not_approved" &&
             audit.snapshot().failedWrites == 3,
         "failed durable consume must remain burned without another append");
}

} // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests = {
      {"broker transitions persist bounded metadata",
       &testBrokerTransitionsPersistBoundedMetadata},
      {"execution outcomes persist without raw data",
       &testExecutionOutcomesPersistWithoutRawData},
      {"rotation reload and malformed lines stay bounded",
       &testRotationReloadAndMalformedLinesStayBounded},
      {"concurrent appends are serialized", &testConcurrentAppendsAreSerialized},
      {"write failures remain visible", &testWriteFailuresRemainVisible},
      {"consumed audit failure withholds and burns grant",
       &testConsumedAuditFailureWithholdsAndBurnsGrant},
  };

  int failures = 0;
  for (const auto &test : tests) {
    try {
      test.second();
      std::cout << "[PASS] " << test.first << '\n';
    } catch (const std::exception &error) {
      ++failures;
      std::cerr << "[FAIL] " << test.first << ": " << error.what() << '\n';
    }
  }
  if (failures != 0) {
    std::cerr << failures << " test group(s) failed\n";
    return 1;
  }
  std::cout << tests.size() << " test groups passed\n";
  return 0;
}
