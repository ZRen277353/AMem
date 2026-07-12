#include "ScanWindow.h"
#include "AppContext.h"
#include "ColorScheme.h"
#include "MemoryViewerWindow.h"
#include "EventBus.h"
#include "Gui.h"
#include "../imgui/imgui.h"
#include "../mem/IMemService.h"
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <vector>


ScanWindow::ScanWindow(Mem::IMemService& memService)
    : memService_(memService)
{
    name = "数值扫描";
    observedProcessRevision = AppContext::Get().processRevision.load(std::memory_order_acquire);
}

unsigned int ScanWindow::getWindowFlags() const
{
    return ImGuiWindowFlags_NoDocking;
}

ScanWindow::~ScanWindow()
{
    requestScanCancellation();
    scanThread.stop();
    scanResultsRefreshThread.stop();
    addressListRefreshThread.stop();
}

void ScanWindow::requestScanCancellation()
{
    scanCancelled = true;
    if (scanCancellation_) {
        scanCancellation_->store(true, std::memory_order_release);
    }
}

void ScanWindow::resetProcessState()
{
    requestScanCancellation();
    scanThread.stop();
    scanResultsRefreshThread.stop();
    addressListRefreshThread.stop();

    {
        std::lock_guard<std::mutex> lock(scanResultsMutex);
        scanResults.clear();
        selectedScanResults.clear();
    }
    {
        std::lock_guard<std::mutex> lock(addressListMutex);
        addressList.clear();
    }

    totalScanResults = 0;
    resultOffset = 0;
    scanCompleted = false;
    scanError = false;
    scanCancelled = false;
    scanCancellation_.reset();
    scanInProgress = false;
    scanEpoch = 0;
    scanResultsRefreshProgress = 0;
    scanResultsRefreshTotal = 0;
    addressListRefreshProgress = 0;
    addressListRefreshTotal = 0;
    scanResultsFirstRefreshLog = true;
    scanResultsLargePageWarningShown = false;
    resultContextMenuAddress = 0;
    resultContextMenuValue.clear();
    resultContextMenuValueType = 0;

    {
        std::lock_guard<std::mutex> lock(scanProgressMutex);
        scanProgress = 0.0f;
        scanMatchCount = 0;
        scanTotalBytes = 0;
        scanScannedBytes = 0;
    }
}

void ScanWindow::onDraw()
{
    auto& ctx = AppContext::Get();
    const uint64_t processRevision = ctx.processRevision.load(std::memory_order_acquire);
    if (processRevision != observedProcessRevision) {
        observedProcessRevision = processRevision;
        resetProcessState();
    }

    // 窗口聚焦时的键盘快捷键
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && !ImGui::GetIO().WantTextInput) {
        // F5: 刷新地址值
        if (ImGui::IsKeyPressed(ImGuiKey_F5) && ctx.hasProcess()) {
            refreshAddressValuesAsync();
        }
        // Enter/F9: 执行扫描（首次或再次）
        if (ImGui::IsKeyPressed(ImGuiKey_F9) && !scanInProgress && ctx.hasProcess()) {
            if (totalScanResults.load() == 0) {
                performFirstScanAsync();
            } else {
                performNextScanAsync();
            }
        }
    }

    if (ctx.hasProcess()) {
        const std::string processName = ctx.getSelectedName();
        ImGui::TextColored(ColorScheme::SuccessBright, "已附加: %s (PID %d)",
            processName.c_str(), ctx.selectedPid.load());
        ImGui::SameLine();
        if (ImGui::Button("刷新地址值")) {
            refreshAddressValuesAsync();
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
