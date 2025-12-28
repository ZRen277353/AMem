#include "PointerChainWindow.h"
#include "ColorScheme.h"
#include "MemoryViewerWindow.h"
#include "Gui.h"
#include "../imgui/imgui.h"
#include <sstream>
#include <iomanip>
#include <algorithm>

PointerChainWindow::PointerChainWindow()
{
    name = "指针链扫描";
    formatter = std::make_unique<memchainer::PointerFormatter>();
}

unsigned int PointerChainWindow::getWindowFlags() const
{
    return ImGuiWindowFlags_NoDocking;
}

PointerChainWindow::~PointerChainWindow()
{
    // 如果有指针加载线程正在运行，先取消并等待
    if (pointerLoadThread.joinable()) {
        pointerLoadInProgress = false;
        pointerLoadThread.join();
    }
    
    // 如果有扫描线程正在运行，先取消并等待
    if (scanThread.joinable()) {
        scanCancelled = true;
        scanThread.join();
    }
}

void PointerChainWindow::setProcessInfo(int* pid, std::string* name)
{
    selectedPid = pid;
    selectedName = name;
}

void PointerChainWindow::setMemoryViewerWindow(MemoryViewerWindow* memViewer)
{
    memoryViewerWindow = memViewer;
}

void PointerChainWindow::setOpenMemoryViewerCallback(std::function<MemoryViewerWindow*()> callback)
{
    openMemoryViewerCallback = callback;
}

void PointerChainWindow::onDraw()
{
    if (!pOpen) return;

    if (ImGui::Begin(name.c_str(), &pOpen, ImGuiWindowFlags_None))
    {
        // 显示进程信息
        if (selectedPid && *selectedPid != 0) {
            ImGui::TextColored(ColorScheme::SuccessBright, "已附加: %s (PID %d)", 
                selectedName ? selectedName->c_str() : "Unknown", *selectedPid);
        } else {
            ImGui::TextDisabled("未附加进程");
        }
        ImGui::Separator();

        // 主要布局：左侧扫描面板，右侧结果面板
        ImGui::BeginChild("LeftPanel", ImVec2(350, 0), ImGuiChildFlags_Borders);
        drawScanPanel();
        ImGui::EndChild();
        
        ImGui::SameLine();
        
        ImGui::BeginChild("RightPanel", ImVec2(0, 0), ImGuiChildFlags_Borders);
        if (pointerLoadInProgress || scanInProgress) {
            drawProgressPanel();
        } else if (showPotentialPointers && pointersLoaded) {
            // 显示潜在指针数据面板
            drawPotentialPointersPanel();
        } else if (scanCompleted) {
            drawResultsPanel();
        } else {
            ImGui::TextDisabled("请先获取潜在指针，然后配置参数并开始扫描");
        }
        ImGui::EndChild();
    }
    ImGui::End();
}

