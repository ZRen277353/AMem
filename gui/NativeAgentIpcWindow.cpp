#include "NativeAgentIpcWindow.h"

#include "ColorScheme.h"
#include "Gui.h"
#include "imgui.h"
#include "ipc/NativeAgentRuntime.h"
#include "ipc/SystemNativeAgentRuntime.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <string>
#include <vector>

namespace {

std::string utf8(const std::wstring &value) {
  if (value.empty()) {
    return {};
  }
  const int required = ::WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
      static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
  if (required <= 0) {
    return "<unavailable>";
  }
  std::string result(static_cast<size_t>(required), '\0');
  if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(),
                            required, nullptr, nullptr) != required) {
    return "<unavailable>";
  }
  return result;
}

const char *serverStateName(NativeIpc::ServerState state) {
  switch (state) {
  case NativeIpc::ServerState::Stopped:
    return "已停止";
  case NativeIpc::ServerState::Listening:
    return "监听中";
  case NativeIpc::ServerState::Connected:
    return "已连接";
  case NativeIpc::ServerState::Stopping:
    return "停止中";
  case NativeIpc::ServerState::Failed:
    return "失败";
  }
  return "未知";
}

const char *phaseName(NativeIpc::RuntimePhase phase) {
  switch (phase) {
  case NativeIpc::RuntimePhase::Idle:
    return "空闲";
  case NativeIpc::RuntimePhase::Handshaking:
    return "握手中";
  case NativeIpc::RuntimePhase::Serving:
    return "服务中";
  case NativeIpc::RuntimePhase::Stopping:
    return "停止中";
  case NativeIpc::RuntimePhase::Failed:
    return "失败";
  }
  return "未知";
}

const char *handshakeStatusName(NativeIpc::HandshakeStatus status) {
  switch (status) {
  case NativeIpc::HandshakeStatus::Established:
    return "已建立";
  case NativeIpc::HandshakeStatus::Rejected:
    return "已拒绝";
  case NativeIpc::HandshakeStatus::Closed:
    return "已关闭";
  case NativeIpc::HandshakeStatus::Cancelled:
    return "已取消";
  case NativeIpc::HandshakeStatus::TimedOut:
    return "超时";
  case NativeIpc::HandshakeStatus::ProtocolError:
    return "协议错误";
  case NativeIpc::HandshakeStatus::IoError:
    return "I/O 错误";
  }
  return "未知";
}

const char *sessionStatusName(NativeIpc::RequestSessionStatus status) {
  switch (status) {
  case NativeIpc::RequestSessionStatus::Closed:
    return "已关闭";
  case NativeIpc::RequestSessionStatus::Cancelled:
    return "已取消";
  case NativeIpc::RequestSessionStatus::IdleTimedOut:
    return "空闲超时";
  case NativeIpc::RequestSessionStatus::RequestLimitReached:
    return "请求数已达上限";
  case NativeIpc::RequestSessionStatus::Invalidated:
    return "目标已失效";
  case NativeIpc::RequestSessionStatus::ProtocolError:
    return "协议错误";
  case NativeIpc::RequestSessionStatus::IoError:
    return "I/O 错误";
  }
  return "未知";
}

bool isRunning(NativeIpc::ServerState state) {
  return state == NativeIpc::ServerState::Listening ||
         state == NativeIpc::ServerState::Connected;
}

void textRow(const char *label, const char *value) {
  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  ImGui::TextDisabled("%s", label);
  ImGui::TableSetColumnIndex(1);
  ImGui::TextUnformatted(value);
}

void countRow(const char *label, uint64_t value) {
  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  ImGui::TextDisabled("%s", label);
  ImGui::TableSetColumnIndex(1);
  ImGui::Text("%llu", static_cast<unsigned long long>(value));
}

const char *capabilityName(NativeIpc::IpcCapability capability) {
  return NativeIpc::CapabilityName(capability);
}

std::string approvalTarget(const NativeIpc::IpcApprovalRecord &record) {
  if (!record.target) {
    return "connection g" + std::to_string(record.connectionGeneration);
  }
  return "PID " + std::to_string(record.target->pid) + " / r" +
         std::to_string(record.target->processRevision) + " / g" +
         std::to_string(record.connectionGeneration);
}

long long remainingApprovalSeconds(const NativeIpc::IpcApprovalRecord &record) {
  const auto remaining = std::chrono::ceil<std::chrono::seconds>(
      record.deadline - std::chrono::steady_clock::now());
  return (std::max)(0ll, remaining.count());
}

