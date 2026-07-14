#include "ipc/IpcApprovalBroker.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

void expect(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

Mem::OperationContext context(uint64_t generation = 7, uint64_t revision = 2) {
  Mem::OperationContext value;
  value.connectionGeneration = generation;
  value.target = Mem::TargetSnapshot{42, 420, revision, generation};
  value.deadline = (std::chrono::steady_clock::time_point::max)();
  return value;
}

NativeIpc::IpcApprovalSubmission
submission(std::string method, uint64_t sessionId = 1, uint64_t requestId = 1,
           const Mem::OperationContext &expected = context(),
           std::chrono::steady_clock::time_point deadline =
               std::chrono::steady_clock::now() + 5s) {
  NativeIpc::IpcApprovalSubmission value;
  value.sessionId = sessionId;
  value.requestId = requestId;
  value.clientName = "AMem.ApprovalTests";
  value.clientVersion = "1.0";
  value.method = std::move(method);
  value.expected = expected;
  value.deadline = deadline;
  return value;
}

class AuditSink final : public NativeIpc::IIpcApprovalAuditSink {
public:
  bool recordApproval(const NativeIpc::IpcApprovalRecord &record,
                      std::string *error) override {
    if (error != nullptr) {
      error->clear();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    records_.push_back(record);
    if (broker_ != nullptr) {
      observedSnapshotSizes_.push_back(broker_->snapshot().size());
    }
    return true;
  }

  std::vector<NativeIpc::IpcApprovalRecord> records() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return records_;
  }

  void setBroker(NativeIpc::IpcApprovalBroker *broker) { broker_ = broker; }

  size_t reentrantSnapshots() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return observedSnapshotSizes_.size();
  }

private:
  mutable std::mutex mutex_;
  std::vector<NativeIpc::IpcApprovalRecord> records_;
  std::vector<size_t> observedSnapshotSizes_;
  NativeIpc::IpcApprovalBroker *broker_ = nullptr;
};

class FailingConsumedAuditSink final : public NativeIpc::IIpcApprovalAuditSink {
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
      *error = "injected durable write failure";
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
    consumedEntered_ = true;
    condition_.notify_all();
    condition_.wait(lock, [&] { return released_; });
    return true;
  }

  bool waitForConsumedAudit() {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, 5s, [&] { return consumedEntered_; });
  }

  void release() {
    std::lock_guard<std::mutex> lock(mutex_);
    released_ = true;
    condition_.notify_all();
  }

private:
  std::mutex mutex_;
  std::condition_variable condition_;
  bool consumedEntered_ = false;
  bool released_ = false;
};

void testCatalogOwnedValidationAndBoundedDto() {
  NativeIpc::IpcApprovalBroker broker;
  expect(broker.submit(submission("status")).code == "approval_not_required",
         "Observe method must not enter approval");
  expect(broker.submit(submission("not_registered")).code == "method_not_found",
         "unknown method must be rejected");
  expect(broker
             .submit(submission("memory_write", 1, 5),
                     NativeIpc::IpcApprovalSubmissionMode::AutoApproved)
             .code == "auto_approval_not_allowed",
         "policy auto-approval must be restricted to lua_execute");

  auto invalidIdentity = submission("memory_write");
  invalidIdentity.sessionId = 0;
  expect(broker.submit(invalidIdentity).code == "invalid_identity",
         "zero session id must be rejected");

  auto invalidClient = submission("memory_write");
  invalidClient.clientName.assign(129, 'x');
  expect(broker.submit(invalidClient).code == "invalid_client",
         "oversized client identity must be rejected");
  invalidClient.clientName = "invalid\nclient";
  expect(broker.submit(invalidClient).code == "invalid_client",
         "client controls must be rejected outside the handshake too");

  auto expired = submission("memory_write");
  expired.deadline = std::chrono::steady_clock::now() - 1ms;
  expect(broker.submit(expired).code == "invalid_deadline",
         "past approval deadline must be rejected");

  Mem::OperationContext missingTarget = context();
  missingTarget.target.reset();
  expect(broker.submit(submission("memory_write", 1, 2, missingTarget)).code ==
             "invalid_context",
         "bound method must require a target");

  const auto driver = broker.submit(submission("driver_initialize", 1, 3));
  expect(driver.ok && driver.record &&
             driver.record->capability ==
                 NativeIpc::IpcCapability::HostExecution &&
             driver.record->targetPolicy ==
                 NativeIpc::IpcMethodTargetPolicy::None,
         "broker must derive driver metadata from catalog");
  const auto selection = broker.submit(submission("process_open", 1, 4));
  expect(selection.ok && selection.record &&
             selection.record->capability ==
                 NativeIpc::IpcCapability::TargetSelection &&
             selection.record->targetPolicy ==
                 NativeIpc::IpcMethodTargetPolicy::Selection,
         "broker must derive selection metadata from catalog");
}

