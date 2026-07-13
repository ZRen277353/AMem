#include "BreakpointWindow.h"
#include "MemoryViewerWindow.h"
#include "DisassemblyHelper.h"
#include "Gui.h"
#include "AppContext.h"
#include "ColorScheme.h"
#include "../imgui/imgui.h"
#include "../mem/IMemService.h"
#include <algorithm>
#include <cstring>
#include <cmath>


BreakpointWindow::BreakpointWindow(Mem::IMemService& memService)
    : memService_(memService)
{
    name = "断点调试器";
    observedProcessRevision = AppContext::Get().processRevision.load(std::memory_order_acquire);
    
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

bool BreakpointWindow::readTargetMemory(
    uint64_t address, uint32_t size,
    std::vector<unsigned char>& bytes,
    Mem::MemoryReadChannel channel,
    Mem::Error* error) {
    Mem::MemoryReadRequest request;
    request.address = address;
    request.size = size;
    request.channel = channel;
    auto response = memService_.readMemory(
        memService_.captureContext(true), request);
    if (!response.ok()) {
        bytes.clear();
        if (error) *error = response.error();
        return false;
    }
    bytes = std::move(response.value().bytes);
    return true;
}

unsigned int BreakpointWindow::getWindowFlags() const
{
    return ImGuiWindowFlags_NoDocking;
}

void BreakpointWindow::resetProcessState()
{
    breakpoints.clear();
    detailWindows.clear();
    showAddBreakpointDialog = false;
    memset(newBreakpointAddress, 0, sizeof(newBreakpointAddress));
    memset(newBreakpointDescription, 0, sizeof(newBreakpointDescription));
    newBreakpointType = 0;
    newBreakpointSize = 0;
}

void BreakpointWindow::draw()
{
    if (!pOpen) return;

    const uint64_t processRevision = AppContext::Get().processRevision.load(std::memory_order_acquire);
    if (processRevision != observedProcessRevision) {
        observedProcessRevision = processRevision;
        resetProcessState();
    }

    if (shouldBringToFront) {
        ImGui::SetNextWindowFocus();
        shouldBringToFront = false;
    }

    // 自动刷新模块列表（仅当有断点存在时才刷新）
    if (AppContext::Get().hasProcess() && !breakpoints.empty()) {
        float currentTime = ImGui::GetTime();
        if (!AppContext::Get().moduleCache.valid || (currentTime - AppContext::Get().moduleCache.lastRefreshTime >= 5.0f)) {
            AppContext::Get().moduleCache.refresh(memService_);
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
            const std::string processName = AppContext::Get().getSelectedName();
            ImGui::TextColored(ColorScheme::SuccessBright, "已附加: %s (PID %d)",
                processName.c_str(), AppContext::Get().selectedPid.load());
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
    
    Mem::BreakpointHitBatchRequest request;
    request.address = bp.address;
    request.limit = Mem::kMaxBreakpointHitBatchSize;
    auto response = memService_.breakpointHitBatch(
        memService_.captureContext(true), request);
    if (response.ok()) {
        const size_t dropped = response.value().dropped;
        auto hitInfos = std::move(response.value().items);
        int newCount = (int)hitInfos.size();
        
        // 如果没有新数据，直接返回
        if (newCount == 0) {
            return;
        }
        
        Gui::log("断点 0x%llX 获取到 %d 条新命中记录", bp.address, newCount);
        
        // 直接追加所有新记录（假设每次读取都是新数据）
        bp.hitHistory.insert(bp.hitHistory.end(), hitInfos.begin(), hitInfos.end());
        bool historyTrimmed = false;
        
        // 限制历史记录最大数量，防止内存溢出
        const int MAX_HISTORY_SIZE =
            static_cast<int>(Mem::kMaxBreakpointHitBatchSize);
        if ((int)bp.hitHistory.size() > MAX_HISTORY_SIZE) {
            // 删除最旧的记录，保留最新的
            int removeCount = (int)bp.hitHistory.size() - MAX_HISTORY_SIZE;
            bp.hitHistory.erase(bp.hitHistory.begin(), bp.hitHistory.begin() + removeCount);
            historyTrimmed = true;
            Gui::log("断点 0x%llX 历史记录已达上限，移除了 %d 条最旧记录", bp.address, removeCount);
        }
        
        if (historyTrimmed) {
            updatePCHitStatistics(index);
            for (auto& window : detailWindows) {
                if (window.breakpointIndex == index) {
                    window.selectedHitIndex = -1;
                }
            }
        } else {
            // 增量更新PC统计信息（只处理新增的记录）
            for (const auto& hit : hitInfos) {
                uint64_t pc = hit.programCounter;
                auto& stat = bp.pcHitStats[pc];

                if (stat.hit_count == 0) {
                    stat.pc_address = pc;
                    stat.first_hit_time = hit.hitTime;
                }

                stat.hit_count++;
                stat.last_hit_time = hit.hitTime;
            }
        }
        
        // 更新总命中次数和版本号
        bp.hitCount = (int)bp.hitHistory.size();
        bp.dataVersion++;
        
        // 通知所有相关的详情窗口需要刷新缓存
        markDetailWindowsForRefresh(index);

        if (dropped > 0) {
            Gui::log("断点 0x%llX 当前批次超过上限，已丢弃 %llu 条较早命中",
                     bp.address,
                     static_cast<unsigned long long>(dropped));
        }
        
        Gui::log("断点 0x%llX 命中信息已更新: +%d条新记录, 总计%d条 (版本: %d)", 
                bp.address, newCount, bp.hitCount, bp.dataVersion);
    } else {
        Gui::log("获取断点命中信息失败: 0x%llX [%s] %s",
                 bp.address,
                 Mem::errorCodeName(response.error().code),
                 response.error().message.c_str());
    }
}

void BreakpointWindow::updatePCHitStatistics(int index)
{
    if (index < 0 || index >= (int)breakpoints.size()) return;
    
    auto& bp = breakpoints[index];
    bp.pcHitStats.clear();
    
    for (const auto& hit : bp.hitHistory) {
        uint64_t pc = hit.programCounter;
        auto& stat = bp.pcHitStats[pc];
        
        if (stat.hit_count == 0) {
            stat.pc_address = pc;
            stat.first_hit_time = hit.hitTime;
        }
        
        stat.hit_count++;
        stat.last_hit_time = hit.hitTime;
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
