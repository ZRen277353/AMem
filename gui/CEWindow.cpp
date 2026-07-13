#include "CEWindow.h"
#include "AppContext.h"
#include "ColorScheme.h"
#include "Gui.h"
#include "../imgui/imgui.h"
#include "../mem/IMemService.h"
#include "ModulesWindow.h"
#include "ScanWindow.h"
#include "MemoryViewerWindow.h"
#include "BreakpointWindow.h"
#include "LogWindow.h"

#ifdef HAVE_LUAJIT
#include "LuaScriptWindow.h"
#endif
#ifdef HAVE_AI_CHAT
#include "ai/ChatWindow.h"
#endif
#ifdef HAVE_NATIVE_IPC
#include "NativeAgentIpcWindow.h"
#endif
#include "ServerConnectWindow.h"
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <cctype>
#include <vector>

namespace {

bool loadProcesses(Mem::IMemService& service,
                   std::vector<Mem::ProcessInfo>& processes,
                   Mem::Error* error = nullptr) {
    const Mem::OperationContext context = service.captureContext(false);
    std::vector<Mem::ProcessInfo> loaded;
    size_t offset = 0;
    while (true) {
        Mem::ProcessListRequest request;
        request.offset = offset;
        request.limit = Mem::kMaxProcessPageSize;
        auto response = service.listProcesses(context, request);
        if (!response.ok()) {
            if (error) {
                *error = response.error();
            }
            return false;
        }
        const auto& page = response.value();
        loaded.insert(loaded.end(), page.items.begin(), page.items.end());
        if (!page.nextOffset) {
            break;
        }
        offset = *page.nextOffset;
    }
    processes = std::move(loaded);
    return true;
}

} // namespace

CEWindow::CEWindow(Mem::IMemService& memService)
    : memService_(memService)
{
    name = "Cheat Engine";
}

unsigned int CEWindow::getWindowFlags() const
{
    return ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking;
}

