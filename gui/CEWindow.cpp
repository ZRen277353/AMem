#include "CEWindow.h"
#include "Gui.h"
#include "../imgui/imgui.h"
#include "../socket/client_singleton.h"
#include "ModulesWindow.h"
#include "ScanWindow.h"
#include "MemoryViewerWindow.h"
#include "BreakpointWindow.h"
#include "PointerChainWindow.h"
#include "LogWindow.h"

#ifdef HAVE_LUAJIT
#include "LuaScriptWindow.h"
#endif
#include "ServerConnectWindow.h"
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <vector>

CEWindow::CEWindow()
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
            if (ImGui::MenuItem("打开进程"))
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
            if (ImGui::MenuItem("Lua脚本管理器"))
                openLuaScriptWindow();
            if (ImGui::MenuItem("服务器连接"))
                openServerConnectWindow();
            if (ImGui::MenuItem("日志"))
                openLogWindow();
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
}

void CEWindow::drawTopProcessBar()
{
    ImGui::PushItemWidth(260.0f);
    if (ImGui::Button("选择进程")) { openProcessModal = true; }
    ImGui::SameLine();
    ImGui::TextDisabled("PID: %d", selectedPid);
    ImGui::PopItemWidth();
    ImGui::Separator();
}

void CEWindow::drawSelectedProcessBanner()
{
    if (selectedPid != 0) {
        ImGui::TextColored(ImVec4(0.6f, 0.9f, 0.6f, 1.0f), "已附加: %s (PID %d)", selectedName.c_str(), selectedPid);
        ImGui::SameLine();
        if (ImGui::Button("模块列表")) {
            auto list = Gui::getWindows<ModulesWindow>();
            ModulesWindow* mw = nullptr;
            if (list.empty()) {
                mw = new ModulesWindow();
                Gui::addWindow(mw);
            } else {
                mw = list.front();
            }
            if (mw) {
                mw->pOpen = true;
                mw->shouldBringToFront = true;
                mw->triggerAutoRefresh();
            }
        }
    } else {
        ImGui::TextDisabled("未附加进程");
    }
}