void testApproveConsumeIsOneShot() {
  NativeIpc::IpcApprovalBroker broker;
  const Mem::OperationContext expected = context();
  const auto submitted =
      broker.submit(submission("memory_write", 10, 20, expected));
  expect(submitted.ok && submitted.record,
         "privileged request should enter pending state");
  const uint64_t approvalId = submitted.record->approvalId;

  const auto approved = broker.decide(
      approvalId, NativeIpc::IpcApprovalDecision::Approve, expected);
  expect(approved.ok &&
             approved.record->state == NativeIpc::IpcApprovalState::Approved,
         "matching context should approve pending request");
  expect(
      broker.decide(approvalId, NativeIpc::IpcApprovalDecision::Deny, expected)
              .code == "approval_not_pending",
      "decision must be a one-way transition");

  const auto consumed = broker.consume(approvalId, 10, 20, expected);
  expect(consumed.ok && consumed.grant &&
             consumed.record->state == NativeIpc::IpcApprovalState::Consumed &&
             consumed.grant->sessionId == 10 &&
             consumed.grant->requestId == 20 &&
             consumed.grant->method == "memory_write" &&
             consumed.grant->target == expected.target,
         "approved request should yield one target-bound grant");
  expect(broker.consume(approvalId, 10, 20, expected).code ==
             "approval_not_approved",
         "approval grant must not be reusable");

  const auto autoApproved = broker.submit(
      submission("lua_execute", 10, 22, expected),
      NativeIpc::IpcApprovalSubmissionMode::AutoApproved);
  expect(autoApproved.ok && autoApproved.record &&
             autoApproved.record->state ==
                 NativeIpc::IpcApprovalState::Approved &&
             autoApproved.record->autoApproved &&
             autoApproved.record->targetPolicy ==
                 NativeIpc::IpcMethodTargetPolicy::Selection,
         "Lua policy submission must start approved without a pending decision");
  const auto autoConsumed = broker.consume(
      autoApproved.record->approvalId, 10, 22, expected);
  expect(autoConsumed.ok && autoConsumed.grant &&
             autoConsumed.grant->autoApproved && autoConsumed.record &&
             autoConsumed.record->state ==
                 NativeIpc::IpcApprovalState::Consumed &&
             broker
                     .consume(autoApproved.record->approvalId, 10, 22,
                              expected)
                     .code == "approval_not_approved",
         "auto-approved Lua grant must remain target-bound and one-shot");

  const auto denied =
      broker.submit(submission("memory_write", 10, 21, expected));
  const auto denial =
      broker.decide(denied.record->approvalId,
                    NativeIpc::IpcApprovalDecision::Deny, context(99, 99));
  expect(denial.ok &&
             denial.record->state == NativeIpc::IpcApprovalState::Denied &&
             broker.consume(denied.record->approvalId, 10, 21, expected).code ==
                 "approval_not_approved",
         "explicit denial must be terminal without trusting current context");
}