void PointerChainWindow::drawScanPanel()
{
    // ===== 潜在指针获取部分 =====
    ImGui::TextColored(ColorScheme::SuccessLight, "潜在指针数据");
    ImGui::Separator();
    
    // 显示潜在指针状态
    if (pointersLoaded) {
        ImGui::TextColored(ColorScheme::SuccessBright, "已加载: %u 个指针", loadedPointerCount);
    } else {
        ImGui::TextDisabled("未加载潜在指针");
    }
    
    ImGui::Spacing();
    
    // 获取潜在指针按钮
    bool canLoadPointers = selectedPid && *selectedPid != 0 && !pointerLoadInProgress && !scanInProgress;
    if (!canLoadPointers) {
        ImGui::BeginDisabled();
    }
    
    if (ImGui::Button("获取潜在指针", ImVec2(-1, 35))) {
        startLoadPointers();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("扫描内存以获取所有潜在指针数据");
    }
    
    if (!canLoadPointers) {
        ImGui::EndDisabled();
    }
    
    // 取消加载按钮
    if (pointerLoadInProgress) {
        ImGui::Spacing();
        if (ImGui::Button("取消加载", ImVec2(-1, 25))) {
            cancelLoadPointers();
        }
    }
    
    // 查看潜在指针数据按钮
    if (pointersLoaded && !scanInProgress && !pointerLoadInProgress) {
        ImGui::Spacing();
        const char* buttonText = showPotentialPointers ? "返回扫描界面" : "查看潜在指针数据";
        if (ImGui::Button(buttonText, ImVec2(-1, 30))) {
            showPotentialPointers = !showPotentialPointers;
            if (showPotentialPointers) {
                // 准备排序后的指针列表
                updateSortedPointers();
            }
        }
        if (ImGui::IsItemHovered() && !showPotentialPointers) {
            ImGui::SetTooltip("查看已加载的潜在指针，包括引用次数和偏移量列表");
        }
    }
    
    ImGui::Spacing();
    ImGui::Separator();
    
    // ===== 扫描参数部分 =====
    // 如果正在查看潜在指针，显示过滤选项；否则显示扫描参数
    if (showPotentialPointers) {
        ImGui::TextColored(ColorScheme::InfoLight, "潜在指针过滤");
    ImGui::Spacing();
    
    // 排序选项
    bool needUpdate = false;
    ImGui::Text("  排序:");
    ImGui::SameLine();
    if (ImGui::Checkbox("按引用次数##Sort", &sortByRefCount)) {
        needUpdate = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("勾选后将高频指针排在前面，否则按地址排序");
    }
    
    ImGui::Spacing();
    
    // 过滤选项
    ImGui::Text("  过滤:");
    ImGui::SameLine();
    ImGui::Text("引用≥");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    if (ImGui::InputInt("##MinRefCount", &minRefCountFilter)) {
        if (minRefCountFilter < 0) minRefCountFilter = 0;
        needUpdate = true;
    }
    ImGui::SameLine();
    ImGui::Text("偏移量≥");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    if (ImGui::InputInt("##MinOffsetCount", &minOffsetCountFilter)) {
        if (minOffsetCountFilter < 0) minOffsetCountFilter = 0;
        needUpdate = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("应用", ImVec2(80, 0))) {
        needUpdate = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("应用过滤条件并重新排序");
    }
    ImGui::SameLine();
    if (ImGui::Button("重置", ImVec2(80, 0))) {
        minRefCountFilter = 0;
        minOffsetCountFilter = 0;
        needUpdate = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("清除所有过滤条件");
    }
    
    if (needUpdate) {
        updateSortedPointers();
        pointerDisplayOffset = 0;
    }
    } else {
        // 扫描模式：显示扫描参数
        ImGui::TextColored(ColorScheme::InfoLight, "扫描参数");
    
    ImGui::Spacing();
    
    // 目标地址输入
    ImGui::Text("目标地址:");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##TargetAddress", targetAddressBuf, sizeof(targetAddressBuf));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("输入十六进制地址，例如: 0x12345678 或 12345678");
    }
    
    ImGui::Spacing();
    
    // 最大深度
    ImGui::Text("最大深度:");
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderInt("##MaxDepth", &maxDepth, 1, 20);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("指针链的最大层数");
    }
    
    ImGui::Spacing();
    
    // 最大偏移
    ImGui::Text("最大偏移:");
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderInt("##MaxOffset", &maxOffset, 0, 5000);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("指针偏移的最大值（字节）");
    }
    
    ImGui::Spacing();
    
    // 结果限制
    ImGui::Checkbox("限制结果数量", &limitResults);
    if (limitResults) {
        ImGui::Text("最大结果数:");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputInt("##MaxLimit", (int*)&maxLimit);
        if (maxLimit < 0) maxLimit = -1;//-1表示不限制
    }
    
    ImGui::Spacing();
    
    // 使用已有指针数据选项
    if (pointersLoaded) {
        ImGui::Checkbox("使用已加载的指针数据", &useExistingPointers);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("勾选后将使用已加载的指针数据，否则重新获取");
        }
        ImGui::Spacing();
    }
    
    ImGui::Separator();
    
    // 开始扫描按钮
    bool canScan = selectedPid && *selectedPid != 0 && !scanInProgress && !pointerLoadInProgress;
    if (!canScan) {
        ImGui::BeginDisabled();
    }
    
    if (ImGui::Button("开始扫描", ImVec2(-1, 40))) {
        startScan();
    }
    
    if (!canScan) {
        ImGui::EndDisabled();
    }
    
    // 取消按钮
    if (scanInProgress) {
        ImGui::Spacing();
        if (ImGui::Button("取消扫描", ImVec2(-1, 30))) {
            cancelScan();
        }
    }
    
    // 导出选项
    if (scanCompleted && !chains.empty()) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextColored(ColorScheme::InfoLight, "导出选项");
        
        ImGui::Text("文件名:");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##ExportFile", exportFileBuf, sizeof(exportFileBuf));
        
        ImGui::Spacing();
        if (ImGui::Button("导出到文件", ImVec2(-1, 30))) {
            exportToFile();
        }
    }
    }  // 结束 showPotentialPointers 的 else 分支
}

