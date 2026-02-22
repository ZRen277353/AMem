#include "BreakpointWindow.h"
#include "MemoryViewerWindow.h"
#include "DisassemblyHelper.h"
#include "Gui.h"
#include "AppContext.h"
#include "ColorScheme.h"
#include "../imgui/imgui.h"
#include "../socket/client_singleton.h"
#include "../socket/client.hpp"
#include <algorithm>
#include <cstring>
#include <cmath>


BreakpointWindow::BreakpointWindow()
{
    name = "断点调试器";
    
    // 初始化反汇编助手
    if (DisassemblyHelper::isCapstoneAvailable()) {
        disassemblyHelper = std::make_unique<DisassemblyHelper>();
        // 初始化为 ARM64 架构（根据需要可以改为其他架构）
        if (disassemblyHelper->initialize(DisassemblyHelper::Architecture::ARM64)) {
            disassemblyInitialized = true;
            Gui::log("反汇编引擎已初始化 (ARM64)");
        } else {
            Gui::log("警告: 反汇编引擎初始化失败 (ARM64)，断点反汇编功能不可用");
            disassemblyHelper.reset();
        }
    } else {
        Gui::log("警告: Capstone 库不可用，反汇编功能已禁用");
    }
}

unsigned int BreakpointWindow::getWindowFlags() const
{
    return ImGuiWindowFlags_NoDocking;
}


void BreakpointWindow::onDraw()
{
    if (!pOpen) return;

    // 自动刷新模块列表（仅当有断点存在时才刷新）
    if (AppContext::Get().hasProcess() && !breakpoints.empty()) {
        float currentTime = ImGui::GetTime();
        if (!AppContext::Get().moduleCache.valid || (currentTime - AppContext::Get().moduleCache.lastRefreshTime >= 5.0f)) {
            AppContext::Get().moduleCache.refresh();
        }
    } else {
        // 如果没有断点或未附加进程，清除模块列表有效性标记（但不立即清空列表，保留作为缓存）
        if (!AppContext::Get().hasProcess() || breakpoints.empty()) {
            AppContext::Get().moduleCache.invalidate();
        }
    }

    if (ImGui::Begin(name.c_str(), &pOpen, ImGuiWindowFlags_None))
    {
        if (AppContext::Get().hasProcess()) {
            ImGui::TextColored(ColorScheme::SuccessBright, "已附加: %s (PID %d)",
                AppContext::Get().selectedName.c_str(), AppContext::Get().selectedPid.load());
        } else {
            ImGui::TextDisabled("未附加进程");
        }
        ImGui::Separator();

        // 控制按钮
        drawBreakpointControls();
        
        ImGui::Separator();
        
        // 只显示断点列表，详情改为弹窗
        drawBreakpointList();
    }
    ImGui::End();
    
    // 添加断点对话框 - 必须在主窗口End之后绘制，否则Combo下拉菜单会被裁剪
    if (showAddBreakpointDialog) {
        drawAddBreakpointDialog();
    }
    
    // 绘制所有详情弹窗
    for (size_t i = 0; i < detailWindows.size(); i++) {
        if (detailWindows[i].isOpen) {
            drawBreakpointDetailWindow(detailWindows[i]);
        }
    }
    
    // 调试信息：显示当前打开的详情窗口数量
    static float lastLogTime = 0;
    float currentTime = ImGui::GetTime();
    if (currentTime - lastLogTime > 5.0f && !detailWindows.empty()) {  // 每5秒记录一次
        int openWindows = 0;
        for (const auto& window : detailWindows) {
            if (window.isOpen) openWindows++;
        }
        if (openWindows > 0) {
            Gui::log("调试: 当前有 %d 个详情窗口打开", openWindows);
        }
        lastLogTime = currentTime;
    }
    
    // 清理已关闭的弹窗
    detailWindows.erase(
        std::remove_if(detailWindows.begin(), detailWindows.end(),
            [](const BreakpointDetailWindow& window) { return !window.isOpen; }),
        detailWindows.end()
    );
}