void testContextInvalidatesBeforeAndAfterDecision() {
  NativeIpc::IpcApprovalBroker broker;
  const Mem::OperationContext expected = context();

  const auto before = broker.submit(submission("memory_write", 1, 1, expected));
  const auto invalidBefore =
      broker.decide(before.record->approvalId,
                    NativeIpc::IpcApprovalDecision::Approve, context(7, 3));
  expect(!invalidBefore.ok && invalidBefore.code == "approval_invalidated" &&
             invalidBefore.record->state ==
                 NativeIpc::IpcApprovalState::Invalidated,
         "target revision change must invalidate pending approval");

  const auto after = broker.submit(submission("memory_write", 1, 2, expected));
  expect(broker
             .decide(after.record->approvalId,
                     NativeIpc::IpcApprovalDecision::Approve, expected)
             .ok,
         "second request should approve on matching context");
  const auto invalidAfter =
      broker.consume(after.record->approvalId, 1, 2, context(8, 2));
  expect(!invalidAfter.ok && invalidAfter.code == "approval_invalidated" &&
             invalidAfter.record->state ==
                 NativeIpc::IpcApprovalState::Invalidated,
         "generation change must revoke approved request before execution");

  const auto selection =
      broker.submit(submission("process_open", 1, 3, expected));
  expect(broker.invalidateStale(context(7, 4)) == 1,
         "selection approval must bind the old selection snapshot");
  expect(broker.snapshot().back().state ==
             NativeIpc::IpcApprovalState::Invalidated,
         "bulk invalidation should be visible in snapshot");

  const auto driver =
      broker.submit(submission("driver_initialize", 1, 4, expected));
  expect(broker
             .decide(driver.record->approvalId,
                     NativeIpc::IpcApprovalDecision::Approve, context(7, 99))
             .ok,
         "None policy should ignore target changes within one generation");
  expect(!broker.consume(driver.record->approvalId, 1, 4, context(8, 99)).ok,
         "None policy must still bind connection generation");
}

void testQueueCancellationExpiryAndRetention() {
  NativeIpc::IpcApprovalBroker broker({2, 3});
  const auto first = broker.submit(submission("memory_write", 10, 1));
  const auto second = broker.submit(submission("memory_write", 20, 1));
  expect(first.ok && second.ok,
         "bounded broker should accept two live records");
  expect(broker.submit(submission("memory_write", 30, 1)).code ==
             "approval_queue_full",
         "live approval count must be bounded");

  expect(broker.cancelSession(10) == 1,
         "session cancellation should revoke its live approval");
  const auto third = broker.submit(submission("memory_write", 30, 1));
  expect(third.ok, "terminal record should free pending capacity");
  expect(broker.cancelSession(10) == 0,
         "terminal cancellation must be idempotent");

  expect(broker
             .decide(second.record->approvalId,
                     NativeIpc::IpcApprovalDecision::Approve, context())
             .ok,
         "approved records should remain live until consumed");
  expect(broker.expire(std::chrono::steady_clock::now() + 10s) == 2,
         "expiry should close pending and approved records");
  expect(broker.submit(submission("memory_write", 20, 1)).code ==
             "duplicate_approval",
         "duplicate check must run before terminal history pruning");
  const auto fourth = broker.submit(submission("memory_write", 40, 1));
  expect(fourth.ok && broker.snapshot().size() == 3,
         "new submission should prune oldest terminal history");
  expect(broker.submit(submission("memory_write", 40, 1)).code ==
             "duplicate_approval",
         "duplicate session/request identity must be rejected");
}

void testAuditReceivesBoundedStateTransitions() {
  AuditSink sink;
  NativeIpc::IpcApprovalBroker broker({}, &sink);
  sink.setBroker(&broker);
  const Mem::OperationContext expected = context();
  const auto submitted =
      broker.submit(submission("lua_execute", 9, 7, expected));
  expect(submitted.ok, "Lua request should require approval metadata");
  expect(broker
             .decide(submitted.record->approvalId,
                     NativeIpc::IpcApprovalDecision::Approve, expected)
             .ok,
         "Lua approval should transition to approved");
  expect(broker.consume(submitted.record->approvalId, 9, 7, expected).ok,
         "Lua approval should be consumable once");

  const auto records = sink.records();
  expect(records.size() == 3 &&
             records[0].state == NativeIpc::IpcApprovalState::Pending &&
             records[1].state == NativeIpc::IpcApprovalState::Approved &&
             records[2].state == NativeIpc::IpcApprovalState::Consumed,
         "audit sink should observe every authorization transition");
  expect(sink.reentrantSnapshots() == 3,
         "audit callback must be able to re-enter broker snapshots");
  for (const auto &record : records) {
    expect(record.method == "lua_execute" &&
               record.clientName == "AMem.ApprovalTests" &&
               record.connectionGeneration == 7,
           "audit DTO should contain bounded identity and target metadata");
  }
}