void PointerChainWindow::drawProgressPanel()
{
    // 如果正在加载指针，显示加载进度
    if (pointerLoadInProgress) {
        ImGui::TextColored(ColorScheme::SuccessLight, "正在获取潜在指针...");
        ImGui::Separator();
        ImGui::Spacing();
        
        // 进度条
        char progressText[128];
        if (pointerLoadTotalRegions > 0) {
            snprintf(progressText, sizeof(progressText), "区域 %u / %u (%.1f%%)", 
                     pointerLoadCurrentRegion, pointerLoadTotalRegions, pointerLoadProgress * 100.0f);
        } else {
            snprintf(progressText, sizeof(progressText), "正在准备...");
        }
        
        ImGui::Text("扫描进度:");
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ColorScheme::InfoBright);
        ImGui::ProgressBar(pointerLoadProgress, ImVec2(-1, 30), progressText);
        ImGui::PopStyleColor();
        
        ImGui::Spacing();
        if (pointerLoadTotalRegions > 0) {
            ImGui::Text("当前区域: %u / %u", pointerLoadCurrentRegion, pointerLoadTotalRegions);
        }
        ImGui::Text("正在扫描内存区域以查找潜在指针...");
        
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextWrapped("这可能需要一些时间，请耐心等待...");
        return;
    }
    
    // 显示扫描进度
    ImGui::TextColored(ColorScheme::SuccessLight, "指针链扫描进行中");
    ImGui::Separator();
    ImGui::Spacing();
    
    // 主进度条
    char progressText[128];
    if (scanProgressInfo.totalBranches > 0) {
        snprintf(progressText, sizeof(progressText), "%.1f%% - 分支 %u/%u", 
                 scanProgressInfo.progress * 100.0f,
                 scanProgressInfo.currentBranch, 
                 scanProgressInfo.totalBranches);
    } else {
        snprintf(progressText, sizeof(progressText), "准备中...");
    }
    
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ColorScheme::SuccessBright);
    ImGui::ProgressBar(scanProgressInfo.progress, ImVec2(-1, 35), progressText);
    ImGui::PopStyleColor();
    
    ImGui::Spacing();
    ImGui::Separator();
    
    // 统计信息面板
    ImGui::BeginChild("ScanStats", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    
    ImGui::Spacing();
    
    // 指针链统计
    ImGui::PushStyleColor(ImGuiCol_Text, ColorScheme::SuccessBright);
    ImGui::Text("  已找到指针链:");
    ImGui::PopStyleColor();
    ImGui::SameLine(150);
    ImGui::Text("%u 条", scanProgressInfo.chainsFound);
    
    ImGui::Spacing();
    
    // 节点统计
    ImGui::Text("  已处理节点:");
    ImGui::SameLine(150);
    if (scanProgressInfo.nodesProcessed > 1000000) {
        ImGui::TextColored(ColorScheme::WarningBright, "%.2fM", 
                          scanProgressInfo.nodesProcessed / 1000000.0f);
    } else if (scanProgressInfo.nodesProcessed > 1000) {
        ImGui::Text("%.1fK", scanProgressInfo.nodesProcessed / 1000.0f);
    } else {
        ImGui::Text("%u", scanProgressInfo.nodesProcessed);
    }
    
    ImGui::Spacing();
    
    // 当前分支
    ImGui::Text("  当前分支:");
    ImGui::SameLine(150);
    ImGui::Text("%u / %u", scanProgressInfo.currentBranch, scanProgressInfo.totalBranches);
    
    ImGui::Spacing();
    
    // 搜索深度
    ImGui::Text("  当前深度:");
    ImGui::SameLine(150);
    ImGui::Text("%u / %u", scanProgressInfo.currentDepth, scanProgressInfo.maxDepth);
    
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    
    // 搜索状态
    ImGui::TextDisabled("  使用深度优先搜索递归查找指针链...");
    
    // 性能提示
    if (scanProgressInfo.nodesProcessed > 1000000) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ColorScheme::WarningBright);
        ImGui::TextWrapped("  提示: 已处理大量节点，扫描可能需要较长时间，请耐心等待...");
        ImGui::PopStyleColor();
    }
    
    ImGui::EndChild();
}

void PointerChainWindow::drawResultsPanel()
{
    ImGui::TextColored(ColorScheme::SuccessLight, "扫描结果");
    ImGui::SameLine();
    ImGui::TextDisabled("(找到 %d 条指针链)", static_cast<int>(chains.size()));
    ImGui::Separator();
    
    if (chains.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ColorScheme::WarningBright, "未找到有效的指针链");
        ImGui::Spacing();
        ImGui::TextWrapped("提示: 尝试增加最大深度或最大偏移量，或者检查目标地址是否正确。");
        return;
    }
    
    // 分页控制
    ImGui::Text("显示范围:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100);
    ImGui::InputInt("##DisplayOffset", &displayOffset);
    ImGui::SameLine();
    ImGui::Text("- ");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100);
    ImGui::InputInt("##DisplayLimit", &displayLimit);
    
    // 限制范围
    if (displayOffset < 0) displayOffset = 0;
    if (displayOffset >= static_cast<int>(chains.size())) displayOffset = chains.size() - 1;
    if (displayLimit < 1) displayLimit = 1;
    if (displayLimit > 1000) displayLimit = 1000;
    
    ImGui::Separator();
    
    // 指针链列表
    ImGui::BeginChild("ChainList", ImVec2(0, 0), ImGuiChildFlags_None);
    
    int endIndex = std::min(displayOffset + displayLimit, static_cast<int>(chains.size()));
    
    for (int i = displayOffset; i < endIndex; i++) {
        const auto& chain = chains[i];
        
        // 创建可选择的项
        bool isSelected = (selectedChainIndex == i);
        std::string chainLabel = formatChainString(chain);
        
        ImGui::PushID(i);
        
        if (ImGui::Selectable(("##chain" + std::to_string(i)).c_str(), isSelected, 0, ImVec2(0, 0))) {
            selectedChainIndex = i;
        }
        
        // 显示指针链编号和简要信息
        ImGui::SameLine();
        ImGui::Text("[%d]", i);
        ImGui::SameLine();
        
        // 显示链的深度
        ImGui::TextColored(ColorScheme::InfoLight, "深度:%d", static_cast<int>(chain.size()));
        ImGui::SameLine();
        
        // 显示链字符串（截断）
        std::string displayStr = chainLabel;
        if (displayStr.length() > 80) {
            displayStr = displayStr.substr(0, 77) + "...";
        }
        ImGui::TextWrapped("%s", displayStr.c_str());
        
        // 如果被选中，显示详细信息
        if (isSelected) {
            ImGui::Indent(20);
            drawChainDetail(chain);
            ImGui::Unindent(20);
        }
        
        ImGui::PopID();
        ImGui::Separator();
    }
    
    ImGui::EndChild();
}