void BreakpointWindow::refreshBreakpointHitInfo(int index)
{
    if (index < 0 || index >= (int)breakpoints.size()) {
        Gui::log("错误: refreshBreakpointHitInfo 无效索引 %d", index);
        return;
    }
    
    auto& bp = breakpoints[index];
    if (!bp.enabled) {
        Gui::log("断点 0x%llX 未启用，跳过刷新", bp.address);
        return;
    }
    
    std::vector<HW_HIT_INFO> hitInfos;
    // 使用调试端口进行自动刷新，避免阻塞主端口
    if (ReadKernelBreakpointInfo(bp.address, hitInfos, PORT_DEBUG)) {
        int newCount = (int)hitInfos.size();
        
        // 如果没有新数据，直接返回
        if (newCount == 0) {
            return;
        }
        
        Gui::log("断点 0x%llX 获取到 %d 条新命中记录", bp.address, newCount);
        
        // 直接追加所有新记录（假设每次读取都是新数据）
        bp.hitHistory.insert(bp.hitHistory.end(), hitInfos.begin(), hitInfos.end());
        
        // 限制历史记录最大数量，防止内存溢出
        const int MAX_HISTORY_SIZE = 50000; // 最多保留5万条记录
        if ((int)bp.hitHistory.size() > MAX_HISTORY_SIZE) {
            // 删除最旧的记录，保留最新的
            int removeCount = (int)bp.hitHistory.size() - MAX_HISTORY_SIZE;
            bp.hitHistory.erase(bp.hitHistory.begin(), bp.hitHistory.begin() + removeCount);
            Gui::log("断点 0x%llX 历史记录已达上限，移除了 %d 条最旧记录", bp.address, removeCount);
        }
        
        // 增量更新PC统计信息（只处理新增的记录）
        for (const auto& hit : hitInfos) {
            uint64_t pc = hit.regs_info.pc;
            auto& stat = bp.pcHitStats[pc];
            
            if (stat.hit_count == 0) {
                stat.pc_address = pc;
                stat.first_hit_time = hit.hit_time;
            }
            
            stat.hit_count++;
            stat.last_hit_time = hit.hit_time;
        }
        
        // 更新总命中次数和版本号
        bp.hitCount = (int)bp.hitHistory.size();
        bp.dataVersion++;
        
        // 通知所有相关的详情窗口需要刷新缓存
        markDetailWindowsForRefresh(index);
        
        Gui::log("断点 0x%llX 命中信息已更新: +%d条新记录, 总计%d条 (版本: %d)", 
                bp.address, newCount, bp.hitCount, bp.dataVersion);
    } else {
        Gui::log("获取断点命中信息失败: 0x%llX", bp.address);
    }
}

void BreakpointWindow::updatePCHitStatistics(int index)
{
    if (index < 0 || index >= (int)breakpoints.size()) return;
    
    auto& bp = breakpoints[index];
    bp.pcHitStats.clear();
    
    for (const auto& hit : bp.hitHistory) {
        uint64_t pc = hit.regs_info.pc;
        auto& stat = bp.pcHitStats[pc];
        
        if (stat.hit_count == 0) {
            stat.pc_address = pc;
            stat.first_hit_time = hit.hit_time;
        }
        
        stat.hit_count++;
        stat.last_hit_time = hit.hit_time;
    }
}

void BreakpointWindow::markDetailWindowsForRefresh(int breakpointIndex)
{
    // 标记所有相关的详情窗口需要刷新缓存
    for (auto& window : detailWindows) {
        if (window.breakpointIndex == breakpointIndex && window.isOpen) {
            window.needsStatRefresh = true;
            window.needsHitRefresh = true;
            // 清空缓存，强制重新构建
            window.cachedSortedStats.clear();
            window.lastFilterStr = "";
            Gui::log("标记断点 %d 的详情窗口需要刷新", breakpointIndex);
        }
    }
}

void BreakpointWindow::refreshAllDetailWindows()
{
    // 刷新所有打开的详情窗口
    for (auto& window : detailWindows) {
        if (window.isOpen) {
            window.needsStatRefresh = true;
            window.needsHitRefresh = true;
            window.cachedSortedStats.clear();
            window.lastFilterStr = "";
        }
    }
    if (!detailWindows.empty()) {
        Gui::log("已标记所有详情窗口需要刷新");
    }
}