void testConsumeRevalidatesRequestIdentity() {
  NativeIpc::IpcApprovalBroker broker;
  const Mem::OperationContext expected = context();

  const auto wrongSession =
      broker.submit(submission("memory_write", 80, 1, expected));
  expect(broker
             .decide(wrongSession.record->approvalId,
                     NativeIpc::IpcApprovalDecision::Approve, expected)
             .ok,
         "identity fixture should approve");
  const auto sessionResult =
      broker.consume(wrongSession.record->approvalId, 81, 1, expected);
  expect(!sessionResult.ok && !sessionResult.grant &&
             sessionResult.code == "approval_identity_mismatch" &&
             sessionResult.record->state ==
                 NativeIpc::IpcApprovalState::Invalidated,
         "consume must invalidate a mismatched server session");

  const auto wrongRequest =
      broker.submit(submission("memory_write", 80, 2, expected));
  expect(broker
             .decide(wrongRequest.record->approvalId,
                     NativeIpc::IpcApprovalDecision::Approve, expected)
             .ok,
         "request identity fixture should approve");
  const auto requestResult =
      broker.consume(wrongRequest.record->approvalId, 80, 3, expected);
  expect(!requestResult.ok && !requestResult.grant &&
             requestResult.code == "approval_identity_mismatch" &&
             requestResult.record->state ==
                 NativeIpc::IpcApprovalState::Invalidated,
         "consume must invalidate a mismatched request id");
}

void testConsumeAuditFailureBurnsGrant() {
  FailingConsumedAuditSink sink;
  NativeIpc::IpcApprovalBroker broker({}, &sink);
  const Mem::OperationContext expected = context();
  const auto submitted =
      broker.submit(submission("memory_write", 90, 1, expected));
  expect(broker
             .decide(submitted.record->approvalId,
                     NativeIpc::IpcApprovalDecision::Approve, expected)
             .ok,
         "audit failure fixture should approve");

  const auto failed =
      broker.consume(submitted.record->approvalId, 90, 1, expected);
  expect(!failed.ok && !failed.grant &&
             failed.code == "approval_audit_failed" && failed.record &&
             failed.record->state == NativeIpc::IpcApprovalState::Consumed,
         "failed consumed audit must withhold the execution grant");
  expect(broker.consume(submitted.record->approvalId, 90, 1, expected).code ==
             "approval_not_approved",
         "audit failure must burn authorization against retry");
}

void testConsumeSerializesWithSessionCancellation() {
  BlockingConsumedAuditSink sink;
  NativeIpc::IpcApprovalBroker broker({}, &sink);
  const Mem::OperationContext expected = context();
  const auto submitted =
      broker.submit(submission("memory_write", 100, 1, expected));
  expect(broker
             .decide(submitted.record->approvalId,
                     NativeIpc::IpcApprovalDecision::Approve, expected)
             .ok,
         "consume cancellation fixture should approve");

  std::atomic<bool> consumeReturned{false};
  NativeIpc::IpcApprovalResult consumed;
  std::thread consumeThread([&] {
    consumed = broker.consume(submitted.record->approvalId, 100, 1, expected);
    consumeReturned.store(true, std::memory_order_release);
  });
  const bool auditEntered = sink.waitForConsumedAudit();
  const bool returnedBeforeAudit =
      consumeReturned.load(std::memory_order_acquire);
  const size_t cancelled = broker.cancelSession(100);
  const auto state = broker.find(submitted.record->approvalId)->state;
  sink.release();
  consumeThread.join();
  expect(auditEntered && !returnedBeforeAudit && cancelled == 0 &&
             state == NativeIpc::IpcApprovalState::Consumed && consumed.ok &&
             consumed.grant && consumeReturned.load(std::memory_order_acquire),
         "consume should beat cancellation but wait for durable audit");
}

void testConsumeAndSessionCancelRaceIsTerminal() {
  const Mem::OperationContext expected = context();
  for (size_t iteration = 0; iteration < 64; ++iteration) {
    NativeIpc::IpcApprovalBroker broker;
    const auto submitted =
        broker.submit(submission("memory_write", 110, 1, expected));
    expect(broker
               .decide(submitted.record->approvalId,
                       NativeIpc::IpcApprovalDecision::Approve, expected)
               .ok,
           "consume race fixture should approve");

    std::atomic<bool> start{false};
    NativeIpc::IpcApprovalResult consumed;
    size_t cancelled = 0;
    std::thread consumeThread([&] {
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      consumed = broker.consume(submitted.record->approvalId, 110, 1, expected);
    });
    std::thread cancelThread([&] {
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      cancelled = broker.cancelSession(110);
    });
    start.store(true, std::memory_order_release);
    consumeThread.join();
    cancelThread.join();

    const auto final = broker.find(submitted.record->approvalId);
    const bool consumedFirst =
        consumed.ok && consumed.grant && cancelled == 0 && final &&
        final->state == NativeIpc::IpcApprovalState::Consumed;
    const bool cancelledFirst =
        !consumed.ok && !consumed.grant &&
        consumed.code == "approval_not_approved" && cancelled == 1 && final &&
        final->state == NativeIpc::IpcApprovalState::Cancelled;
    expect(consumedFirst || cancelledFirst,
           "consume and session cancellation must have one terminal winner");
  }
}