void PointerChainWindow::drawChainDetail(const std::list<memchainer::PointerChainNode>& chain)
{
    ImGui::TextColored(ColorScheme::SuccessLight, "指针链详情:");
    
    if (ImGui::BeginTable("ChainDetailTable", 4, 
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
        ImVec2(0, 200)))
    {
        ImGui::TableSetupColumn("层级", ImGuiTableColumnFlags_WidthFixed, 50);
        ImGui::TableSetupColumn("地址", ImGuiTableColumnFlags_WidthFixed, 150);
        ImGui::TableSetupColumn("值", ImGuiTableColumnFlags_WidthFixed, 150);
        ImGui::TableSetupColumn("偏移", ImGuiTableColumnFlags_WidthFixed, 200);
        ImGui::TableHeadersRow();
        
        int level = 0;
        for (const auto& node : chain) {
            ImGui::TableNextRow();
            
            // 层级
            ImGui::TableNextColumn();
            ImGui::Text("%d", level);
            
            // 地址
            ImGui::TableNextColumn();
            std::string addrStr = formatAddress(node.address);
            if (ImGui::Selectable(addrStr.c_str(), false, ImGuiSelectableFlags_None)) {
                MemoryViewerWindow* viewer = ensureMemoryViewerWindow();
                if (viewer) {
                    viewer->jumpToAddress(node.address);
                    Gui::log("跳转到地址: 0x%llX", node.address);
                }
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("点击跳转到内存查看器");
            }
            
            // 值
            ImGui::TableNextColumn();
            std::string valueStr = formatAddress(node.value);
            ImGui::Text("%s", valueStr.c_str());
            
            // 偏移
            ImGui::TableNextColumn();
            if (level == 0) {
                // 显示静态区域信息
                if (node.staticOffset && node.staticOffset->region) {
                    ImGui::TextColored(ColorScheme::SuccessLight, "%s+0x%llX", 
                                     node.staticOffset->region->name,
                                     node.staticOffset->staticOffset);
                } else {
                    ImGui::TextDisabled("静态");
                }
            } else {
                std::string offsetStr = formatOffset(node.offset);
                ImGui::Text("%s", offsetStr.c_str());
            }
            
            level++;
        }
        
        ImGui::EndTable();
    }
}