void CEWindow::drawMenuBar()
{
    if (ImGui::BeginMenuBar())
    {
        if (ImGui::BeginMenu("文件"))
        {
            if (ImGui::MenuItem("打开进程", "Ctrl+P"))
                openProcessModal = true;
            ImGui::MenuItem("保存", nullptr, false);
            ImGui::MenuItem("退出", nullptr, false);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("编辑"))
        {
            ImGui::MenuItem("首选项", nullptr, false);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("窗口"))
        {
            if (ImGui::MenuItem("数值扫描", "Ctrl+S"))
                openScanWindow();
            if (ImGui::MenuItem("内存查看器", "Ctrl+M"))
                openMemoryViewerWindow();
            if (ImGui::MenuItem("断点调试", "Ctrl+B"))
                openBreakpointWindow();
            ImGui::Separator();
            if (ImGui::MenuItem("Lua脚本管理器", "Ctrl+L"))
                openLuaScriptWindow();
            if (ImGui::MenuItem("服务器连接"))
                openServerConnectWindow();
            if (ImGui::MenuItem("日志"))
                openLogWindow();
#ifdef HAVE_AI_CHAT
            ImGui::Separator();
            if (ImGui::MenuItem("AI 聊天"))
                openAIChatWindow();
#endif
#ifdef HAVE_NATIVE_IPC
            if (ImGui::MenuItem("Native Agent IPC"))
                openNativeAgentIpcWindow();
#endif
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
}

void CEWindow::drawTopProcessBar()
{
    auto& ctx = AppContext::Get();
    ImGui::PushItemWidth(260.0f);
    if (ImGui::Button("选择进程")) { openProcessModal = true; }
    ImGui::SameLine();
    ImGui::TextDisabled("PID: %d", ctx.selectedPid.load());
    ImGui::PopItemWidth();
    ImGui::Separator();
}

void CEWindow::drawSelectedProcessBanner()
{
    auto& ctx = AppContext::Get();
    int pid = ctx.selectedPid.load();
    if (pid != 0) {
        const std::string processName = ctx.getSelectedName();
        ImGui::TextColored(ColorScheme::SuccessBright, "已附加: %s (PID %d)", processName.c_str(), pid);
        ImGui::SameLine();
        if (ImGui::Button("模块列表")) {
            openModulesWindow();
        }
    } else {
        ImGui::TextDisabled("未附加进程");
    }
}

void CEWindow::drawProcessSelectModal()
{
    static std::vector<Mem::ProcessInfo> list;
    static char filterText[256] = "";
    static bool listLoadAttempted = false;

    if (openProcessModal)
        ImGui::OpenPopup("进程列表");

    if (ImGui::BeginPopupModal("进程列表", &openProcessModal, ImGuiWindowFlags_AlwaysAutoResize))
    {
        if (ImGui::Button("刷新")) {
            list.clear();
            listLoadAttempted = true;
            Mem::Error error;
            if (!loadProcesses(memService_, list, &error))
                Gui::log("获取进程列表失败 [%s]: %s",
                         Mem::errorCodeName(error.code), error.message.c_str());
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(450.0f);
        ImGui::InputTextWithHint("##filter", "输入进程名称进行过滤...", filterText, sizeof(filterText));

        ImGui::Separator();
        if (!listLoadAttempted) {
            list.clear();
            listLoadAttempted = true;
            Mem::Error error;
            if (!loadProcesses(memService_, list, &error))
                Gui::log("获取进程列表失败 [%s]: %s",
                         Mem::errorCodeName(error.code), error.message.c_str());
        }

        if (ImGui::BeginChild("proc_modal", ImVec2(600, 400), ImGuiChildFlags_Borders))
        {
            if (ImGui::BeginTable("proc_modal_tbl", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable)) {
                ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_WidthFixed, 120.0f);
                ImGui::TableSetupColumn("进程名", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();

                // 应用过滤器
                std::string filterStr(filterText);
                for (size_t i = 0; i < filterStr.length(); i++) {
                    filterStr[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(filterStr[i])));
                }

                for (auto& it : list) {
                    if (filterStr.length() > 0) {
                        std::string nameLower = it.name;
                        for (size_t i = 0; i < nameLower.length(); i++) {
                            nameLower[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(nameLower[i])));
                        }
                        if (nameLower.find(filterStr) == std::string::npos) {
                            continue;
                        }
                    }

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("%d", it.pid);
                    ImGui::TableSetColumnIndex(1);
                    if (ImGui::Selectable(it.name.c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) {
                        Mem::OpenProcessRequest request;
                        request.pid = it.pid;
                        request.name = it.name;
                        auto response = memService_.openProcess(
                            memService_.captureContext(true), request);
                        if (response.ok()) {
                            Gui::log("进程已打开: %s (PID %d, handle %d)",
                                     response.value().name.c_str(),
                                     response.value().target.pid,
                                     response.value().target.processHandle);
                            ImGui::CloseCurrentPopup();
                            openProcessModal = false;
                            listLoadAttempted = false;
                        } else {
                            Gui::log("打开进程失败 [%s]: %s",
                                     Mem::errorCodeName(response.error().code),
                                     response.error().message.c_str());
                        }
                    }
                }
                ImGui::EndTable();
            }
        }
        ImGui::EndChild();
        if (ImGui::Button("关闭")) {
            ImGui::CloseCurrentPopup();
            openProcessModal = false;
            listLoadAttempted = false;
        }
        ImGui::EndPopup();
    } else if (!openProcessModal) {
        listLoadAttempted = false;
    }
}

void CEWindow::onDraw()
{
    auto& ctx = AppContext::Get();
    ImGuiStyle& style = ImGui::GetStyle();
    float old_window_rounding = style.WindowRounding;
    float old_frame_rounding = style.FrameRounding;
    style.WindowRounding = 2.0f;
    style.FrameRounding = 2.0f;

    ImGui::SetNextWindowSize(ImVec2(800, 400), ImGuiCond_Once);

    // 全局键盘快捷键（不在文本输入框中时生效）
    if (!ImGui::GetIO().WantTextInput) {
        ImGuiIO& io = ImGui::GetIO();
        if (io.KeyCtrl) {
            if (ImGui::IsKeyPressed(ImGuiKey_P)) { openProcessModal = true; }
            else if (ImGui::IsKeyPressed(ImGuiKey_S)) { openScanWindow(); }
            else if (ImGui::IsKeyPressed(ImGuiKey_M)) { openMemoryViewerWindow(); }
            else if (ImGui::IsKeyPressed(ImGuiKey_B)) { openBreakpointWindow(); }
            else if (ImGui::IsKeyPressed(ImGuiKey_L)) { openLuaScriptWindow(); }
        }
    }

    drawMenuBar();
    drawSelectedProcessBanner();
    drawTopProcessBar();

    ImGui::Text("CheatEngine 主控制面板");
    ImGui::Separator();

    const std::string processName = ctx.getSelectedName();
    ImGui::Text("当前进程: %s (PID: %d)", processName.c_str(), ctx.selectedPid.load());
    ImGui::Spacing();

    ImGui::Text("可用功能窗口:");
    if (ImGui::Button("数值扫描", ImVec2(200, 40))) {
        openScanWindow();
    }
    ImGui::SameLine();
    if (ImGui::Button("内存查看器", ImVec2(200, 40))) {
        openMemoryViewerWindow();
    }
    ImGui::SameLine();
    if (ImGui::Button("断点调试", ImVec2(200, 40))) {
        openBreakpointWindow();
    }
    ImGui::SameLine();
    if (ImGui::Button("SDK解析器", ImVec2(200, 40))) {
    }

    drawProcessSelectModal();

    style.WindowRounding = old_window_rounding;
    style.FrameRounding = old_frame_rounding;
}

// 窗口管理方法 — 使用 Gui::getOrCreate 简化
void CEWindow::openScanWindow()
{
    Gui::getOrCreate<ScanWindow>(memService_);
}

void CEWindow::openMemoryViewerWindow()
{
    Gui::getOrCreate<MemoryViewerWindow>(memService_);
}

void CEWindow::openBreakpointWindow()
{
    Gui::getOrCreate<BreakpointWindow>(memService_);
}

void CEWindow::openModulesWindow()
{
    auto* mw = Gui::getOrCreate<ModulesWindow>(memService_);
    if (mw) mw->triggerAutoRefresh();
}

void CEWindow::openLuaScriptWindow()
{
#ifdef HAVE_LUAJIT
    Gui::getOrCreate<LuaScriptWindow>(memService_);
#endif
}

void CEWindow::openServerConnectWindow()
{
    Gui::getOrCreate<ServerConnectWindow>(memService_);
}

void CEWindow::openLogWindow()
{
    Gui::getOrCreate<LogWindow>();
}

#ifdef HAVE_AI_CHAT
void CEWindow::openAIChatWindow()
{
    Gui::getOrCreate<AI::ChatWindow>();
}
#endif

#ifdef HAVE_NATIVE_IPC
void CEWindow::openNativeAgentIpcWindow()
{
    Gui::getOrCreate<NativeAgentIpcWindow>();
}
#endif