void CEWindow::drawProcessSelectModal()
{
    if (openProcessModal)
        ImGui::OpenPopup("进程列表");

    if (ImGui::BeginPopupModal("进程列表", &openProcessModal, ImGuiWindowFlags_AlwaysAutoResize))
    {
        static std::vector<ProcessInfoItem> list;
        static char filterText[256] = "";
        
        if (ImGui::Button("刷新")) {
            list.clear();
            if (!FetchProcessList(list))
                Gui::log("获取进程列表失败");
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(450.0f);
        ImGui::InputTextWithHint("##filter", "输入进程名称进行过滤...", filterText, sizeof(filterText));
        
        ImGui::Separator();
        if (ImGui::BeginChild("proc_modal", ImVec2(600, 400), ImGuiChildFlags_Borders))
        {
            if (ImGui::BeginTable("proc_modal_tbl", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable)) {
                ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_WidthFixed, 120.0f);
                ImGui::TableSetupColumn("进程名", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();
                if (list.empty()) {
                    FetchProcessList(list);
                }
                
                // 应用过滤器
                std::string filterStr(filterText);
                for (size_t i = 0; i < filterStr.length(); i++) {
                    filterStr[i] = tolower(filterStr[i]);
                }
                
                for (auto& it : list) {
                    // 如果有过滤文本，检查进程名是否包含过滤文本（不区分大小写）
                    if (filterStr.length() > 0) {
                        std::string nameLower = it.name;
                        for (size_t i = 0; i < nameLower.length(); i++) {
                            nameLower[i] = tolower(nameLower[i]);
                        }
                        if (nameLower.find(filterStr) == std::string::npos) {
                            continue; // 不匹配，跳过
                        }
                    }
                    
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("%d", it.pid);
                    ImGui::TableSetColumnIndex(1);
                    if (ImGui::Selectable(it.name.c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) {
                        selectedPid = it.pid;
                        selectedName = it.name;
                        SetCurrentPid(selectedPid);
                        int handle = 0;
                        if (OpenProcessHandle(selectedPid, handle))
                            Gui::log("进程已打开，句柄=%d", handle);
                        else
                            Gui::log("无法打开进程句柄 %d", selectedPid);
                        ImGui::CloseCurrentPopup();
                        openProcessModal = false;
                    }
                }
                ImGui::EndTable();
            }
        }
        ImGui::EndChild();
        if (ImGui::Button("关闭")) { ImGui::CloseCurrentPopup(); openProcessModal = false; }
        ImGui::EndPopup();
    }
}

void CEWindow::onDraw()
{
    ImGuiStyle& style = ImGui::GetStyle();
    float old_window_rounding = style.WindowRounding;
    float old_frame_rounding = style.FrameRounding;
    style.WindowRounding = 2.0f;
    style.FrameRounding = 2.0f;

    ImGui::SetNextWindowSize(ImVec2(800, 400), ImGuiCond_Once);

    drawMenuBar();
    drawSelectedProcessBanner();
    drawTopProcessBar();

    // 显示简单的主窗口内容
    ImGui::Text("CheatEngine 主控制面板");
    ImGui::Separator();
    
    //if (selectedPid != 0) {
        ImGui::Text("当前进程: %s (PID: %d)", selectedName.c_str(), selectedPid);
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
        if (ImGui::Button("指针链图表", ImVec2(200, 40))) {
            openPointerChainWindow();
        }
          ImGui::SameLine();
        if (ImGui::Button("SDK解析器", ImVec2(200, 40))) {
          
        }
    // } else {
    //     ImGui::TextDisabled("请先选择一个进程以开始使用功能");
    //     ImGui::Text("使用 文件 -> 打开进程 或点击上方的 '选择进程' 按钮");
    // }

    drawProcessSelectModal();

    style.WindowRounding = old_window_rounding;
    style.FrameRounding = old_frame_rounding;
}

// 窗口管理方法实现
void CEWindow::openScanWindow()
{
    if (!scanWindow) {
        scanWindow = new ScanWindow();
        scanWindow->setProcessInfo(&selectedPid, &selectedName);
        // 如果内存查看器已存在，设置引用
        if (memoryViewerWindow) {
            scanWindow->setMemoryViewerWindow(memoryViewerWindow);
        }
        // 设置回调函数，用于在需要时自动打开内存查看器
        scanWindow->setOpenMemoryViewerCallback([this]() -> MemoryViewerWindow* {
            openMemoryViewerWindow();
            return memoryViewerWindow;
        });
        Gui::addWindow(scanWindow);
    }
    scanWindow->pOpen = true;
    scanWindow->shouldBringToFront = true;
}

void CEWindow::openMemoryViewerWindow()
{
    if (!memoryViewerWindow) {
        memoryViewerWindow = new MemoryViewerWindow();
        memoryViewerWindow->setProcessInfo(&selectedPid, &selectedName);
        
        // 将内存查看器引用传递给其他已存在的窗口
        if (breakpointWindow) {
            breakpointWindow->setMemoryViewerWindow(memoryViewerWindow);
        }
        if (scanWindow) {
            scanWindow->setMemoryViewerWindow(memoryViewerWindow);
        }
        if (pointerChainWindow) {
            pointerChainWindow->setMemoryViewerWindow(memoryViewerWindow);
        }
        
        // 将引用传递给所有ModulesWindow实例
        auto modulesList = Gui::getWindows<ModulesWindow>();
        for (auto* mw : modulesList) {
            mw->setMemoryViewerWindow(memoryViewerWindow);
        }
        
        Gui::addWindow(memoryViewerWindow);
    }
    memoryViewerWindow->pOpen = true;
    memoryViewerWindow->shouldBringToFront = true;
}

void CEWindow::openBreakpointWindow()
{
    if (!breakpointWindow) {
        breakpointWindow = new BreakpointWindow();
        breakpointWindow->setProcessInfo(&selectedPid, &selectedName);
        // 如果内存查看器已存在，设置引用
        if (memoryViewerWindow) {
            breakpointWindow->setMemoryViewerWindow(memoryViewerWindow);
        }
        // 设置回调函数，用于在需要时自动打开内存查看器
        breakpointWindow->setOpenMemoryViewerCallback([this]() -> MemoryViewerWindow* {
            openMemoryViewerWindow();
            return memoryViewerWindow;
        });
        Gui::addWindow(breakpointWindow);
    }
    breakpointWindow->pOpen = true;
    breakpointWindow->shouldBringToFront = true;
}

void CEWindow::openModulesWindow()
{
    auto list = Gui::getWindows<ModulesWindow>();
    ModulesWindow* mw = nullptr;
    if (list.empty()) {
        mw = new ModulesWindow();
        // 如果内存查看器已存在，设置引用
        if (memoryViewerWindow) {
            mw->setMemoryViewerWindow(memoryViewerWindow);
        }
        // 设置回调函数，用于在需要时自动打开内存查看器
        mw->setOpenMemoryViewerCallback([this]() -> MemoryViewerWindow* {
            openMemoryViewerWindow();
            return memoryViewerWindow;
        });
        Gui::addWindow(mw);
    } else {
        mw = list.front();
    }
    if (mw) {
        mw->pOpen = true;
        mw->shouldBringToFront = true;
        mw->triggerAutoRefresh();
    }
}

void CEWindow::openPointerChainWindow()
{
    if (!pointerChainWindow) {
        pointerChainWindow = new PointerChainWindow();
        pointerChainWindow->setProcessInfo(&selectedPid, &selectedName);
        // 如果内存查看器已存在，设置引用
        if (memoryViewerWindow) {
            pointerChainWindow->setMemoryViewerWindow(memoryViewerWindow);
        }
        // 设置回调函数，用于在需要时自动打开内存查看器
        pointerChainWindow->setOpenMemoryViewerCallback([this]() -> MemoryViewerWindow* {
            openMemoryViewerWindow();
            return memoryViewerWindow;
        });
        Gui::addWindow(pointerChainWindow);
    }
    pointerChainWindow->pOpen = true;
    pointerChainWindow->shouldBringToFront = true;
}

void CEWindow::openLuaScriptWindow()
{
#ifdef HAVE_LUAJIT
    auto list = Gui::getWindows<LuaScriptWindow>();
    LuaScriptWindow* lsw = nullptr;
    if (list.empty()) {
        lsw = new LuaScriptWindow();
        Gui::addWindow(lsw);
    } else {
        lsw = list.front();
    }
    if (lsw) {
        lsw->pOpen = true;
        lsw->shouldBringToFront = true;
    }
#endif
}

void CEWindow::openServerConnectWindow()
{
    auto list = Gui::getWindows<ServerConnectWindow>();
    ServerConnectWindow* scw = nullptr;
    if (list.empty()) {
        scw = new ServerConnectWindow();
        Gui::addWindow(scw);
    } else {
        scw = list.front();
    }
    if (scw) {
        scw->pOpen = true;
        scw->shouldBringToFront = true;
    }
}

void CEWindow::openLogWindow()
{
    auto list = Gui::getWindows<LogWindow>();
    LogWindow* lw = nullptr;
    if (list.empty()) {
        lw = new LogWindow();
        Gui::addWindow(lw);
    } else {
        lw = list.front();
    }
    if (lw) {
        lw->pOpen = true;
        lw->shouldBringToFront = true;
    }
}


// void CEWindow::TodoDraw()
// {
//     ImGui::Text("TodoDraw");
// }