void PointerChainWindow::drawPotentialPointersPanel()
{
    ImGui::TextColored(ColorScheme::SuccessLight, "潜在指针数据分析");
    ImGui::Separator();
    
    // 统计信息面板
    ImGui::BeginChild("StatsPanel", ImVec2(0, 80), ImGuiChildFlags_Borders);
    ImGui::Spacing();
    
    // 计算统计信息
    int highFreqCount = 0;
    int midFreqCount = 0;
    int totalRefCount = 0;
    for (auto* ptr : sortedPointers) {
        if (ptr->refCount > 10) highFreqCount++;
        else if (ptr->refCount > 5) midFreqCount++;
        totalRefCount += ptr->refCount;
    }
    float avgRefCount = sortedPointers.empty() ? 0.0f : static_cast<float>(totalRefCount) / sortedPointers.size();
    
    // 第一行：总数和平均引用
    ImGui::Text("  总数:");
    ImGui::SameLine();
    ImGui::TextColored(ColorScheme::SuccessLight, "%d", static_cast<int>(sortedPointers.size()));
    ImGui::SameLine(150);
    ImGui::Text("原始数量:");
    ImGui::SameLine();
    ImGui::TextColored(ColorScheme::InfoLight, "%u", loadedPointerCount);
    ImGui::SameLine(300);
    ImGui::Text("平均引用:");
    ImGui::SameLine();
    ImGui::TextColored(ColorScheme::WarningLight, "%.2f", avgRefCount);
    
    ImGui::Spacing();
    
    // 第二行：高频、中频指针数
    ImGui::Text("  ");
    ImGui::SameLine();
    ImGui::TextColored(ColorScheme::ErrorBright, "●");
    ImGui::SameLine();
    ImGui::Text("高频(>10): %d", highFreqCount);
    ImGui::SameLine(150);
    ImGui::TextColored(ColorScheme::WarningBright, "●");
    ImGui::SameLine();
    ImGui::Text("中频(5-10): %d", midFreqCount);
    ImGui::SameLine(300);
    ImGui::TextColored(ColorScheme::SuccessLight, "●");
    ImGui::SameLine();
    ImGui::Text("低频(<5): %d", static_cast<int>(sortedPointers.size()) - highFreqCount - midFreqCount);
    
    ImGui::Spacing();
    ImGui::EndChild();
    
    ImGui::Spacing();
    
    // 排序和过滤选项
    ImGui::BeginChild("FilterOptions", ImVec2(0, 100), ImGuiChildFlags_Borders);
    
    // 排序选项
    bool needUpdate = false;
    ImGui::Text("  排序:");
    ImGui::SameLine();
    if (ImGui::Checkbox("按引用次数##Sort", &sortByRefCount)) {
        needUpdate = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("勾选后将高频指针排在前面，否则按地址排序");
    }
    
    ImGui::Spacing();
    
    // 过滤选项
    ImGui::Text("  过滤:");
    ImGui::SameLine();
    ImGui::Text("引用≥");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    if (ImGui::InputInt("##MinRefCount", &minRefCountFilter)) {
        if (minRefCountFilter < 0) minRefCountFilter = 0;
        needUpdate = true;
    }
    ImGui::SameLine();
    ImGui::Text("偏移量≥");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    if (ImGui::InputInt("##MinOffsetCount", &minOffsetCountFilter)) {
        if (minOffsetCountFilter < 0) minOffsetCountFilter = 0;
        needUpdate = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("应用", ImVec2(80, 0))) {
        needUpdate = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("应用过滤条件并重新排序");
    }
    ImGui::SameLine();
    if (ImGui::Button("重置", ImVec2(80, 0))) {
        minRefCountFilter = 0;
        minOffsetCountFilter = 0;
        needUpdate = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("清除所有过滤条件");
    }
    
    if (needUpdate) {
        updateSortedPointers();
        pointerDisplayOffset = 0;
    }
    
    ImGui::Spacing();
    ImGui::EndChild();
    
    ImGui::Spacing();
    
    // 分页控制和快捷操作
    ImGui::Text("显示:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    ImGui::InputInt("##PointerDisplayOffset", &pointerDisplayOffset);
    ImGui::SameLine();
    ImGui::Text("-");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    ImGui::InputInt("##PointerDisplayLimit", &pointerDisplayLimit);
    ImGui::SameLine();
    
    // 快捷分页按钮
    if (ImGui::Button("上一页", ImVec2(80, 0))) {
        pointerDisplayOffset -= pointerDisplayLimit;
        if (pointerDisplayOffset < 0) pointerDisplayOffset = 0;
    }
    ImGui::SameLine();
    if (ImGui::Button("下一页", ImVec2(80, 0))) {
        pointerDisplayOffset += pointerDisplayLimit;
        if (pointerDisplayOffset >= static_cast<int>(sortedPointers.size())) {
            pointerDisplayOffset = std::max(0, static_cast<int>(sortedPointers.size()) - pointerDisplayLimit);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("首页", ImVec2(60, 0))) {
        pointerDisplayOffset = 0;
    }
    ImGui::SameLine();
    if (ImGui::Button("末页", ImVec2(60, 0))) {
        pointerDisplayOffset = std::max(0, static_cast<int>(sortedPointers.size()) - pointerDisplayLimit);
    }
    
    // 限制范围
    if (pointerDisplayOffset < 0) pointerDisplayOffset = 0;
    if (pointerDisplayOffset >= static_cast<int>(sortedPointers.size())) 
        pointerDisplayOffset = std::max(0, static_cast<int>(sortedPointers.size()) - 1);
    if (pointerDisplayLimit < 1) pointerDisplayLimit = 1;
    if (pointerDisplayLimit > 500) pointerDisplayLimit = 500;
    
    ImGui::Separator();
    
    // 指针列表
    ImGui::BeginChild("PointerList", ImVec2(0, 0), ImGuiChildFlags_None);
    
    int endIndex = std::min(pointerDisplayOffset + pointerDisplayLimit, static_cast<int>(sortedPointers.size()));
    
    for (int i = pointerDisplayOffset; i < endIndex; i++) {
        auto* pointer = sortedPointers[i];
        
        // 创建可选择的项
        bool isSelected = (selectedPointerIndex == i);
        
        ImGui::PushID(i);
        
        if (ImGui::Selectable(("##pointer" + std::to_string(i)).c_str(), isSelected, 0, ImVec2(0, 0))) {
            selectedPointerIndex = i;
        }
        
        // 显示指针信息
        ImGui::SameLine();
        
        // 索引
        ImGui::TextColored(ColorScheme::TextDisabled, "[%d]", i);
        ImGui::SameLine();
        
        // 频率指示器
        if (pointer->refCount > 10) {
            ImGui::TextColored(ColorScheme::ErrorBright, "●");
        } else if (pointer->refCount > 5) {
            ImGui::TextColored(ColorScheme::WarningBright, "●");
        } else {
            ImGui::TextColored(ColorScheme::SuccessLight, "●");
        }
        ImGui::SameLine();
        
        // 地址（可点击跳转）
        std::string addrStr = formatAddress(pointer->address);
        ImGui::TextColored(ColorScheme::InfoLight, "%s", addrStr.c_str());
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("双击跳转到内存查看器");
            if (ImGui::IsMouseDoubleClicked(0)) {
                MemoryViewerWindow* viewer = ensureMemoryViewerWindow();
                if (viewer) {
                    viewer->jumpToAddress(pointer->address);
                    Gui::log("跳转到地址: 0x%llX", pointer->address);
                }
            }
        }
        ImGui::SameLine();
        
        // 箭头
        ImGui::TextDisabled("→");
        ImGui::SameLine();
        
        // 值（可点击跳转）
        std::string valueStr = formatAddress(pointer->value);
        ImGui::Text("%s", valueStr.c_str());
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("双击跳转到内存查看器");
            if (ImGui::IsMouseDoubleClicked(0)) {
                MemoryViewerWindow* viewer = ensureMemoryViewerWindow();
                if (viewer) {
                    viewer->jumpToAddress(pointer->value);
                    Gui::log("跳转到地址: 0x%llX", pointer->value);
                }
            }
        }
        ImGui::SameLine();
        
        // 引用次数
        ImGui::TextColored(ColorScheme::WarningLight, "[引用:%d]", pointer->refCount);
        ImGui::SameLine();
        
        // 偏移量数量
        ImGui::TextColored(ColorScheme::InfoBright, "[偏移:%d]", static_cast<int>(pointer->Offsets.size()));
        
        // 静态偏移信息（如果有）
        if (pointer->staticOffset_ && pointer->staticOffset_->region) {
            ImGui::SameLine();
            ImGui::TextColored(ColorScheme::SuccessLight, "[%s+0x%llX]", 
                             pointer->staticOffset_->region->name,
                             pointer->staticOffset_->staticOffset);
        }
        
        // 如果被选中，显示详细信息
        if (isSelected) {
            ImGui::Indent(20);
            drawPointerDetail(pointer);
            ImGui::Unindent(20);
        }
        
        ImGui::PopID();
        ImGui::Separator();
    }
    
    ImGui::EndChild();
}

