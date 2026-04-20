#include "../BreakpointWindow.h"
#include "../DisassemblyHelper.h"
#include "../Gui.h"
#include "../AppContext.h"
#include "../ColorScheme.h"
#include "../../imgui/imgui.h"
#include "../../socket/client_singleton.h"
#include "../../socket/client.hpp"
#include <algorithm>
#include <cstring>
#include <cmath>
#include <memory>

void BreakpointWindow::openBreakpointDetailWindow(int breakpointIndex)
{
    if (breakpointIndex < 0 || breakpointIndex >= (int)breakpoints.size()) {
        Gui::log("错误: 无效的断点索引 %d (范围: 0-%d)", breakpointIndex, (int)breakpoints.size() - 1);
        return;
    }
    
    // 检查是否已经有这个断点的详情窗口打开
    for (auto& window : detailWindows) {
        if (window.breakpointIndex == breakpointIndex && window.isOpen) {
            // 如果已经打开，直接返回（或者可以选择聚焦到该窗口）
            Gui::log("断点 %d 的详情窗口已经打开，不重复创建", breakpointIndex);
            return;
        }
    }
    
    // 创建新的详情窗口
    auto& bp = breakpoints[breakpointIndex];
    char title[256];
    snprintf(title, sizeof(title), "断点详情 - 0x%llX##BP%d", bp.address, breakpointIndex);  // 添加##确保ID唯一
    
    Gui::log("创建详情窗口: 索引=%d, 地址=0x%llX, 标题=%s", breakpointIndex, bp.address, title);
    
    detailWindows.emplace_back(breakpointIndex, title);
    
    // 设置需要刷新的标志，而不是立即刷新
    auto& newWindow = detailWindows.back();
    newWindow.isOpen = true;  // 明确设置为打开状态
    newWindow.needsStatRefresh = true;
    newWindow.needsHitRefresh = true;
    
    Gui::log("详情窗口创建成功: 当前共有 %d 个窗口, isOpen=%d", (int)detailWindows.size(), newWindow.isOpen ? 1 : 0);
}

