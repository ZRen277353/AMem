#pragma once

#include "IpcApprovalAudit.h"

#include <vector>

namespace NativeIpc {

class NativeAgentRuntime;

// Lazily constructs product runtime/approval services on first use. Shutdown
// does not create either service when native IPC was never opened or used.
NativeAgentRuntime &GetSystemNativeAgentRuntime();
IpcApprovalBroker &GetSystemIpcApprovalBroker();
IpcApprovalAuditSnapshot GetSystemIpcApprovalAuditSnapshot();
std::vector<IpcApprovalRecord> RefreshSystemIpcApprovals();
IpcApprovalResult DecideSystemIpcApproval(uint64_t approvalId,
                                          IpcApprovalDecision decision);
void StopSystemNativeAgentRuntime();
void ShutdownSystemNativeAgentRuntime();

} // namespace NativeIpc