// 开始加载潜在指针
void PointerChainWindow::startLoadPointers()
{
    // 重置指针加载状态
    pointerLoadInProgress = true;
    pointersLoaded = false;
    loadedPointerCount = 0;
    pointerLoadCurrentRegion = 0;
    pointerLoadTotalRegions = 0;
    pointerLoadProgress = 0.0f;
    
    Gui::log("开始获取潜在指针...");
    
    // 启动加载线程
    if (pointerLoadThread.joinable()) {
        pointerLoadThread.join();
    }
    
    pointerLoadThread = std::thread([this]() {
        performLoadPointers();
    });
}

// 取消加载潜在指针
void PointerChainWindow::cancelLoadPointers()
{
    pointerLoadInProgress = false;
    Gui::log("正在取消指针加载...");
}

// 更新指针加载进度
void PointerChainWindow::updateLoadProgress(uint32_t currentRegion, uint32_t totalRegions, float progress)
{
    pointerLoadCurrentRegion = currentRegion;
    pointerLoadTotalRegions = totalRegions;
    pointerLoadProgress = progress;
}

// 执行加载潜在指针
void PointerChainWindow::performLoadPointers()
{
    try {
        // 如果扫描器还不存在，创建一个
        if (!scanner) {
            scanner = std::make_shared<memchainer::PointerScanner>();
        }
        
        // 进度回调
        auto progressCallback = [this](uint32_t currentRegion, uint32_t totalRegions, float progress) {
            updateLoadProgress(currentRegion, totalRegions, progress);
        };
        
        // 调用 findPointers 获取潜在指针（清除已有数据）
        uint32_t pointerCount = scanner->findPointers(true, progressCallback);
        
        if (pointerCount > 0) {
            pointersLoaded = true;
            loadedPointerCount = pointerCount;
            Gui::log("成功加载 %u 个潜在指针", pointerCount);
        } else {
            Gui::log("未找到任何潜在指针");
        }
        
        pointerLoadInProgress = false;
        
    } catch (const std::exception& e) {
        Gui::log("加载指针出错: %s", e.what());
        pointerLoadInProgress = false;
        pointersLoaded = false;
    }
}

void PointerChainWindow::startScan()
{
    // 解析目标地址
    uint64_t targetAddress = 0;
    std::string addrStr = targetAddressBuf;
    
    // 移除 0x 前缀
    if (addrStr.substr(0, 2) == "0x" || addrStr.substr(0, 2) == "0X") {
        addrStr = addrStr.substr(2);
    }
    
    try {
        targetAddress = std::stoull(addrStr, nullptr, 16);
    } catch (...) {
        Gui::log("错误: 无效的目标地址");
        return;
    }
    
    if (targetAddress == 0) {
        Gui::log("错误: 目标地址不能为0");
        return;
    }
    
    // 重置状态
    scanInProgress = true;
    scanCompleted = false;
    scanCancelled = false;
    chains.clear();
    selectedChainIndex = -1;
    
    // 重置进度信息
    scanProgressInfo = memchainer::PointerScanner::ScanProgressInfo();
    scanProgressInfo.maxDepth = maxDepth;
    
    Gui::log("开始扫描指针链，目标地址: 0x%llX", targetAddress);
    
    // 启动扫描线程
    if (scanThread.joinable()) {
        scanThread.join();
    }
    
    scanThread = std::thread([this, targetAddress]() {
        performScan(targetAddress);
    });
}

void PointerChainWindow::cancelScan()
{
    scanCancelled = true;
    Gui::log("正在取消扫描...");
}