void drawPendingApprovals(
    const std::vector<NativeIpc::IpcApprovalRecord> &records) {
  size_t pendingCount = 0;
  size_t approvedCount = 0;
  for (const auto &record : records) {
    pendingCount += record.state == NativeIpc::IpcApprovalState::Pending;
    approvedCount += record.state == NativeIpc::IpcApprovalState::Approved;
  }

  ImGui::SeparatorText("特权审批");
  ImGui::TextDisabled("待处理: %zu", pendingCount);
  ImGui::SameLine();
  ImGui::TextDisabled("已批准待消费: %zu", approvedCount);
  if (pendingCount == 0) {
    return;
  }

  if (!ImGui::BeginTable("native_ipc_approvals", 6,
                         ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                             ImGuiTableFlags_SizingStretchProp)) {
    return;
  }
  ImGui::TableSetupColumn("客户端", ImGuiTableColumnFlags_WidthStretch, 1.2f);
  ImGui::TableSetupColumn("方法", ImGuiTableColumnFlags_WidthStretch, 1.2f);
  ImGui::TableSetupColumn("权限", ImGuiTableColumnFlags_WidthFixed, 110.0f);
  ImGui::TableSetupColumn("目标", ImGuiTableColumnFlags_WidthStretch, 1.3f);
  ImGui::TableSetupColumn("剩余", ImGuiTableColumnFlags_WidthFixed, 70.0f);
  ImGui::TableSetupColumn("决策", ImGuiTableColumnFlags_WidthFixed, 120.0f);
  ImGui::TableHeadersRow();

  for (const auto &record : records) {
    if (record.state != NativeIpc::IpcApprovalState::Pending) {
      continue;
    }
    const std::string approvalKey = std::to_string(record.approvalId);
    ImGui::PushID(approvalKey.c_str());
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted(record.clientName.c_str());
    ImGui::TableSetColumnIndex(1);
    ImGui::TextUnformatted(record.method.c_str());
    ImGui::TableSetColumnIndex(2);
    ImGui::TextUnformatted(capabilityName(record.capability));
    ImGui::TableSetColumnIndex(3);
    const std::string target = approvalTarget(record);
    ImGui::TextUnformatted(target.c_str());
    ImGui::TableSetColumnIndex(4);
    ImGui::Text("%llds", remainingApprovalSeconds(record));
    ImGui::TableSetColumnIndex(5);
    if (ImGui::SmallButton("批准")) {
      const NativeIpc::IpcApprovalResult result =
          NativeIpc::DecideSystemIpcApproval(
              record.approvalId, NativeIpc::IpcApprovalDecision::Approve);
      Gui::log("Native IPC approval %llu: %s",
               static_cast<unsigned long long>(record.approvalId),
               result.ok ? "approved" : result.code.c_str());
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("拒绝")) {
      const NativeIpc::IpcApprovalResult result =
          NativeIpc::DecideSystemIpcApproval(
              record.approvalId, NativeIpc::IpcApprovalDecision::Deny);
      Gui::log("Native IPC approval %llu: %s",
               static_cast<unsigned long long>(record.approvalId),
               result.ok ? "denied" : result.code.c_str());
    }
    ImGui::PopID();
  }
  ImGui::EndTable();
}

std::string auditTimestamp(int64_t timestampMs) {
  const std::time_t seconds = static_cast<std::time_t>(timestampMs / 1000);
  std::tm local{};
  if (::localtime_s(&local, &seconds) != 0) {
    return "-";
  }
  char value[32]{};
  if (std::strftime(value, sizeof(value), "%m-%d %H:%M:%S", &local) == 0) {
    return "-";
  }
  return value;
}

void drawApprovalAudit(
    const NativeIpc::IpcApprovalAuditSnapshot &snapshot) {
  ImGui::SeparatorText("审批审计");
  ImGui::TextDisabled("%s", snapshot.filepath.c_str());
  ImGui::TextDisabled("本次写入: %llu",
                      static_cast<unsigned long long>(
                          snapshot.successfulWrites));
  ImGui::SameLine();
  ImGui::TextDisabled("失败: %llu",
                      static_cast<unsigned long long>(snapshot.failedWrites));
  if (!snapshot.lastError.empty()) {
    ImGui::TextColored(ColorScheme::Error, "%s", snapshot.lastError.c_str());
  }
  if (snapshot.recent.empty()) {
    return;
  }

  if (!ImGui::BeginTable("native_ipc_approval_audit", 6,
                         ImGuiTableFlags_RowBg |
                             ImGuiTableFlags_BordersInnerH |
                             ImGuiTableFlags_SizingStretchProp)) {
    return;
  }
  ImGui::TableSetupColumn("时间", ImGuiTableColumnFlags_WidthFixed, 100.0f);
  ImGui::TableSetupColumn("客户端", ImGuiTableColumnFlags_WidthStretch, 1.2f);
  ImGui::TableSetupColumn("方法", ImGuiTableColumnFlags_WidthStretch, 1.2f);
  ImGui::TableSetupColumn("状态", ImGuiTableColumnFlags_WidthFixed, 90.0f);
  ImGui::TableSetupColumn("会话", ImGuiTableColumnFlags_WidthFixed, 70.0f);
  ImGui::TableSetupColumn("请求", ImGuiTableColumnFlags_WidthFixed, 70.0f);
  ImGui::TableHeadersRow();

  const size_t first = snapshot.recent.size() > 20
                           ? snapshot.recent.size() - 20
                           : 0;
  for (size_t index = snapshot.recent.size(); index > first; --index) {
    const auto &entry = snapshot.recent[index - 1];
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    const std::string timestamp = auditTimestamp(entry.timestampMs);
    ImGui::TextUnformatted(timestamp.c_str());
    ImGui::TableSetColumnIndex(1);
    ImGui::TextUnformatted(entry.clientName.c_str());
    ImGui::TableSetColumnIndex(2);
    ImGui::TextUnformatted(entry.method.c_str());
    ImGui::TableSetColumnIndex(3);
    ImGui::TextUnformatted(entry.state.c_str());
    ImGui::TableSetColumnIndex(4);
    ImGui::Text("%llu", static_cast<unsigned long long>(entry.sessionId));
    ImGui::TableSetColumnIndex(5);
    ImGui::Text("%llu", static_cast<unsigned long long>(entry.requestId));
  }
  ImGui::EndTable();
}

} // namespace