void testCancelAllRevokesPendingAndApproved() {
  NativeIpc::IpcApprovalBroker broker;
  const Mem::OperationContext expected = context();
  const auto pending =
      broker.submit(submission("memory_write", 50, 1, expected));
  const auto approved =
      broker.submit(submission("memory_write", 60, 1, expected));
  expect(pending.ok && approved.ok &&
             broker
                 .decide(approved.record->approvalId,
                         NativeIpc::IpcApprovalDecision::Approve, expected)
                 .ok,
         "cancel-all fixture should contain pending and approved records");

  expect(broker.cancelAll() == 2,
         "runtime stop should revoke every live approval");
  const auto records = broker.snapshot();
  expect(records.size() == 2 &&
             records[0].state == NativeIpc::IpcApprovalState::Cancelled &&
             records[1].state == NativeIpc::IpcApprovalState::Cancelled &&
             broker.cancelAll() == 0,
         "cancel-all should be terminal and idempotent");
}

void testRequestCancellationAndLookup() {
  NativeIpc::IpcApprovalBroker broker;
  const auto first = broker.submit(submission("memory_write", 70, 1));
  const auto second = broker.submit(submission("memory_write", 70, 2));
  expect(first.ok && second.ok &&
             broker.find(first.record->approvalId).has_value(),
         "request cancellation fixture should be discoverable by id");
  expect(broker.cancelRequest(70, 1) == 1 &&
             broker.find(first.record->approvalId)->state ==
                 NativeIpc::IpcApprovalState::Cancelled &&
             broker.find(second.record->approvalId)->state ==
                 NativeIpc::IpcApprovalState::Pending,
         "request cancellation must only revoke the exact request");
  expect(broker.cancelRequest(70, 1) == 0 &&
             broker.cancelRequest(99, 2) == 0 &&
             !broker.find(9999).has_value(),
         "request cancellation and lookup must be bounded and idempotent");
}

void testConcurrentSubmissionAndInvalidation() {
  NativeIpc::IpcApprovalBroker broker({16, 32});
  std::vector<std::thread> threads;
  std::atomic<int> accepted{0};
  for (uint64_t index = 1; index <= 8; ++index) {
    threads.emplace_back([&, index] {
      if (broker.submit(submission("memory_write", index, 1)).ok) {
        accepted.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }
  expect(accepted.load(std::memory_order_relaxed) == 8 &&
             broker.snapshot().size() == 8,
         "broker should serialize concurrent submissions");
  expect(broker.invalidateStale(context(8, 2)) == 8,
         "generation invalidation should revoke every live record");
}

} // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests = {
      {"catalog validation and bounded DTO",
       &testCatalogOwnedValidationAndBoundedDto},
      {"approve and consume is one-shot", &testApproveConsumeIsOneShot},
      {"context invalidation before and after decision",
       &testContextInvalidatesBeforeAndAfterDecision},
      {"queue cancellation expiry and retention",
       &testQueueCancellationExpiryAndRetention},
      {"audit receives bounded transitions",
       &testAuditReceivesBoundedStateTransitions},
      {"consume revalidates request identity",
       &testConsumeRevalidatesRequestIdentity},
      {"consume audit failure burns grant", &testConsumeAuditFailureBurnsGrant},
      {"consume serializes with session cancellation",
       &testConsumeSerializesWithSessionCancellation},
      {"consume and session cancellation race is terminal",
       &testConsumeAndSessionCancelRaceIsTerminal},
      {"cancel all revokes pending and approved",
       &testCancelAllRevokesPendingAndApproved},
      {"request cancellation and lookup",
       &testRequestCancellationAndLookup},
      {"concurrent submission and invalidation",
       &testConcurrentSubmissionAndInvalidation},
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