void PointerChainWindow::performScan(uint64_t targetAddress)
{
    try {
        // 确保扫描器存在
        if (!scanner) {
            scanner = std::make_shared<memchainer::PointerScanner>();
        }
        
        // 根据选项决定是否重新获取指针
        bool needReload = !useExistingPointers || !pointersLoaded;
        
        if (needReload) {
            // 指针加载进度回调
            auto pointerProgressCallback = [this](uint32_t currentRegion, uint32_t totalRegions, float progress) {
                updateLoadProgress(currentRegion, totalRegions, progress);
            };
            
            // 查找指针（清除已有数据）
            Gui::log("正在查找潜在指针...");
            uint32_t pointerCount = scanner->findPointers(true, pointerProgressCallback);
            
            if (pointerCount == 0) {
                Gui::log("未找到任何潜在指针");
                scanInProgress = false;
                scanCompleted = true;
                return;
            }
            
            // 更新指针加载状态
            pointersLoaded = true;
            loadedPointerCount = pointerCount;
            
            Gui::log("找到 %u 个潜在指针，开始扫描指针链...", pointerCount);
        } else {
            // 使用已有的指针数据
            Gui::log("使用已加载的 %u 个潜在指针，开始扫描指针链...", loadedPointerCount);
        }
        
        // 配置扫描选项
        memchainer::ScanOptions options;
        options.maxDepth = maxDepth;
        options.maxOffset = maxOffset;
        options.limitResults = limitResults;
        options.resultLimit = maxLimit;
        options.threadCount = std::thread::hardware_concurrency();
        
        // 进度回调
        auto progressCallback = [this](const memchainer::PointerScanner::ScanProgressInfo& info) {
            updateProgress(info);
        };
        
        // 使用深度优先搜索扫描指针链
        Gui::log("开始深度优先搜索扫描指针链...");
        int result = scanner->scanPointerChain(targetAddress, options, progressCallback);
        
        if (result == 0) {
            Gui::log("未找到有效的指针链");
        } else {
            Gui::log("找到 %d 条指针链", result);
            
            // 获取结果
            chains = scanner->getChains();
            
            // 更新最终统计
            scanProgressInfo.chainsFound = static_cast<uint32_t>(chains.size());
        }
        
        scanInProgress = false;
        scanCompleted = true;
        
    } catch (const std::exception& e) {
        Gui::log("扫描出错: %s", e.what());
        scanInProgress = false;
        scanCompleted = true;
    }
}

void PointerChainWindow::updateProgress(const memchainer::PointerScanner::ScanProgressInfo& info)
{
    scanProgressInfo = info;
}

void PointerChainWindow::exportToFile()
{
    if (chains.empty()) {
        Gui::log("没有可导出的指针链");
        return;
    }
    
    std::string filename = exportFileBuf;
    if (filename.empty()) {
        filename = "pointer_chains.txt";
    }
    
    try {
        if (formatter->formatToTextFile(chains, filename)) {
            Gui::log("指针链已导出到: %s", filename.c_str());
        } else {
            Gui::log("导出失败");
        }
    } catch (const std::exception& e) {
        Gui::log("导出出错: %s", e.what());
    }
}

std::string PointerChainWindow::formatAddress(uint64_t address)
{
    std::stringstream ss;
    ss << "0x" 
    << std::hex << std::uppercase << std::setfill('0') << std::setw(12) << address;
    return ss.str();
}

std::string PointerChainWindow::formatOffset(int64_t offset)
{
    std::stringstream ss;
    if (offset >= 0) {
        ss << "+0x" << std::hex << std::uppercase << offset;
    } else {
        ss << "-0x" << std::hex << std::uppercase << (-offset);
    }
    return ss.str();
}

std::string PointerChainWindow::formatChainString(const std::list<memchainer::PointerChainNode>& chain)
{
    if (chain.empty()) {
        return "空链";
    }
    
    std::stringstream ss;

    auto it = chain.begin();
    if (it->staticOffset && it->staticOffset->region) {
        ss << "[" << it->staticOffset->region->name << "+0x" 
                   << std::hex << std::uppercase << it->staticOffset->staticOffset << "]";
    } else {
        ss << "[Static]";
    }
    ++it;
    
    for (; it != chain.end(); ++it) {
      
        ss << " -> [" << formatAddress(it->address)<< "]" << formatOffset(it->offset) ;
        
    }
    
    return ss.str();
}

// 确保内存查看器窗口存在
MemoryViewerWindow* PointerChainWindow::ensureMemoryViewerWindow()
{
    // 如果已经存在，直接返回
    if (memoryViewerWindow) {
        return memoryViewerWindow;
    }
    
    // 如果有回调函数，使用回调创建窗口
    if (openMemoryViewerCallback) {
        memoryViewerWindow = openMemoryViewerCallback();
        if (memoryViewerWindow) {
            Gui::log("已自动打开内存查看器窗口");
            return memoryViewerWindow;
        }
    }
    
    // 如果没有回调或回调失败，返回nullptr
    Gui::log("错误: 无法打开内存查看器窗口");
    return nullptr;
} 