NativeAgentIpcWindow::NativeAgentIpcWindow() { name = "Native Agent IPC"; }

void NativeAgentIpcWindow::onDraw() {
  NativeIpc::NativeAgentRuntime &runtime =
      NativeIpc::GetSystemNativeAgentRuntime();
  NativeIpc::NativeAgentRuntimeSnapshot snapshot = runtime.snapshot();
  const bool running = isRunning(snapshot.server.state);

  const ImVec4 statusColor =
      running ? ColorScheme::SuccessBright : ColorScheme::TextDisabled;
  ImGui::TextColored(statusColor, "%s", serverStateName(snapshot.server.state));
  ImGui::SameLine();
  if (running) {
    if (ImGui::Button("停止")) {
      NativeIpc::StopSystemNativeAgentRuntime();
      Gui::log("Native Agent IPC 已停止");
      snapshot = runtime.snapshot();
    }
  } else if (ImGui::Button("启用")) {
    std::wstring error;
    if (runtime.start(error)) {
      Gui::log("Native Agent IPC 已启用（Observe）");
    } else {
      const std::string message = utf8(error);
      Gui::log("Native Agent IPC 启用失败: %s", message.c_str());
    }
    snapshot = runtime.snapshot();
  }

  ImGui::Separator();
  if (ImGui::BeginTable("native_ipc_status", 2,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                            ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("项目", ImGuiTableColumnFlags_WidthFixed, 150.0f);
    ImGui::TableSetupColumn("状态", ImGuiTableColumnFlags_WidthStretch);
    textRow("权限", "Observe");
    textRow("特权能力", "禁用");
    textRow("运行阶段", phaseName(snapshot.phase));
    const std::string pipeName = utf8(snapshot.server.pipeName);
    textRow("管道", pipeName.c_str());
    countRow("已接受连接", snapshot.server.acceptedConnections);
    countRow("已建立会话", snapshot.establishedSessions);
    countRow("已完成会话", snapshot.completedSessions);
    if (snapshot.activeSessionId != 0) {
      countRow("当前会话 ID", snapshot.activeSessionId);
    }
    if (snapshot.lastSessionId != 0) {
      countRow("最近会话 ID", snapshot.lastSessionId);
    }
    if (!snapshot.activeClientName.empty()) {
      textRow("当前客户端", snapshot.activeClientName.c_str());
      textRow("客户端版本", snapshot.activeClientVersion.empty()
                                ? "未提供"
                                : snapshot.activeClientVersion.c_str());
    }
    if (snapshot.lastHandshakeStatus) {
      textRow("最近握手", handshakeStatusName(*snapshot.lastHandshakeStatus));
    }
    if (snapshot.lastSessionStatus) {
      textRow("最近会话", sessionStatusName(*snapshot.lastSessionStatus));
    }
    countRow("最近请求", snapshot.lastRequestFrames);
    countRow("最近调度", snapshot.lastDispatchedRequests);
    countRow("最近响应", snapshot.lastResponsesSent);
    countRow("最近取消", snapshot.lastCancellationsObserved);
    ImGui::EndTable();
  }

  if (!snapshot.lastError.empty()) {
    const std::string error = utf8(snapshot.lastError);
    ImGui::Separator();
    ImGui::TextColored(ColorScheme::Error, "%s", error.c_str());
  }

  drawPendingApprovals(NativeIpc::RefreshSystemIpcApprovals());
  drawApprovalAudit(NativeIpc::GetSystemIpcApprovalAuditSnapshot());
}