void BreakpointWindow::drawBreakpointDetailWindow(BreakpointDetailWindow& detailWindow)
{
    if (detailWindow.breakpointIndex < 0 || detailWindow.breakpointIndex >= (int)breakpoints.size()) {
        Gui::log("警告: 详情窗口的断点索引无效 %d (断点数量: %d)，关闭窗口", 
                detailWindow.breakpointIndex, (int)breakpoints.size());
        detailWindow.isOpen = false;
        return;
    }
    
    auto& bp = breakpoints[detailWindow.breakpointIndex];
    
    // 设置窗口大小和位置（每个窗口稍微偏移，避免完全重叠）
    static int windowOffset = 0;
    ImGui::SetNextWindowSize(ImVec2(900, 700), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(150 + windowOffset * 30, 120 + windowOffset * 30), ImGuiCond_FirstUseEver);
    windowOffset = (windowOffset + 1) % 10;  // 最多偏移10次后循环
    
    // 确保窗口标题是唯一的
    std::string windowTitle = detailWindow.windowTitle;
    
    // 尝试开始绘制窗口
    bool windowVisible = ImGui::Begin(windowTitle.c_str(), &detailWindow.isOpen, ImGuiWindowFlags_None);
    
    if (!windowVisible) {
        // 窗口被折叠或不可见，但仍然需要调用End
        ImGui::End();
        return;
    }
    
    // 调试信息
    ImGui::Text("窗口状态: 索引=%d, 地址=0x%llX, 命中=%d条", 
               detailWindow.breakpointIndex, bp.address, bp.hitCount);
    ImGui::Separator();
    
    // 断点基本信息 - 使用表格布局
    if (ImGui::BeginTable("BasicInfo", 4, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("地址", ImGuiTableColumnFlags_WidthFixed, 150);
        ImGui::TableSetupColumn("状态", ImGuiTableColumnFlags_WidthFixed, 100);
        ImGui::TableSetupColumn("命中", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("描述", ImGuiTableColumnFlags_WidthStretch);
        
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextColored(ColorScheme::AddressBright, "0x%llX", bp.address);
        ImGui::Text("(%s, %s)", getBreakpointTypeName(bp.type), getBreakpointSizeName(bp.size));
        
        ImGui::TableSetColumnIndex(1);
        if (!bp.enabled) {
            ImGui::TextColored(ColorScheme::TextSecondary, "禁用");
        } else if (bp.suspended) {
            ImGui::TextColored(ColorScheme::Warning, "暂停");
        } else {
            ImGui::TextColored(ColorScheme::Success, "活动");
        }
        
        ImGui::TableSetColumnIndex(2);
        ImGui::Text("%d", bp.hitCount);
        if (bp.hitCount > 0 && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("总命中次数: %d", bp.hitCount);
        }
        
        ImGui::TableSetColumnIndex(3);
        ImGui::TextWrapped("%s", bp.description.empty() ? "无描述" : bp.description.c_str());
        
        ImGui::EndTable();
    }
    
    ImGui::Separator();
    
    // 控制面板
    if (ImGui::BeginTable("ControlPanel", 2, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("控制按钮", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("显示选项", ImGuiTableColumnFlags_WidthStretch);
        
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        
        if (ImGui::Button("刷新命中信息")) {
            refreshBreakpointHitInfo(detailWindow.breakpointIndex);
            detailWindow.needsStatRefresh = true;
            detailWindow.needsHitRefresh = true;
        }
        ImGui::SameLine();
                    if (ImGui::Button("清除命中历史")) {
                bp.hitHistory.clear();
                bp.pcHitStats.clear();
                bp.hitCount = 0;
                bp.dataVersion++;  // 增加数据版本号
                // 通知所有相关的详情窗口需要刷新
                markDetailWindowsForRefresh(detailWindow.breakpointIndex);
                Gui::log("已清除断点 0x%llX 的命中历史 (版本: %d)", bp.address, bp.dataVersion);
            }
        
        ImGui::TableSetColumnIndex(1);
        ImGui::Checkbox("PC统计", &detailWindow.showPCStatistics);
        ImGui::SameLine();
        ImGui::Checkbox("详细历史", &detailWindow.showHitHistoryDetails);
        ImGui::Checkbox("寄存器信息", &detailWindow.showRegisterInfo);
        ImGui::SameLine();
        ImGui::Checkbox("紧凑模式", &detailWindow.compactMode);
        
        ImGui::EndTable();
    }
    
    ImGui::Separator();
    
    // 使用标签页显示不同的信息
    if (ImGui::BeginTabBar("BreakpointDetailsTab")) {
        bool hasVisibleTab = false;
        
        if (detailWindow.showPCStatistics && ImGui::BeginTabItem("PC统计")) {
            hasVisibleTab = true;
            drawPCHitStatisticsInWindow(detailWindow);
            ImGui::EndTabItem();
        }
        
        if (detailWindow.showHitHistoryDetails && ImGui::BeginTabItem("命中历史")) {
            hasVisibleTab = true;
            drawDetailedHitInfoInWindow(detailWindow);
            ImGui::EndTabItem();
        }
        
        // 如果没有可见的标签页，显示一个默认的信息标签页
        if (!hasVisibleTab) {
            if (ImGui::BeginTabItem("信息")) {
                ImGui::Text("请在上方选择要显示的信息类型：");
                ImGui::Bullet(); ImGui::Text("PC统计 - 显示不同PC地址的命中统计");
                ImGui::Bullet(); ImGui::Text("详细历史 - 显示完整的命中记录和寄存器信息");
                
                if (ImGui::Button("启用PC统计")) {
                    detailWindow.showPCStatistics = true;
                }
                ImGui::SameLine();
                if (ImGui::Button("启用详细历史")) {
                    detailWindow.showHitHistoryDetails = true;
                }
                
                ImGui::EndTabItem();
            }
        }
        
        ImGui::EndTabBar();
    }
    
    // 自动刷新逻辑
    if (autoRefreshHitInfo && bp.enabled && !bp.suspended) {
        float currentTime = ImGui::GetTime();
        if (currentTime - lastRefreshTime >= refreshInterval) {
            refreshBreakpointHitInfo(detailWindow.breakpointIndex);
            lastRefreshTime = currentTime;
        }
    }
    ImGui::End();
}

void BreakpointWindow::drawPCHitStatisticsInWindow(BreakpointDetailWindow& detailWindow)
{
    if (detailWindow.breakpointIndex < 0 || detailWindow.breakpointIndex >= (int)breakpoints.size()) {
        return;
    }
    
    auto& bp = breakpoints[detailWindow.breakpointIndex];
    
    // 扩展的统计信息摘要
    if (ImGui::BeginTable("StatsSummary", 5, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Borders))
    {
        ImGui::TableSetupColumn("不同PC", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("总命中", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("热点PC", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("平均命中", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("命中分布", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextColored(ColorScheme::StatCount, "%d个", (int)bp.pcHitStats.size());
        
        ImGui::TableSetColumnIndex(1);
        ImGui::TextColored(ColorScheme::StatHighlight, "%d次", bp.hitCount);
        
        ImGui::TableSetColumnIndex(2);
        if (!bp.pcHitStats.empty()) {
            auto maxHit = std::max_element(bp.pcHitStats.begin(), bp.pcHitStats.end(),
                [](const auto& a, const auto& b) { return a.second.hit_count < b.second.hit_count; });
            float maxHitRate = bp.hitCount > 0 ? (float)maxHit->second.hit_count / bp.hitCount * 100.0f : 0.0f;
            std::string maxHitAddrStr = AppContext::Get().moduleCache.formatWithModule(maxHit->first);
            ImGui::TextColored(ColorScheme::ErrorLight, "%s", maxHitAddrStr.c_str());
            ImGui::Text("(%d次, %.1f%%)", maxHit->second.hit_count, maxHitRate);
        } else {
            ImGui::TextDisabled("无");
        }
        
        ImGui::TableSetColumnIndex(3);
        if (!bp.pcHitStats.empty()) {
            float avgHits = (float)bp.hitCount / bp.pcHitStats.size();
            ImGui::TextColored(ColorScheme::StatAverage, "%.1f次", avgHits);
        } else {
            ImGui::TextDisabled("0次");
        }
        
        ImGui::TableSetColumnIndex(4);
        if (!bp.pcHitStats.empty()) {
            // 计算命中分布统计
            int highFreq = 0, medFreq = 0, lowFreq = 0;
            float avgHits = (float)bp.hitCount / bp.pcHitStats.size();
            for (const auto& stat : bp.pcHitStats) {
                if (stat.second.hit_count > avgHits * 2) {
                    highFreq++;
                } else if (stat.second.hit_count > avgHits * 0.5) {
                    medFreq++;
                } else {
                    lowFreq++;
                }
            }
            ImGui::Text("高频:%d 中频:%d 低频:%d", highFreq, medFreq, lowFreq);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("高频: >%.1f次\n中频: %.1f-%.1f次\n低频: <%.1f次", 
                    avgHits * 2, avgHits * 0.5, avgHits * 2, avgHits * 0.5);
            }
        } else {
            ImGui::TextDisabled("无数据");
        }
        
        ImGui::EndTable();
    }
    
    ImGui::Separator();
    
    // 搜索过滤器
    ImGui::AlignTextToFramePadding();
    ImGui::Text("过滤:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200);
    ImGui::InputText("##PCFilter", detailWindow.pcFilterBuffer, sizeof(detailWindow.pcFilterBuffer), ImGuiInputTextFlags_CharsHexadecimal);
    ImGui::SameLine();
    if (ImGui::Button("清除")) {
        detailWindow.pcFilterBuffer[0] = '\0';
        detailWindow.needsStatRefresh = true;
    }
    
    // 检查是否需要刷新缓存
    std::string currentFilter = detailWindow.pcFilterBuffer;
    bool dataChanged = (detailWindow.cachedSortedStats.size() != bp.pcHitStats.size()) || 
                      (detailWindow.lastDataVersion != bp.dataVersion);
    bool sortChanged = (detailWindow.lastSortBy != detailWindow.sortBy) || 
                      (detailWindow.lastSortDescending != detailWindow.sortDescending);
    
    // 数据量过大警告
    if (bp.pcHitStats.size() > 10000) {
        ImGui::TextColored(ColorScheme::Warning, 
                          "警告: PC统计数据较多 (%d个)，可能影响性能", (int)bp.pcHitStats.size());
    }
    
    if (detailWindow.needsStatRefresh || currentFilter != detailWindow.lastFilterStr || dataChanged || sortChanged) {
        detailWindow.cachedSortedStats.clear();
        detailWindow.cachedSortedStats.reserve(bp.pcHitStats.size()); // 预分配内存
        
        try {
            for (auto& pair : bp.pcHitStats) {
                detailWindow.cachedSortedStats.push_back({pair.first, &pair.second});
            }
            
            // 根据选择的排序方式进行排序
            std::sort(detailWindow.cachedSortedStats.begin(), detailWindow.cachedSortedStats.end(), 
            [&detailWindow, &bp](const auto& a, const auto& b) {
                switch (detailWindow.sortBy) {
                    case 0: // 命中次数
                        if (detailWindow.sortDescending)
                            return a.second->hit_count > b.second->hit_count;
                        else
                            return a.second->hit_count < b.second->hit_count;
                    case 1: // PC地址
                        if (detailWindow.sortDescending)
                            return a.first > b.first;
                        else
                            return a.first < b.first;
                    case 2: // 首次命中时间
                        if (detailWindow.sortDescending)
                            return a.second->first_hit_time > b.second->first_hit_time;
                        else
                            return a.second->first_hit_time < b.second->first_hit_time;
                    case 3: // 最后命中时间
                        if (detailWindow.sortDescending)
                            return a.second->last_hit_time > b.second->last_hit_time;
                        else
                            return a.second->last_hit_time < b.second->last_hit_time;
                    default:
                        return false; // 默认情况，保持原顺序
                }
            });
            
            detailWindow.lastFilterStr = currentFilter;
            detailWindow.needsStatRefresh = false;
            detailWindow.lastDataVersion = bp.dataVersion;  // 更新版本号
            detailWindow.lastSortBy = detailWindow.sortBy;
            detailWindow.lastSortDescending = detailWindow.sortDescending;
            
            if (dataChanged) {
                Gui::log("检测到PC统计数据变化，已刷新缓存 (版本: %d)", bp.dataVersion);
            }
        } catch (const std::exception& e) {
            Gui::log("错误: 刷新PC统计缓存失败: %s", e.what());
            detailWindow.cachedSortedStats.clear();
        }
    }
    
    // 排序选项
    ImGui::AlignTextToFramePadding();
    ImGui::Text("排序方式:");
    ImGui::SameLine();
    const char* sortOptions[] = { "命中次数", "PC地址", "首次命中时间", "最后命中时间" };
    ImGui::SetNextItemWidth(120);
    ImGui::Combo("##SortBy", &detailWindow.sortBy, sortOptions, 4);
    ImGui::SameLine();
    ImGui::Checkbox("降序", &detailWindow.sortDescending);
    
    ImGui::Separator();
    
    // 左右分栏布局
    if (ImGui::BeginTable("PCStatLayout", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV))
    {
        ImGui::TableSetupColumn("PC统计列表", ImGuiTableColumnFlags_WidthStretch, 0.6f);
        ImGui::TableSetupColumn("寄存器信息", ImGuiTableColumnFlags_WidthStretch, 0.4f);
        ImGui::TableHeadersRow();
        
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        
        // 左侧：PC统计表格
        if (ImGui::BeginChild("PCStatList", ImVec2(0, 0), true))
        {
            if (ImGui::BeginTable("PCStatTable", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY))
            {
                ImGui::TableSetupColumn("排名", ImGuiTableColumnFlags_WidthFixed, 50);
                ImGui::TableSetupColumn("PC地址", ImGuiTableColumnFlags_WidthFixed, 120);
                ImGui::TableSetupColumn("命中次数", ImGuiTableColumnFlags_WidthFixed, 80);
                ImGui::TableSetupColumn("首次命中", ImGuiTableColumnFlags_WidthFixed, 100);
                ImGui::TableSetupColumn("最后命中", ImGuiTableColumnFlags_WidthFixed, 100);
                ImGui::TableSetupColumn("时间间隔", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();
                
                // 应用过滤器
                std::string filterStr = currentFilter;
                std::transform(filterStr.begin(), filterStr.end(), filterStr.begin(), ::tolower);
                
                int rank = 0;
                int displayCount = 0;
                const int MAX_DISPLAY_ROWS = 5000; // 最多显示5000行，防止UI卡死
                
                for (const auto& pair : detailWindow.cachedSortedStats) {
                    // 限制显示数量，防止界面卡顿
                    if (displayCount >= MAX_DISPLAY_ROWS) {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextColored(ColorScheme::Warning, "...");
                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextColored(ColorScheme::Warning, "数据过多，仅显示前%d条", MAX_DISPLAY_ROWS);
                        break;
                    }
                    
                    const auto& stat = *pair.second;
                    
                    // 过滤逻辑
                    if (!filterStr.empty()) {
                        char addrStr[32];
                        snprintf(addrStr, sizeof(addrStr), "%llx", stat.pc_address);
                        std::string addrStrLower = addrStr;
                        std::transform(addrStrLower.begin(), addrStrLower.end(), addrStrLower.begin(), ::tolower);
                        
                        if (addrStrLower.find(filterStr) == std::string::npos) {
                            continue;  // 跳过不匹配的条目
                        }
                    }
                    
                    rank++;
                    displayCount++;
                    
                    bool isSelected = (detailWindow.selectedPCAddress == stat.pc_address);
                    ImGui::TableNextRow();
                    
                    ImGui::TableSetColumnIndex(0);
                    // 显示排名，前三名使用特殊颜色
                    if (rank == 1) {
                        ImGui::TextColored(ColorScheme::RankGold, "#%d", rank);
                    } else if (rank == 2) {
                        ImGui::TextColored(ColorScheme::RankSilver, "#%d", rank);
                    } else if (rank == 3) {
                        ImGui::TextColored(ColorScheme::RankBronze, "#%d", rank);
                    } else {
                        ImGui::Text("#%d", rank);
                    }
                    
                    ImGui::TableSetColumnIndex(1);
                    std::string pcAddrDisplayStr = AppContext::Get().moduleCache.formatAddressWithModuleAndSymbol(stat.pc_address);
                    char pcAddrStr[512];
                    snprintf(pcAddrStr, sizeof(pcAddrStr), "%s", pcAddrDisplayStr.c_str());
                    if (ImGui::Selectable(pcAddrStr, isSelected, ImGuiSelectableFlags_SpanAllColumns)) {
                        detailWindow.selectedPCAddress = stat.pc_address;
                        Gui::log("选中PC地址: 0x%llX", stat.pc_address);
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("PC地址: 0x%llX", stat.pc_address);
                    }
                    
                    // 右键菜单 - 使用唯一ID避免断言失败
                    char pc_popup_id[64];
                    snprintf(pc_popup_id, sizeof(pc_popup_id), "PCPopup_%llX", stat.pc_address);
                    if (ImGui::BeginPopupContextItem(pc_popup_id)) {
                        char addrHexStr[32];
                        snprintf(addrHexStr, sizeof(addrHexStr), "0x%llX", stat.pc_address);
                        if (ImGui::MenuItem("复制地址")) {
                            ImGui::SetClipboardText(addrHexStr);
                        }
                        if (ImGui::MenuItem("在内存查看器中打开")) {
                            navigateToAddress(stat.pc_address);
                            Gui::log("跳转到内存地址: 0x%llX", stat.pc_address);
                        }
                        ImGui::EndPopup();
                    }
                    
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%d", stat.hit_count);
                    
                    ImGui::TableSetColumnIndex(3);
                    std::string firstTime = formatTime(stat.first_hit_time);
                    ImGui::Text("%s", firstTime.c_str());
                    
                    ImGui::TableSetColumnIndex(4);
                    std::string lastTime = formatTime(stat.last_hit_time);
                    ImGui::Text("%s", lastTime.c_str());
                    
                    ImGui::TableSetColumnIndex(5);
                    std::string timeDiff = formatTimeDiff(stat.first_hit_time, stat.last_hit_time);
                    ImGui::Text("%s", timeDiff.c_str());
                    if (timeDiff != "-" && ImGui::IsItemHovered()) {
                        uint64_t interval = stat.last_hit_time - stat.first_hit_time;
                        ImGui::SetTooltip("精确时间间隔: %llu 个时间单位", interval);
                    }
                }
                
                ImGui::EndTable();
            }
        }
        ImGui::EndChild();
        
        // 右侧：寄存器信息和反汇编
        ImGui::TableSetColumnIndex(1);
        if (ImGui::BeginChild("RegisterInfo", ImVec2(0, 0), true))
        {
            if (detailWindow.selectedPCAddress != 0) {
                // 查找该PC地址的最新命中记录
                bool foundHit = false;
                for (int i = (int)bp.hitHistory.size() - 1; i >= 0; i--) {
                    if (bp.hitHistory[i].regs_info.pc == detailWindow.selectedPCAddress) {
                        std::string pcAddrDisplayStr = AppContext::Get().moduleCache.formatAddressWithModuleAndSymbol(detailWindow.selectedPCAddress);
                        ImGui::Text("PC地址: %s 的详细信息", pcAddrDisplayStr.c_str());
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("原始地址: 0x%llX", detailWindow.selectedPCAddress);
                        }
                        ImGui::Separator();
                        
                        // 使用标签页显示寄存器和反汇编
                        if (ImGui::BeginTabBar("PCDetailTab")) {
                            if (ImGui::BeginTabItem("寄存器")) {
                                drawRegisterInfoInWindow(bp.hitHistory[i].regs_info, bp.hitHistory[i].fpsimd_info, detailWindow);
                                ImGui::EndTabItem();
                            }
                            
                            if (detailWindow.showDisassembly && ImGui::BeginTabItem("反汇编")) {
                                if (disassemblyInitialized && disassemblyHelper) {
                                    std::string pcAddrDisplayStr = AppContext::Get().moduleCache.formatAddressWithModuleAndSymbol(detailWindow.selectedPCAddress);
                                    ImGui::TextColored(ColorScheme::SuccessLight, "PC: %s 的反汇编", pcAddrDisplayStr.c_str());
                                    ImGui::Separator();
                                    
                                    // 控制选项
                                    static int beforeCount = 4;
                                    static int afterCount = 4;
                                    ImGui::Text("显示范围:");
                                    ImGui::SameLine();
                                    ImGui::SetNextItemWidth(80);
                                    ImGui::InputInt("##BeforeCount", &beforeCount, 1, 10);
                                    ImGui::SameLine();
                                    ImGui::Text("条指令前");
                                    ImGui::SameLine();
                                    ImGui::SetNextItemWidth(80);
                                    ImGui::InputInt("##AfterCount", &afterCount, 1, 10);
                                    ImGui::SameLine();
                                    ImGui::Text("条指令后");
                                    
                                    // 限制范围
                                    if (beforeCount < 0) beforeCount = 0;
                                    if (beforeCount > 20) beforeCount = 20;
                                    if (afterCount < 0) afterCount = 0;
                                    if (afterCount > 20) afterCount = 20;
                                    
                                    ImGui::Separator();
                                    
                                    // 调用新的方法读取并显示PC周围的指令
                                    drawDisassemblyForPC(detailWindow.selectedPCAddress, beforeCount, afterCount, detailWindow);
                                } else {
                                    ImGui::TextColored(ColorScheme::Warning, "反汇编引擎未初始化");
                                    ImGui::TextWrapped("请安装 Capstone 库以启用反汇编功能");
                                }
                                ImGui::EndTabItem();
                            }
                            
                            ImGui::EndTabBar();
                        }
                        
                        foundHit = true;
                        break;
                    }
                }
                
                if (!foundHit) {
                    ImGui::TextDisabled("未找到该PC地址的命中记录");
                }
            } else {
                ImGui::TextDisabled("请在左侧列表中选择一个PC地址");
                ImGui::TextWrapped("选中后将在此处显示该PC地址的寄存器状态和反汇编信息");
            }
        }
        ImGui::EndChild();
        
        ImGui::EndTable();
    }
}

void BreakpointWindow::drawDetailedHitInfoInWindow(BreakpointDetailWindow& detailWindow)
{
    if (detailWindow.breakpointIndex < 0 || detailWindow.breakpointIndex >= (int)breakpoints.size()) {
        return;
    }
    
    auto& bp = breakpoints[detailWindow.breakpointIndex];
    
    // 命中历史摘要
    if (ImGui::BeginTable("HitSummary", 3, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("总记录", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("显示限制", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("最新命中", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("%d条", (int)bp.hitHistory.size());
        
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(80);
        ImGui::InputInt("##MaxHits", &detailWindow.maxDisplayedHits, 100, 500);
        if (detailWindow.maxDisplayedHits < 10) detailWindow.maxDisplayedHits = 10;
        if (detailWindow.maxDisplayedHits > 10000) detailWindow.maxDisplayedHits = 10000;
        
        ImGui::TableSetColumnIndex(2);
        if (!bp.hitHistory.empty()) {
            const auto& lastHit = bp.hitHistory.back();
            ImGui::Text("时间: %llu", lastHit.hit_time);
        } else {
            ImGui::Text("无");
        }
        
        ImGui::EndTable();
    }
    
    ImGui::Separator();
    
    // 确定显示范围
    int totalHits = (int)bp.hitHistory.size();
    int displayCount = (totalHits < detailWindow.maxDisplayedHits) ? totalHits : detailWindow.maxDisplayedHits;
    int startIndex = (totalHits - displayCount > 0) ? (totalHits - displayCount) : 0;
    
    if (totalHits > detailWindow.maxDisplayedHits) {
        ImGui::TextColored(ColorScheme::WarningLight, 
                          "注意: 只显示最新的 %d 条记录 (共 %d 条)", displayCount, totalHits);
    }
    
    // 命中记录列表
    float childHeight = detailWindow.compactMode ? 150 : 200;
    if (ImGui::BeginChild("HitList", ImVec2(0, childHeight), true)) {
        if (ImGui::BeginTable("DetailedHitTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY))
        {
            ImGui::TableSetupColumn("序号", ImGuiTableColumnFlags_WidthFixed, 50);
            ImGui::TableSetupColumn("命中地址", ImGuiTableColumnFlags_WidthFixed, 120);
            ImGui::TableSetupColumn("PC地址", ImGuiTableColumnFlags_WidthFixed, 120);
            ImGui::TableSetupColumn("命中时间", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            
            for (int i = startIndex; i < startIndex + displayCount; i++)
            {
                const auto& hit = bp.hitHistory[i];
                ImGui::TableNextRow();
                
                bool isSelected = (detailWindow.selectedHitIndex == i);
                
                ImGui::TableSetColumnIndex(0);
                char indexStr[16];
                snprintf(indexStr, sizeof(indexStr), "%d", i + 1);
                if (ImGui::Selectable(indexStr, isSelected, ImGuiSelectableFlags_SpanAllColumns)) {
                    detailWindow.selectedHitIndex = i;
                }
                
                ImGui::TableSetColumnIndex(1);
                std::string hitAddrStr = AppContext::Get().moduleCache.formatWithModule(hit.hit_addr);
                ImGui::Text("%s", hitAddrStr.c_str());
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("命中地址: 0x%llX", hit.hit_addr);
                }
                
                ImGui::TableSetColumnIndex(2);
                // std::string pcAddrStr = AppContext::Get().moduleCache.formatWithModule(hit.regs_info.pc);
                // char pcStr[256];
                // snprintf(pcStr, sizeof(pcStr), "%s", pcAddrStr.c_str());
                char pcStr[32];
                snprintf(pcStr, sizeof(pcStr), "0x%llX", hit.regs_info.pc);
                if (ImGui::Selectable(pcStr, false, ImGuiSelectableFlags_None)) {
                    navigateToAddress(hit.regs_info.pc);
                    Gui::log("跳转到PC地址: 0x%llX", hit.regs_info.pc);
                }
                
                ImGui::TableSetColumnIndex(3);
                std::string hitTime = formatTime(hit.hit_time);
                ImGui::Text("%s", hitTime.c_str());
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("精确时间戳: %llu", hit.hit_time);
                }
            }
            
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();
    
    // 显示选中记录的寄存器信息
    if (detailWindow.showRegisterInfo && detailWindow.selectedHitIndex >= 0 && detailWindow.selectedHitIndex < (int)bp.hitHistory.size()) {
        ImGui::Separator();
        
        // 寄存器信息标题
        if (ImGui::BeginTable("RegInfoHeader", 2, ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("标题", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("操作", ImGuiTableColumnFlags_WidthFixed, 150);
            
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("命中记录 #%d 的寄存器状态:", detailWindow.selectedHitIndex + 1);
            
            ImGui::TableSetColumnIndex(1);
            if (ImGui::SmallButton("跳转到PC")) {
                const auto& hit = bp.hitHistory[detailWindow.selectedHitIndex];
                navigateToAddress(hit.regs_info.pc);
                Gui::log("跳转到PC地址: 0x%llX", hit.regs_info.pc);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("跳转到SP")) {
                const auto& hit = bp.hitHistory[detailWindow.selectedHitIndex];
                navigateToAddress(hit.regs_info.sp);
                Gui::log("跳转到栈地址: 0x%llX", hit.regs_info.sp);
            }
            
            ImGui::EndTable();
        }
        
        const auto& hit = bp.hitHistory[detailWindow.selectedHitIndex];
        drawRegisterInfoInWindow(hit.regs_info, hit.fpsimd_info, detailWindow);
    }
}

void BreakpointWindow::drawRegisterInfoInWindow(const struct _user_pt_regs& regs, const struct _user_fpsimd_state& fpsimd, BreakpointDetailWindow& detailWindow)
{
    // 使用标签页组织通用寄存器和浮点寄存器
    if (ImGui::BeginTabBar("RegisterTabs")) {
        // 通用寄存器标签页
        if (ImGui::BeginTabItem("通用寄存器")) {
            if (ImGui::BeginTable("RegisterTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
            {
                ImGui::TableSetupColumn("寄存器", ImGuiTableColumnFlags_WidthFixed, 80);
                ImGui::TableSetupColumn("值 (十六进制)", ImGuiTableColumnFlags_WidthFixed, 140);
                ImGui::TableSetupColumn("寄存器", ImGuiTableColumnFlags_WidthFixed, 80);
                ImGui::TableSetupColumn("值 (十六进制)", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();
                
                // 显示通用寄存器 (X0-X30)
                for (int i = 0; i < 31; i += 2) {
                    ImGui::TableNextRow();
                    
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextColored(ColorScheme::Register, "X%d", i);
                    ImGui::TableSetColumnIndex(1);
                    char regStr[32];
                    snprintf(regStr, sizeof(regStr), "0x%016llX", regs.regs[i]);
                    if (ImGui::Selectable(regStr, false, ImGuiSelectableFlags_None)) {
                        if (regs.regs[i] != 0) {
                            navigateToAddress(regs.regs[i]);
                            Gui::log("跳转到寄存器X%d地址: 0x%llX", i, regs.regs[i]);
                        }
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("十进制: %llu\n点击跳转到内存查看器", regs.regs[i]);
                    }
                    
                    if (i + 1 < 31) {
                        ImGui::TableSetColumnIndex(2);
                        ImGui::TextColored(ColorScheme::Register, "X%d", i + 1);
                        ImGui::TableSetColumnIndex(3);
                        snprintf(regStr, sizeof(regStr), "0x%016llX", regs.regs[i + 1]);
                        if (ImGui::Selectable(regStr, false, ImGuiSelectableFlags_None)) {
                            if (regs.regs[i + 1] != 0) {
                                navigateToAddress(regs.regs[i + 1]);
                                Gui::log("跳转到寄存器X%d地址: 0x%llX", i + 1, regs.regs[i + 1]);
                            }
                        }
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("十进制: %llu\n点击跳转到内存查看器", regs.regs[i + 1]);
                        }
                    }
                }
                
                // 显示特殊寄存器
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextColored(ColorScheme::RegisterSpecial, "SP");
                ImGui::TableSetColumnIndex(1);
                char spStr[32];
                snprintf(spStr, sizeof(spStr), "0x%016llX", regs.sp);
                if (ImGui::Selectable(spStr, false, ImGuiSelectableFlags_None)) {
                    navigateToAddress(regs.sp);
                    Gui::log("跳转到栈指针地址: 0x%llX", regs.sp);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("栈指针\n十进制: %llu\n点击跳转到内存查看器", regs.sp);
                }
                
                ImGui::TableSetColumnIndex(2);
                ImGui::TextColored(ColorScheme::RegisterSpecial, "PC");
                ImGui::TableSetColumnIndex(3);
                char pcStr[32];
                snprintf(pcStr, sizeof(pcStr), "0x%016llX", regs.pc);
                if (ImGui::Selectable(pcStr, false, ImGuiSelectableFlags_None)) {
                    navigateToAddress(regs.pc);
                    Gui::log("跳转到程序计数器地址: 0x%llX", regs.pc);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("程序计数器\n十进制: %llu\n点击跳转到内存查看器", regs.pc);
                }
                
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextColored(ColorScheme::RegisterStatus, "PSTATE");
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("0x%016llX", regs.pstate);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("处理器状态寄存器\n十进制: %llu", regs.pstate);
                }
                
                ImGui::TableSetColumnIndex(2);
                ImGui::TextColored(ColorScheme::RegisterStatus, "ORIG_X0");
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("0x%016llX", regs.orig_x0);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("原始X0寄存器值\n十进制: %llu", regs.orig_x0);
                }
                
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextColored(ColorScheme::RegisterStatus, "SYSCALLNO");
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("0x%016llX", regs.syscallno);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("系统调用号\n十进制: %llu", regs.syscallno);
                }
                
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        
        // 浮点寄存器标签页
        if (ImGui::BeginTabItem("浮点寄存器")) {
            // 浮点寄存器显示选项（静态变量，在标签页作用域内）
            static int fpDisplayMode = 0;  // 0: 十六进制, 1: 双精度浮点, 2: 单精度浮点, 3: 半精度浮点, 4: 64位整数, 5: 32位整数
            static int fpRegGroup = 0;  // 0: 全部, 1: 参数寄存器(V0-V7), 2: 被调用者保存(V8-V15), 3: 临时寄存器(V16-V31)
            static bool filterNonZero = false;
            static bool highlightSpecial = true;
            
            // 浮点状态和控制寄存器 - 增强显示
            if (ImGui::BeginTable("FPStatusTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
            {
                ImGui::TableSetupColumn("寄存器", ImGuiTableColumnFlags_WidthFixed, 100);
                ImGui::TableSetupColumn("值 (十六进制)", ImGuiTableColumnFlags_WidthFixed, 140);
                ImGui::TableSetupColumn("寄存器", ImGuiTableColumnFlags_WidthFixed, 100);
                ImGui::TableSetupColumn("值 (十六进制)", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();
                
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextColored(ColorScheme::FloatRegister, "FPSR");
                ImGui::TableSetColumnIndex(1);
                char fpsrStr[64];
                snprintf(fpsrStr, sizeof(fpsrStr), "0x%08X", fpsimd.fpsr);
                if (ImGui::Selectable(fpsrStr, false, ImGuiSelectableFlags_None)) {
                    ImGui::SetClipboardText(fpsrStr);
                }
                if (ImGui::IsItemHovered()) {
                    // 解析FPSR标志位
                    bool n = (fpsimd.fpsr & (1u << 31)) != 0;
                    bool z = (fpsimd.fpsr & (1u << 30)) != 0;
                    bool c = (fpsimd.fpsr & (1u << 29)) != 0;
                    bool v = (fpsimd.fpsr & (1u << 28)) != 0;
                    ImGui::SetTooltip("浮点状态寄存器 (FPSR)\n"
                                     "十六进制: 0x%08X\n"
                                     "十进制: %u\n"
                                     "条件标志: N=%d Z=%d C=%d V=%d\n"
                                     "点击复制", fpsimd.fpsr, fpsimd.fpsr, n, z, c, v);
                }
                
                ImGui::TableSetColumnIndex(2);
                ImGui::TextColored(ColorScheme::FloatRegister, "FPCR");
                ImGui::TableSetColumnIndex(3);
                char fpcrStr[64];
                snprintf(fpcrStr, sizeof(fpcrStr), "0x%08X", fpsimd.fpcr);
                if (ImGui::Selectable(fpcrStr, false, ImGuiSelectableFlags_None)) {
                    ImGui::SetClipboardText(fpcrStr);
                }
                if (ImGui::IsItemHovered()) {
                    // 解析FPCR的舍入模式 (bits [23:22])
                    int roundingMode = (fpsimd.fpcr >> 22) & 0x3;
                    const char* roundingModes[] = { "RN (最近舍入)", "RP (正无穷舍入)", "RM (负无穷舍入)", "RZ (零舍入)" };
                    ImGui::SetTooltip("浮点控制寄存器 (FPCR)\n"
                                     "十六进制: 0x%08X\n"
                                     "十进制: %u\n"
                                     "舍入模式: %s\n"
                                     "点击复制", fpsimd.fpcr, fpsimd.fpcr, roundingModes[roundingMode]);
                }
                
                ImGui::EndTable();
            }
            
            ImGui::Separator();
            
            // 显示选项控制面板
            if (ImGui::BeginTable("FPDisplayOptions", 4, ImGuiTableFlags_SizingStretchProp))
            {
                ImGui::TableSetupColumn("显示格式", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("寄存器分组", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("过滤选项", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("其他", ImGuiTableColumnFlags_WidthStretch);
                
                ImGui::TableNextRow();
                
                // 显示格式
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                ImGui::Text("显示格式:");
                ImGui::SameLine();
                const char* displayModes[] = { 
                    "十六进制 (128位)", 
                    "双精度浮点 (64位)", 
                    "单精度浮点 (32位)",
                    "半精度浮点 (16位)",
                    "64位整数",
                    "32位整数"
                };
                ImGui::SetNextItemWidth(180);
                ImGui::Combo("##FPDisplayMode", &fpDisplayMode, displayModes, 6);
                
                // 寄存器分组
                ImGui::TableSetColumnIndex(1);
                ImGui::AlignTextToFramePadding();
                ImGui::Text("寄存器分组:");
                ImGui::SameLine();
                const char* groupModes[] = { "全部 (V0-V31)", "参数寄存器 (V0-V7)", "被调用者保存 (V8-V15)", "临时寄存器 (V16-V31)" };
                ImGui::SetNextItemWidth(180);
                ImGui::Combo("##FPRegGroup", &fpRegGroup, groupModes, 4);
                
                // 过滤选项
                ImGui::TableSetColumnIndex(2);
                ImGui::AlignTextToFramePadding();
                ImGui::Checkbox("仅显示非零", &filterNonZero);
                
                // 其他选项
                ImGui::TableSetColumnIndex(3);
                ImGui::AlignTextToFramePadding();
                ImGui::Checkbox("高亮特殊值", &highlightSpecial);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("高亮显示 NaN、Inf、零值等特殊值");
                }
                
                ImGui::EndTable();
            }
            
            ImGui::Separator();
            
            // 辅助函数：检查浮点特殊值
            auto isFloatSpecial = [](double val) -> int {
                if (val == 0.0) return 1;  // 零值
                if (std::isnan(val)) return 2;  // NaN
                if (std::isinf(val)) return 3;  // Inf
                return 0;  // 正常值
            };
            
            auto isFloat32Special = [](float val) -> int {
                if (val == 0.0f) return 1;
                if (std::isnan(val)) return 2;
                if (std::isinf(val)) return 3;
                return 0;
            };
            
            // 确定显示范围
            int regStart = 0, regEnd = 32;
            switch (fpRegGroup) {
                case 1: regStart = 0; regEnd = 8; break;   // V0-V7
                case 2: regStart = 8; regEnd = 16; break;  // V8-V15
                case 3: regStart = 16; regEnd = 32; break; // V16-V31
                default: regStart = 0; regEnd = 32; break; // 全部
            }
            
            // 向量寄存器表格 (V0-V31)
            // 根据显示模式调整列数
            int numCols = 5;
            if (fpDisplayMode == 2) numCols = 7;      // 单精度：4个值
            else if (fpDisplayMode == 3) numCols = 11; // 半精度：8个值
            else if (fpDisplayMode == 5) numCols = 7;  // 32位整数：4个值
            
            if (ImGui::BeginTable("VectorRegisterTable", numCols, 
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable))
            {
                ImGui::TableSetupColumn("寄存器", ImGuiTableColumnFlags_WidthFixed, 70);
                ImGui::TableSetupColumn("低64位", ImGuiTableColumnFlags_WidthFixed, 160);
                ImGui::TableSetupColumn("高64位", ImGuiTableColumnFlags_WidthFixed, 160);
                
                // 根据显示模式设置列标题
                if (fpDisplayMode == 2) {
                    // 单精度浮点数模式：显示4个32位值
                    ImGui::TableSetupColumn("S[0]", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("S[1]", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("S[2]", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("S[3]", ImGuiTableColumnFlags_WidthStretch);
                } else if (fpDisplayMode == 3) {
                    // 半精度浮点数模式：显示8个16位值
                    for (int i = 0; i < 8; i++) {
                        char colName[16];
                        snprintf(colName, sizeof(colName), "H[%d]", i);
                        ImGui::TableSetupColumn(colName, ImGuiTableColumnFlags_WidthStretch);
                    }
                } else if (fpDisplayMode == 5) {
                    // 32位整数模式：显示4个32位值
                    ImGui::TableSetupColumn("W[0]", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("W[1]", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("W[2]", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("W[3]", ImGuiTableColumnFlags_WidthStretch);
                } else {
                    // 其他模式：2列格式化值
                    ImGui::TableSetupColumn("值 (格式)", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("值 (格式)", ImGuiTableColumnFlags_WidthStretch);
                }
                ImGui::TableHeadersRow();
                
                for (int i = regStart; i < regEnd; i++) {
                    // 获取128位向量寄存器的值（从字节数组读取）
                    uint64_t low64, high64;
#ifdef _MSC_VER
                    std::memcpy(&low64, fpsimd.vregs[i], 8);
                    std::memcpy(&high64, fpsimd.vregs[i] + 8, 8);
#else
                    __uint128_t vreg = fpsimd.vregs[i];
                    low64 = (uint64_t)(vreg & 0xFFFFFFFFFFFFFFFFULL);
                    high64 = (uint64_t)((vreg >> 64) & 0xFFFFFFFFFFFFFFFFULL);
#endif
                    
                    // 过滤非零寄存器
                    if (filterNonZero && low64 == 0 && high64 == 0) {
                        continue;
                    }
                    
                    ImGui::TableNextRow();
                    
                    // 寄存器名称（添加分组标记）
                    ImGui::TableSetColumnIndex(0);
                    const char* groupLabel = "";
                    if (i < 8) groupLabel = " [参数]";
                    else if (i < 16) groupLabel = " [保存]";
                    else groupLabel = " [临时]";
                    ImGui::TextColored(ColorScheme::FloatRegister, "V%d%s", i, groupLabel);
                    
                    // 低64位
                    ImGui::TableSetColumnIndex(1);
                    char lowStr[32];
                    snprintf(lowStr, sizeof(lowStr), "0x%016llX", low64);
                    char lowDisplayStr[64];
                    snprintf(lowDisplayStr, sizeof(lowDisplayStr), "D%d: %s", i, lowStr);
                    if (ImGui::Selectable(lowDisplayStr, false, ImGuiSelectableFlags_None)) {
                        ImGui::SetClipboardText(lowStr);
                        Gui::log("已复制 D%d 的值: %s", i, lowStr);
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("双精度视图 D%d\n十六进制: %s\n十进制: %llu\n点击复制", i, lowStr, low64);
                    }
                    
                    // 高64位
                    ImGui::TableSetColumnIndex(2);
                    char highStr[32];
                    snprintf(highStr, sizeof(highStr), "0x%016llX", high64);
                    char highDisplayStr[64];
                    snprintf(highDisplayStr, sizeof(highDisplayStr), "Q%d[64:127]", i);
                    if (ImGui::Selectable(highDisplayStr, false, ImGuiSelectableFlags_None)) {
                        ImGui::SetClipboardText(highStr);
                        Gui::log("已复制 Q%d[64:127] 的值: %s", i, highStr);
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Q寄存器高64位 [64:127]\n十六进制: %s\n十进制: %llu\n点击复制", highStr, high64);
                    }
                    
                    // 根据显示模式显示格式化的值
                    if (fpDisplayMode == 1) {
                        // 双精度浮点数 (64位)
                        ImGui::TableSetColumnIndex(3);
                        double dLow = *reinterpret_cast<double*>(&low64);
                        int specialLow = highlightSpecial ? isFloatSpecial(dLow) : 0;
                        ImVec4 colorLow = ColorScheme::FloatValue;
                        if (specialLow == 1) colorLow = ColorScheme::WarningLight;      // 零值
                        else if (specialLow == 2) colorLow = ColorScheme::ErrorLight;   // NaN
                        else if (specialLow == 3) colorLow = ColorScheme::WarningBright; // Inf
                        
                        char dLowStr[128];
                        if (specialLow == 2) snprintf(dLowStr, sizeof(dLowStr), "D%d: NaN", i);
                        else if (specialLow == 3) snprintf(dLowStr, sizeof(dLowStr), "D%d: %sInf", i, dLow < 0 ? "-" : "+");
                        else snprintf(dLowStr, sizeof(dLowStr), "D%d: %.15g", i, dLow);
                        
                        ImGui::TextColored(colorLow, "%s", dLowStr);
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("双精度浮点数 D%d\n值: %.15g\n十六进制: %s\n点击复制", i, dLow, lowStr);
                        }
                        
                        ImGui::TableSetColumnIndex(4);
                        double dHigh = *reinterpret_cast<double*>(&high64);
                        int specialHigh = highlightSpecial ? isFloatSpecial(dHigh) : 0;
                        ImVec4 colorHigh = ColorScheme::FloatValue;
                        if (specialHigh == 1) colorHigh = ColorScheme::WarningLight;
                        else if (specialHigh == 2) colorHigh = ColorScheme::ErrorLight;
                        else if (specialHigh == 3) colorHigh = ColorScheme::WarningBright;
                        
                        char dHighStr[128];
                        if (specialHigh == 2) snprintf(dHighStr, sizeof(dHighStr), "Q%d[64:127]: NaN", i);
                        else if (specialHigh == 3) snprintf(dHighStr, sizeof(dHighStr), "Q%d[64:127]: %sInf", i, dHigh < 0 ? "-" : "+");
                        else snprintf(dHighStr, sizeof(dHighStr), "Q%d[64:127]: %.15g", i, dHigh);
                        
                        ImGui::TextColored(colorHigh, "%s", dHighStr);
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("双精度浮点数 Q%d[64:127]\n值: %.15g\n十六进制: %s\n点击复制", i, dHigh, highStr);
                        }
                    } else if (fpDisplayMode == 2) {
                        // 单精度浮点数 (32位) - 显示所有4个值，分开显示
                        uint32_t fValues[4];
                        fValues[0] = (uint32_t)(low64 & 0xFFFFFFFF);
                        fValues[1] = (uint32_t)((low64 >> 32) & 0xFFFFFFFF);
                        fValues[2] = (uint32_t)(high64 & 0xFFFFFFFF);
                        fValues[3] = (uint32_t)((high64 >> 32) & 0xFFFFFFFF);
                        
                        for (int j = 0; j < 4; j++) {
                            ImGui::TableSetColumnIndex(3 + j);
                            float fVal = *reinterpret_cast<float*>(&fValues[j]);
                            int special = highlightSpecial ? isFloat32Special(fVal) : 0;
                            ImVec4 color = ColorScheme::FloatValue;
                            if (special == 1) color = ColorScheme::WarningLight;
                            else if (special == 2) color = ColorScheme::ErrorLight;
                            else if (special == 3) color = ColorScheme::WarningBright;
                            
                            char sStr[128];
                            if (special == 2) snprintf(sStr, sizeof(sStr), "NaN");
                            else if (special == 3) snprintf(sStr, sizeof(sStr), "%sInf", fVal < 0 ? "-" : "+");
                            else snprintf(sStr, sizeof(sStr), "%.7g", fVal);
                            
                            ImGui::TextColored(color, "%s", sStr);
                            if (ImGui::IsItemHovered()) {
                                ImGui::SetTooltip("S%d[%d]: %.7g\n十六进制: 0x%08X\n点击复制", i, j, fVal, fValues[j]);
                            }
                            if (ImGui::IsItemClicked(0)) {
                                char hexStr[16];
                                snprintf(hexStr, sizeof(hexStr), "0x%08X", fValues[j]);
                                ImGui::SetClipboardText(hexStr);
                            }
                        }
                    } else if (fpDisplayMode == 3) {
                        // 半精度浮点数 (16位) - 显示8个值
                        // 注意：这里需要将16位半精度转换为32位浮点显示
                        uint16_t hValues[8];
                        hValues[0] = (uint16_t)(low64 & 0xFFFF);
                        hValues[1] = (uint16_t)((low64 >> 16) & 0xFFFF);
                        hValues[2] = (uint16_t)((low64 >> 32) & 0xFFFF);
                        hValues[3] = (uint16_t)((low64 >> 48) & 0xFFFF);
                        hValues[4] = (uint16_t)(high64 & 0xFFFF);
                        hValues[5] = (uint16_t)((high64 >> 16) & 0xFFFF);
                        hValues[6] = (uint16_t)((high64 >> 32) & 0xFFFF);
                        hValues[7] = (uint16_t)((high64 >> 48) & 0xFFFF);
                        
                        for (int j = 0; j < 8; j++) {
                            ImGui::TableSetColumnIndex(3 + j);
                            // 简化的半精度转换（实际应该使用FP16库）
                            uint32_t sign = (hValues[j] >> 15) & 1;
                            uint32_t exp = (hValues[j] >> 10) & 0x1F;
                            uint32_t mant = hValues[j] & 0x3FF;
                            
                            char hStr[64];
                            if (exp == 0x1F) {
                                // 特殊值：Inf 或 NaN
                                if (mant == 0) snprintf(hStr, sizeof(hStr), "%sInf", sign ? "-" : "+");
                                else snprintf(hStr, sizeof(hStr), "NaN");
                            } else if (exp == 0 && mant == 0) {
                                // 零值
                                snprintf(hStr, sizeof(hStr), "0.0");
                            } else {
                                // 规范化或非规范化数：简化的转换（对于调试显示足够）
                                if (exp == 0) {
                                    // 非规范化数（denormalized）
                                    float approx = (float)(sign ? -1 : 1) * powf(2.0f, -14.0f) * (mant / 1024.0f);
                                    snprintf(hStr, sizeof(hStr), "%.4g", approx);
                                } else {
                                    // 规范化数
                                    float approx = (float)(sign ? -1 : 1) * powf(2.0f, (int)exp - 15) * (1.0f + mant / 1024.0f);
                                    snprintf(hStr, sizeof(hStr), "%.4g", approx);
                                }
                            }
                            
                            ImGui::TextColored(ColorScheme::FloatValue, "%s", hStr);
                            if (ImGui::IsItemHovered()) {
                                ImGui::SetTooltip("H%d[%d]: 0x%04X", i, j, hValues[j]);
                            }
                        }
                    } else if (fpDisplayMode == 4) {
                        // 64位整数
                        ImGui::TableSetColumnIndex(3);
                        ImGui::TextColored(ColorScheme::FloatValue, "%lld", (long long)low64);
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("64位有符号整数: %lld\n无符号: %llu", (long long)low64, low64);
                        }
                        
                        ImGui::TableSetColumnIndex(4);
                        ImGui::TextColored(ColorScheme::FloatValue, "%lld", (long long)high64);
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("64位有符号整数: %lld\n无符号: %llu", (long long)high64, high64);
                        }
                    } else if (fpDisplayMode == 5) {
                        // 32位整数 - 显示4个值
                        int32_t wValues[4];
                        wValues[0] = (int32_t)(low64 & 0xFFFFFFFF);
                        wValues[1] = (int32_t)((low64 >> 32) & 0xFFFFFFFF);
                        wValues[2] = (int32_t)(high64 & 0xFFFFFFFF);
                        wValues[3] = (int32_t)((high64 >> 32) & 0xFFFFFFFF);
                        
                        for (int j = 0; j < 4; j++) {
                            ImGui::TableSetColumnIndex(3 + j);
                            ImGui::TextColored(ColorScheme::FloatValue, "%d", wValues[j]);
                            if (ImGui::IsItemHovered()) {
                                ImGui::SetTooltip("W%d[%d]: %d (0x%08X)", i, j, wValues[j], (uint32_t)wValues[j]);
                            }
                        }
                    } else {
                        // 十六进制显示模式：显示完整的Q寄存器
                        ImGui::TableSetColumnIndex(3);
                        char qLowStr[64];
                        snprintf(qLowStr, sizeof(qLowStr), "Q%d[0:63]", i);
                        if (ImGui::Selectable(qLowStr, false, ImGuiSelectableFlags_None)) {
                            ImGui::SetClipboardText(lowStr);
                        }
                        
                        ImGui::TableSetColumnIndex(4);
                        char qHighStr[64];
                        snprintf(qHighStr, sizeof(qHighStr), "Q%d[64:127]", i);
                        if (ImGui::Selectable(qHighStr, false, ImGuiSelectableFlags_None)) {
                            ImGui::SetClipboardText(highStr);
                        }
                    }
                }
                
                ImGui::EndTable();
            }
            
            // 显示说明
            ImGui::Separator();
            ImGui::TextColored(ColorScheme::TextSecondary, "提示:");
            ImGui::BulletText("寄存器分组: [参数]=V0-V7(函数参数), [保存]=V8-V15(被调用者保存), [临时]=V16-V31(临时寄存器)");
            ImGui::BulletText("点击寄存器值可复制到剪贴板");
            ImGui::BulletText("特殊值颜色: 黄色=零值/Inf, 红色=NaN");
            
            ImGui::EndTabItem();
        }
        
        ImGui::EndTabBar();
    }
}

void BreakpointWindow::closeBreakpointDetailWindow(int windowIndex)
{
    if (windowIndex >= 0 && windowIndex < (int)detailWindows.size()) {
        detailWindows[windowIndex].isOpen = false;
    }
} 

std::string BreakpointWindow::formatTime(uint64_t timestamp)
{
    if (timestamp == 0) return "未知";
    
    // 简单的时间戳格式化
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%llu", timestamp);
    return std::string(buffer);
}

std::string BreakpointWindow::formatTimeDiff(uint64_t start, uint64_t end)
{
    if (start == 0 || end == 0 || end <= start) return "-";
    
    uint64_t diff = end - start;
    char buffer[32];
    
    if (diff < 1000) {
        snprintf(buffer, sizeof(buffer), "%llu", diff);
    } else if (diff < 1000000) {
        snprintf(buffer, sizeof(buffer), "%.1fK", diff / 1000.0);
    } else if (diff < 1000000000) {
        snprintf(buffer, sizeof(buffer), "%.1fM", diff / 1000000.0);
    } else {
        snprintf(buffer, sizeof(buffer), "%.1fG", diff / 1000000000.0);
    }
    
    return std::string(buffer);
}

// 反汇编显示函数
void BreakpointWindow::drawDisassemblyInWindow(uint64_t address, const uint8_t* code, size_t codeSize)
{
    if (!disassemblyInitialized || !disassemblyHelper) {
        ImGui::TextColored(ColorScheme::Warning, "反汇编引擎未初始化");
        ImGui::TextWrapped("提示: 请确保已安装 Capstone 库");
        ImGui::Separator();
        ImGui::Text("安装方法:");
        ImGui::BulletText("使用 vcpkg: vcpkg install capstone:x64-windows");
        ImGui::BulletText("或从 https://www.capstone-engine.org/ 下载预编译版本");
        return;
    }
    
    if (!code || codeSize == 0) {
        ImGui::TextDisabled("无可用代码数据");
        return;
    }
    
    // 反汇编代码
    DisassemblyResult result = disassemblyHelper->disassembleMultiple(address, code, codeSize, 20);
    
    if (!result.success) {
        ImGui::TextColored(ColorScheme::ErrorBright, "反汇编失败: %s", result.errorMessage.c_str());
        return;
    }
    
    // 显示反汇编结果
    ImGui::Text("架构: %s", DisassemblyHelper::getArchitectureName(disassemblyHelper->getCurrentArchitecture()).c_str());
    ImGui::Text("指令数量: %d", (int)result.instructions.size());
    ImGui::Separator();
    
    // 使用表格显示反汇编指令
    if (ImGui::BeginTable("DisassemblyTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY))
    {
        ImGui::TableSetupColumn("地址", ImGuiTableColumnFlags_WidthFixed, 120);
        ImGui::TableSetupColumn("十六进制", ImGuiTableColumnFlags_WidthFixed, 120);
        ImGui::TableSetupColumn("助记符", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("操作数", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        
        for (const auto& instr : result.instructions) {
            ImGui::TableNextRow();
            
            // 地址列
            ImGui::TableSetColumnIndex(0);
            char addrStr[32];
            snprintf(addrStr, sizeof(addrStr), "0x%llX", instr.address);
            if (ImGui::Selectable(addrStr, false, ImGuiSelectableFlags_SpanAllColumns)) {
                // 点击地址可以跳转到内存查看器
                navigateToAddress(instr.address);
                Gui::log("跳转到地址: 0x%llX", instr.address);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("点击跳转到内存查看器\n地址: 0x%llX\n大小: %u 字节", 
                                 instr.address, instr.size);
            }
            
            // 右键菜单
            char popup_id[64];
            snprintf(popup_id, sizeof(popup_id), "DisasmPopup_%llX", instr.address);
            if (ImGui::BeginPopupContextItem(popup_id)) {
                if (ImGui::MenuItem("复制地址")) {
                    ImGui::SetClipboardText(addrStr);
                }
                if (ImGui::MenuItem("复制指令")) {
                    ImGui::SetClipboardText(instr.fullInstruction.c_str());
                }
                if (ImGui::MenuItem("在内存查看器中打开")) {
                    navigateToAddress(instr.address);
                }
                ImGui::EndPopup();
            }
            
            // 十六进制字节列
            ImGui::TableSetColumnIndex(1);
            ImGui::TextColored(ColorScheme::DisassemblyHex, "%s", instr.hexBytes.c_str());
            
            // 助记符列
            ImGui::TableSetColumnIndex(2);
            ImGui::TextColored(ColorScheme::DisassemblyMnemonic, "%s", instr.mnemonic.c_str());
            
            // 操作数列
            ImGui::TableSetColumnIndex(3);
            if (!instr.operands.empty()) {
                ImGui::Text("%s", instr.operands.c_str());
            } else {
                ImGui::TextDisabled("-");
            }
        }
        
        ImGui::EndTable();
    }
}

// 读取并反汇编PC地址周围的指令
void BreakpointWindow::drawDisassemblyForPC(uint64_t pcAddress, int beforeCount, int afterCount, BreakpointDetailWindow& detailWindow)
{
    if (!disassemblyInitialized || !disassemblyHelper) {
        ImGui::TextColored(ColorScheme::Warning, "反汇编引擎未初始化");
        return;
    }
    
    if (pcAddress == 0) {
        ImGui::TextDisabled("PC地址无效");
        return;
    }
    
    // ARM64指令是4字节对齐的
    const uint32_t INSTRUCTION_SIZE = 4;
    
    // 计算需要读取的内存范围
    uint64_t startAddress = pcAddress - (beforeCount * INSTRUCTION_SIZE);
    uint32_t totalInstructions = beforeCount + 1 + afterCount;
    uint32_t totalSize = totalInstructions * INSTRUCTION_SIZE;
    
    // 检查缓存是否有效
    bool cacheValid = detailWindow.disasmCache.isValid &&
                     detailWindow.disasmCache.cachedPCAddress == pcAddress &&
                     detailWindow.disasmCache.cachedBeforeCount == beforeCount &&
                     detailWindow.disasmCache.cachedAfterCount == afterCount &&
                     detailWindow.disasmCache.cachedResult != nullptr;
    
    // 自动刷新检查
    float currentTime = ImGui::GetTime();
    bool shouldAutoRefresh = detailWindow.autoRefreshDisasm && 
                            (currentTime - detailWindow.lastDisasmRefreshTime >= detailWindow.disasmRefreshInterval);
    
    // 控制面板
    ImGui::Text("反汇编控制:");
    ImGui::SameLine();
    if (ImGui::Button("手动刷新")) {
        cacheValid = false;  // 强制刷新
        detailWindow.lastDisasmRefreshTime = currentTime;
    }
    ImGui::SameLine();
    ImGui::Checkbox("自动刷新", &detailWindow.autoRefreshDisasm);
    if (detailWindow.autoRefreshDisasm) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        ImGui::SliderFloat("间隔##disasm", &detailWindow.disasmRefreshInterval, 1.0f, 10.0f, "%.1fs");
    }
    
    // 显示缓存状态
    ImGui::SameLine();
    if (cacheValid) {
        ImGui::TextColored(ColorScheme::CacheValid, "[缓存]");
    } else {
        ImGui::TextColored(ColorScheme::CacheLoading, "[读取中]");
    }
    
    ImGui::Separator();
    
    // 如果需要刷新或缓存无效，重新读取和反汇编
    if (!cacheValid || shouldAutoRefresh) {
        // 从远程进程读取内存（使用调试端口进行自动刷新）
        std::vector<unsigned char> memoryData;
        bool readSuccess = ReadProcessMemoryBytes(startAddress, totalSize, memoryData, PORT_DEBUG);
        
        if (!readSuccess || memoryData.empty()) {
            // 读取失败，但如果有缓存，继续使用缓存
            if (cacheValid && detailWindow.disasmCache.cachedResult) {
                ImGui::TextColored(ColorScheme::Warning, "警告: 内存读取失败，使用缓存数据");
                ImGui::Separator();
            } else {
                ImGui::TextColored(ColorScheme::ErrorBright, "内存读取失败");
                ImGui::TextWrapped("无法从地址 0x%llX 读取 %u 字节的内存。", startAddress, totalSize);
                ImGui::Separator();
                ImGui::Text("可能的原因:");
                ImGui::BulletText("地址无效或不可访问");
                ImGui::BulletText("进程未连接或句柄无效");
                ImGui::BulletText("内存区域没有执行权限");
                
                // 清除缓存
                detailWindow.disasmCache.isValid = false;
                detailWindow.disasmCache.cachedResult.reset();
                return;
            }
        } else {
            // 读取成功，反汇编
            auto result = std::make_shared<DisassemblyResult>();
            *result = disassemblyHelper->disassembleMultiple(
                startAddress, 
                memoryData.data(), 
                memoryData.size(), 
                totalInstructions
            );
            
            if (!result->success || result->instructions.empty()) {
                // 反汇编失败，但如果有缓存，继续使用缓存
                if (cacheValid && detailWindow.disasmCache.cachedResult) {
                    ImGui::TextColored(ColorScheme::Warning, "警告: 反汇编失败，使用缓存数据");
                    ImGui::Separator();
                } else {
                    ImGui::TextColored(ColorScheme::ErrorBright, "反汇编失败: %s", 
                                      result->success ? "没有指令" : result->errorMessage.c_str());
                    detailWindow.disasmCache.isValid = false;
                    detailWindow.disasmCache.cachedResult.reset();
                    return;
                }
            } else {
                // 成功，更新缓存
                detailWindow.disasmCache.cachedPCAddress = pcAddress;
                detailWindow.disasmCache.cachedBeforeCount = beforeCount;
                detailWindow.disasmCache.cachedAfterCount = afterCount;
                detailWindow.disasmCache.cachedMemoryData = std::move(memoryData);
                detailWindow.disasmCache.cachedResult = result;
                detailWindow.disasmCache.isValid = true;
                detailWindow.lastDisasmRefreshTime = currentTime;
                
                ImGui::TextColored(ColorScheme::CacheValid, "内存读取成功 (%zu 字节)", 
                                  detailWindow.disasmCache.cachedMemoryData.size());
                ImGui::Separator();
            }
        }
    }
    
    // 使用缓存的结果显示
    if (!detailWindow.disasmCache.isValid || !detailWindow.disasmCache.cachedResult) {
        ImGui::TextDisabled("没有可用的反汇编数据");
        return;
    }
    
    auto& result = *detailWindow.disasmCache.cachedResult;
    
    // 显示反汇编结果
    ImGui::Text("架构: %s", DisassemblyHelper::getArchitectureName(disassemblyHelper->getCurrentArchitecture()).c_str());
    ImGui::Text("反汇编指令数: %d / %d", (int)result.instructions.size(), totalInstructions);
    ImGui::Separator();
    
    // 使用表格显示反汇编指令
    if (ImGui::BeginTable("DisassemblyPCTable", 5, 
        ImGuiTableFlags_Borders | 
        ImGuiTableFlags_RowBg | 
        ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_Resizable))
    {
        ImGui::TableSetupColumn("标记", ImGuiTableColumnFlags_WidthFixed, 40);
        ImGui::TableSetupColumn("地址", ImGuiTableColumnFlags_WidthFixed, 120);
        ImGui::TableSetupColumn("十六进制", ImGuiTableColumnFlags_WidthFixed, 120);
        ImGui::TableSetupColumn("助记符", ImGuiTableColumnFlags_WidthFixed, 100);
        ImGui::TableSetupColumn("操作数", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        
        for (const auto& instr : result.instructions) {
            bool isCurrentPC = (instr.address == pcAddress);
            
            ImGui::TableNextRow();
            
            // 如果是当前PC，高亮显示整行
            if (isCurrentPC) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, 
                    ImGui::GetColorU32(ColorScheme::DisassemblyPCBg));
            }
            
            // 标记列 - 显示PC指示器
            ImGui::TableSetColumnIndex(0);
            if (isCurrentPC) {
                ImGui::TextColored(ColorScheme::DisassemblyPC, "=>");
            } else {
                ImGui::TextDisabled("  ");
            }
            
            // 地址列
            ImGui::TableSetColumnIndex(1);
            std::string addrDisplayStr = AppContext::Get().moduleCache.formatAddressWithModuleAndSymbol(instr.address);
            char rawAddrStr[32];
            snprintf(rawAddrStr, sizeof(rawAddrStr), "0x%llX", instr.address);
            
            // 当前PC用不同颜色显示
            if (isCurrentPC) {
                ImGui::TextColored(ColorScheme::DisassemblyPC, "%s", addrDisplayStr.c_str());
            } else {
                if (ImGui::Selectable(addrDisplayStr.c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) {
                    navigateToAddress(instr.address);
                    Gui::log("跳转到地址: 0x%llX", instr.address);
                }
            }
            
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("地址: 0x%llX\n解析: %s\n大小: %u 字节%s", 
                                 instr.address, addrDisplayStr.c_str(), instr.size,
                                 isCurrentPC ? "\n[当前PC]" : "");
            }
            
            // 右键菜单
            char popup_id[64];
            snprintf(popup_id, sizeof(popup_id), "DisasmPCPopup_%llX", instr.address);
            if (ImGui::BeginPopupContextItem(popup_id)) {
                if (ImGui::MenuItem("复制地址")) {
                    ImGui::SetClipboardText(rawAddrStr);
                }
                if (ImGui::MenuItem("复制指令")) {
                    ImGui::SetClipboardText(instr.fullInstruction.c_str());
                }
                if (ImGui::MenuItem("复制十六进制")) {
                    ImGui::SetClipboardText(instr.hexBytes.c_str());
                }
                ImGui::Separator();
                if (ImGui::MenuItem("在内存查看器中打开")) {
                    navigateToAddress(instr.address);
                }
                ImGui::EndPopup();
            }
            
            // 十六进制字节列
            ImGui::TableSetColumnIndex(2);
            if (isCurrentPC) {
                ImGui::TextColored(ColorScheme::FloatValue, "%s", instr.hexBytes.c_str());
            } else {
                ImGui::TextColored(ColorScheme::DisassemblyHex, "%s", instr.hexBytes.c_str());
            }
            
            // 助记符列
            ImGui::TableSetColumnIndex(3);
            if (isCurrentPC) {
                ImGui::TextColored(ColorScheme::DisassemblyMnemonic, "%s", instr.mnemonic.c_str());
            } else {
                ImGui::TextColored(ColorScheme::DisassemblyMnemonic, "%s", instr.mnemonic.c_str());
            }
            
            // 操作数列
            ImGui::TableSetColumnIndex(4);
            if (!instr.operands.empty()) {
                if (isCurrentPC) {
                    ImGui::TextColored(ColorScheme::TextPrimary, "%s", instr.operands.c_str());
                } else {
                    ImGui::Text("%s", instr.operands.c_str());
                }
            } else {
                ImGui::TextDisabled("-");
            }
        }
        
        ImGui::EndTable();
    }
    
    // 显示说明
    ImGui::Separator();
    ImGui::TextColored(ColorScheme::TextSecondary, "提示: => 标记表示当前PC位置");
    ImGui::TextColored(ColorScheme::TextSecondary, "右键点击指令可复制或在内存查看器中查看");
}