// 绘制指针详细信息
void PointerChainWindow::drawPointerDetail(memchainer::PointerAllData* pointer)
{
    if (!pointer) return;
    
    ImGui::TextColored(ColorScheme::SuccessLight, "指针详情:");
    ImGui::Spacing();
    
    // 基本信息表格
    if (ImGui::BeginTable("PointerBasicInfo", 2, 
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg,
        ImVec2(0, 0)))
    {
        ImGui::TableSetupColumn("属性", ImGuiTableColumnFlags_WidthFixed, 150);
        ImGui::TableSetupColumn("值", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        
        // 地址
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("指针地址");
        ImGui::TableNextColumn();
        std::string addrStr = formatAddress(pointer->address);
        if (ImGui::Selectable(addrStr.c_str(), false, ImGuiSelectableFlags_None)) {
            MemoryViewerWindow* viewer = ensureMemoryViewerWindow();
            if (viewer) {
                viewer->jumpToAddress(pointer->address);
                Gui::log("跳转到地址: 0x%llX", pointer->address);
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("点击跳转到内存查看器");
        }
        
        // 值
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("指针值");
        ImGui::TableNextColumn();
        std::string valueStr = formatAddress(pointer->value);
        if (ImGui::Selectable(valueStr.c_str(), false, ImGuiSelectableFlags_None)) {
            MemoryViewerWindow* viewer = ensureMemoryViewerWindow();
            if (viewer) {
                viewer->jumpToAddress(pointer->value);
                Gui::log("跳转到地址: 0x%llX", pointer->value);
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("点击跳转到内存查看器");
        }
        
        // 引用次数
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("引用次数");
        ImGui::TableNextColumn();
        if (pointer->refCount > 10) {
            ImGui::TextColored(ColorScheme::ErrorBright, "%d 次 (高频)", pointer->refCount);
        } else if (pointer->refCount > 5) {
            ImGui::TextColored(ColorScheme::WarningBright, "%d 次 (中频)", pointer->refCount);
        } else {
            ImGui::TextColored(ColorScheme::SuccessLight, "%d 次", pointer->refCount);
        }
        
        // 静态偏移信息
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("静态偏移");
        ImGui::TableNextColumn();
        if (pointer->staticOffset_ && pointer->staticOffset_->region) {
            ImGui::TextColored(ColorScheme::SuccessLight, "%s+0x%llX", 
                             pointer->staticOffset_->region->name,
                             pointer->staticOffset_->staticOffset);
        } else {
            ImGui::TextDisabled("无");
        }
        
        ImGui::EndTable();
    }
    
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    
    // 偏移量列表
    ImGui::TextColored(ColorScheme::InfoLight, "偏移量列表:");
    ImGui::SameLine();
    ImGui::TextDisabled("(共 %d 个)", static_cast<int>(pointer->Offsets.size()));
    
    if (pointer->Offsets.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("  无偏移量数据");
    } else {
        ImGui::BeginChild("OffsetList", ImVec2(0, 150), ImGuiChildFlags_Borders);
        
        // 将偏移量转为vector并排序以便展示
        std::vector<int32_t> offsetVec(pointer->Offsets.begin(), pointer->Offsets.end());
        std::sort(offsetVec.begin(), offsetVec.end());
        
        // 分列显示偏移量（每行5个）
        int column = 0;
        for (int32_t offset : offsetVec) {
            if (column > 0) {
                ImGui::SameLine();
            }
            
            std::string offsetStr = formatOffset(offset);
            ImGui::TextColored(ColorScheme::InfoBright, "%s", offsetStr.c_str());
            
            column++;
            if (column >= 5) {
                column = 0;
            }
        }
        
        ImGui::EndChild();
    }
}

// 更新排序后的指针列表
void PointerChainWindow::updateSortedPointers()
{
    if (!scanner || !pointersLoaded) {
        sortedPointers.clear();
        return;
    }
    
    // 获取指针缓存
    const auto& pointerCache = scanner->getPointerCache();
    
    // 清空并重新填充
    sortedPointers.clear();
    sortedPointers.reserve(pointerCache.size());
    
    // 应用过滤器
    for (auto* pointer : pointerCache) {
        if (!pointer) continue;
        
        // 引用次数过滤
        if (pointer->refCount < minRefCountFilter) {
            continue;
        }
        
        // 偏移量数量过滤
        if (static_cast<int>(pointer->Offsets.size()) < minOffsetCountFilter) {
            continue;
        }
        
        sortedPointers.push_back(pointer);
    }
    
    // 排序
    if (sortByRefCount) {
        // 按引用次数降序排序
        std::sort(sortedPointers.begin(), sortedPointers.end(),
                  [](memchainer::PointerAllData* a, memchainer::PointerAllData* b) {
                      if (a->refCount != b->refCount) {
                          return a->refCount > b->refCount;  // 引用次数高的在前
                      }
                      return a->Offsets.size() > b->Offsets.size();  // 偏移量多的在前
                  });
    } else {
        // 按地址排序
        std::sort(sortedPointers.begin(), sortedPointers.end(),
                  [](memchainer::PointerAllData* a, memchainer::PointerAllData* b) {
                      return a->address < b->address;
                  });
    }
    
    Gui::log("已更新指针列表，过滤后共 %d 个指针", static_cast<int>(sortedPointers.size()));
}