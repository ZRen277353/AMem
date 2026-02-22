#include "ScanWindow.h"
#include "AppContext.h"
#include "ColorScheme.h"
#include "MemoryViewerWindow.h"
#include "EventBus.h"
#include "Gui.h"
#include "../imgui/imgui.h"
#include "../socket/client_singleton.h"
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <vector>


ScanWindow::ScanWindow()
{
    name = "数值扫描";
}

unsigned int ScanWindow::getWindowFlags() const
{
    return ImGuiWindowFlags_NoDocking;
}

ScanWindow::~ScanWindow()
{
    // ScopedThread 析构时自动 requestStop + join
    // 但扫描线程依赖 scanCancelled 标志来中断服务端操作
    scanCancelled = true;
}

void ScanWindow::onDraw()
{
    if (!pOpen) return;

    auto& ctx = AppContext::Get();
    if (ImGui::Begin(name.c_str(), &pOpen, ImGuiWindowFlags_None))
    {
        // 窗口聚焦时的键盘快捷键
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && !ImGui::GetIO().WantTextInput) {
            // F5: 刷新地址值
            if (ImGui::IsKeyPressed(ImGuiKey_F5) && ctx.hasProcess()) {
                refreshAddressValues();
            }
            // Enter/F9: 执行扫描（首次或再次）
            if (ImGui::IsKeyPressed(ImGuiKey_F9) && !scanInProgress && ctx.hasProcess()) {
                if (totalScanResults == 0) {
                    performFirstScanAsync();
                } else {
                    performNextScanAsync();
                }
            }
        }

        if (ctx.hasProcess()) {
            ImGui::TextColored(ColorScheme::SuccessBright, "已附加: %s (PID %d)",
                ctx.selectedName.c_str(), ctx.selectedPid.load());
            ImGui::SameLine();
            if (ImGui::Button("刷新地址值")) {
                refreshAddressValues();
            }
        } else {
            ImGui::TextDisabled("未附加进程");
        }
        ImGui::Separator();

        // 主要布局：左侧扫描面板，右侧结果面板
        drawScanPanel();
        ImGui::SameLine();
        ImGui::BeginChild("RightPanel", ImVec2(0, 0), ImGuiChildFlags_Borders);
        drawResultsPanel();
        drawAddressListPanel();
        ImGui::EndChild();

        drawMemoryTypeSelectionModal();
    }
    ImGui::End();
}

void ScanWindow::updateScanProgress(float progress, uint64_t matchCount, uint64_t scannedBytes, uint64_t totalBytes)
{
    // 使用互斥锁保护，因为此函数会从扫描线程中调用
    std::lock_guard<std::mutex> lock(scanProgressMutex);
    
    scanProgress = progress;
    scanMatchCount = matchCount;
    scanScannedBytes = scannedBytes;
    scanTotalBytes = totalBytes;
}

// 从CEWindow.cpp移植所有扫描相关的方法
