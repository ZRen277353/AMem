#include "ScanWindow.h"
#include "ColorScheme.h"
#include "MemoryViewerWindow.h"
#include "Gui.h"
#include "../imgui/imgui.h"
#include "../socket/client_singleton.h"
#include <algorithm>
#include <sstream>
#include <iomanip>
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
    // 清理扫描线程
    if (scanThread.joinable()) {
        scanCancelled = true;  // 通知扫描线程停止
        scanThread.join();
    }
    
    // 清理刷新线程
    if (scanResultsRefreshThread.joinable()) {
        scanResultsRefreshing = false;
        scanResultsRefreshThread.join();
    }
    if (addressListRefreshThread.joinable()) {
        addressListRefreshing = false;
        addressListRefreshThread.join();
    }
}

void ScanWindow::setProcessInfo(int* pid, std::string* processName)
{
    selectedPid = pid;
    selectedName = processName;
}

void ScanWindow::setMemoryViewerWindow(MemoryViewerWindow* memViewer)
{
    memoryViewerWindow = memViewer;
}

void ScanWindow::setOpenMemoryViewerCallback(std::function<MemoryViewerWindow*()> callback)
{
    openMemoryViewerCallback = callback;
}

void ScanWindow::onDraw()
{
    if (!pOpen) return;

    if (ImGui::Begin(name.c_str(), &pOpen, ImGuiWindowFlags_None))
    {
        if (selectedPid && *selectedPid != 0) {
            ImGui::TextColored(ColorScheme::SuccessBright, "已附加: %s (PID %d)", 
                selectedName ? selectedName->c_str() : "Unknown", *selectedPid);
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

// 静态回调函数，用于扫描进度
static void ScanProgressCallbackStatic(float progress, uint64_t matchCount, uint64_t scannedBytes, uint64_t totalBytes, void* userData)
{
    ScanWindow* window = static_cast<ScanWindow*>(userData);
    if (window) {
        window->updateScanProgress(progress, matchCount, scannedBytes, totalBytes);
    }
}

void ScanWindow::updateScanProgress(float progress, uint64_t matchCount, uint64_t scannedBytes, uint64_t totalBytes)
{
    // 使用互斥锁保护，因为此函数会从扫描线程中调用
    std::lock_guard<std::mutex> lock(scanProgressMutex);
    
    scanProgress = progress;
    scanMatchCount = matchCount;
    scanScannedBytes = scannedBytes;
    scanTotalBytes = totalBytes;
    
    // 使用 std::fixed 格式化进度信息用于日志输出
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1) << (progress * 100.0f) << "%";
    if (totalBytes > 0) {
        oss << " (" << (scannedBytes / (1024 * 1024)) << " / " << (totalBytes / (1024 * 1024)) << " MB)";
    }
    // 可选：输出调试信息
    // Gui::log("扫描进度: %s, 匹配: %llu", oss.str().c_str(), matchCount);
}

// 从CEWindow.cpp移植所有扫描相关的方法
void ScanWindow::drawScanRangeSettings()
{
    ImGui::Text("Memory Types:");
    ImGui::SameLine();
    
    // 显示当前选择状态的简要信息
    if (selectedMemoryTypes == MemoryType::All) {
        ImGui::TextColored(ColorScheme::SuccessLight, "All");
    } else if (selectedMemoryTypes == 0) {
        ImGui::TextColored(ColorScheme::ErrorLight, "None");
    } else {
        // 计算选中的类型数量
        int selectedCount = 0;
        uint32_t tempMask = selectedMemoryTypes;
        while (tempMask) {
            if (tempMask & 1) selectedCount++;
            tempMask >>= 1;
        }
        ImGui::TextColored(ColorScheme::InfoLight, "%d types", selectedCount);
    }
    
    ImGui::SameLine();
    if (ImGui::Button("Select##MemoryTypes")) {
        showMemoryTypeModal = true;
    }
}

void ScanWindow::drawScanProgressBar()
{
    if (scanInProgress) {
        ImGui::Separator();
        ImGui::Text("正在扫描...");
        
        // 显示真实的进度条
        ImVec4 progressColor = ColorScheme::SuccessBright;
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, progressColor);
        
        // 使用 std::fixed 创建进度显示文本
        std::ostringstream progressOss;
        progressOss << std::fixed << std::setprecision(1) << (scanProgress * 100.0f) << "%";
        if (scanTotalBytes > 0) {
            progressOss << " (" << (scanScannedBytes / (1024 * 1024)) 
                       << " / " << (scanTotalBytes / (1024 * 1024)) << " MB)";
        }
        
        ImGui::ProgressBar(scanProgress, ImVec2(-1, 0), progressOss.str().c_str());
        ImGui::PopStyleColor();
        
        // 显示匹配计数和扫描速度
        if (scanMatchCount > 0) {
            ImGui::Text("已找到匹配: %llu 个", scanMatchCount);
        }
        
        // 显示扫描速度和预计剩余时间
        static uint64_t lastScannedBytes = 0;
        static float lastUpdateTime = 0.0f;
        static float scanStartTime = 0.0f;
        
        float currentTime = ImGui::GetTime();
        
        // 记录扫描开始时间
        if (scanProgress == 0.0f || scanStartTime == 0.0f) {
            scanStartTime = currentTime;
            lastUpdateTime = currentTime;
            lastScannedBytes = 0;
        }
        
        // 更新速度显示
        if (currentTime - lastUpdateTime > 0.5f && scanScannedBytes > lastScannedBytes) {
            float timeDelta = currentTime - lastUpdateTime;
            uint64_t bytesDelta = scanScannedBytes - lastScannedBytes;
            float speed = (float)bytesDelta / timeDelta / (1024 * 1024); // MB/s
            
            // 使用 std::fixed 显示速度
            std::ostringstream speedOss;
            speedOss << "扫描速度: " << std::fixed << std::setprecision(1) << speed << " MB/s";
            ImGui::Text("%s", speedOss.str().c_str());
            
            // 计算并显示预计剩余时间
            if (scanProgress > 0.01f && speed > 0.1f) {
                float totalElapsed = currentTime - scanStartTime;
                float estimatedTotal = totalElapsed / scanProgress;
                float estimatedRemaining = estimatedTotal - totalElapsed;
                
                if (estimatedRemaining > 0) {
                    int remainingMinutes = (int)(estimatedRemaining / 60);
                    int remainingSeconds = (int)estimatedRemaining % 60;
                    ImGui::SameLine();
                    ImGui::TextColored(ColorScheme::InfoLight, 
                        "预计剩余: %d:%02d", remainingMinutes, remainingSeconds);
                }
            }
            
            lastScannedBytes = scanScannedBytes;
            lastUpdateTime = currentTime;
        }
        
        ImGui::Text("扫描类型: %s", getScanTypeName(scanType));
        ImGui::Text("数值类型: %s", getValueTypeName(valueType));
        
        // 显示当前内存类型选择
        if (selectedMemoryTypes == MemoryType::All) {
            ImGui::Text("内存范围: 所有类型");
        } else if (selectedMemoryTypes == 0) {
            ImGui::TextColored(ColorScheme::ErrorLight, "内存范围: 未选择");
        } else {
            int selectedCount = 0;
            uint32_t tempMask = selectedMemoryTypes;
            while (tempMask) {
                if (tempMask & 1) selectedCount++;
                tempMask >>= 1;
            }
            ImGui::Text("内存范围: %d 种类型", selectedCount);
        }
        
        // 取消扫描按钮
        ImGui::Spacing();
        if (ImGui::Button("取消扫描", ImVec2(-1, 0))) {
            scanCancelled = true;
            // 使用 PORT_DEBUG 端口发送取消命令到服务端
            if (StopSearchScan(PORT_DEBUG)) {
                Gui::log("用户取消了扫描，已发送停止命令到服务端");
            } else {
                Gui::log("用户取消了扫描，但发送停止命令失败（将等待本地线程结束）");
            }
        }
    } else if (scanCompleted) {
        // 显示扫描完成状态
        ImGui::Separator();
        if (scanCancelled) {
            ImGui::TextColored(ColorScheme::WarningBright, "扫描已取消");
        } else {
            ImGui::TextColored(ColorScheme::SuccessBright, "扫描已完成");
        }
        
        if (totalScanResults > 0) {
            ImGui::Text("找到 %d 个匹配结果", totalScanResults);
            if (scanTotalBytes > 0) {
                ImGui::Text("已扫描 %llu MB 内存", scanTotalBytes / (1024 * 1024));
            }
        } else {
            if (scanCancelled) {
                ImGui::TextColored(ColorScheme::WarningBright, "扫描被中断");
            } else {
                ImGui::TextColored(ColorScheme::WarningBright, "未找到匹配结果");
            }
        }
    }
}

// 数据类型转换方法
std::vector<unsigned char> ScanWindow::parseValueInput(const std::string& input, int type)
{
    std::vector<unsigned char> result;
    
    try {
        switch (type) {
            case 0: { // 1字节
                uint8_t value = hexInput ? std::stoul(input, nullptr, 16) : std::stoul(input);
                result.resize(1);
                result[0] = value;
                break;
            }
            case 1: { // 2字节
                uint16_t value = hexInput ? std::stoul(input, nullptr, 16) : std::stoul(input);
                result.resize(2);
                std::memcpy(result.data(), &value, 2);
                break;
            }
            case 2: { // 4字节
                uint32_t value = hexInput ? std::stoul(input, nullptr, 16) : std::stoul(input);
                result.resize(4);
                std::memcpy(result.data(), &value, 4);
                break;
            }
            case 3: { // 8字节
                uint64_t value = hexInput ? std::stoull(input, nullptr, 16) : std::stoull(input);
                result.resize(8);
                std::memcpy(result.data(), &value, 8);
                break;
            }
            case 4: { // 单精度浮点
                float value = std::stof(input);
                result.resize(4);
                std::memcpy(result.data(), &value, 4);
                break;
            }
            case 5: { // 双精度浮点
                double value = std::stod(input);
                result.resize(8);
                std::memcpy(result.data(), &value, 8);
                break;
            }
        }
    } catch (...) {
        result.clear();
    }
    
    return result;
}

std::string ScanWindow::formatValueOutput(const std::vector<unsigned char>& data, int type)
{
    if (data.empty()) return "无效";
    
    std::ostringstream oss;
    
    switch (type) {
        case 0: { // 1字节
            if (data.size() >= 1) {
                oss << (int)data[0];
            }
            break;
        }
        case 1: { // 2字节
            if (data.size() >= 2) {
                uint16_t value;
                std::memcpy(&value, data.data(), 2);
                oss << value;
            }
            break;
        }
        case 2: { // 4字节
            if (data.size() >= 4) {
                uint32_t value;
                std::memcpy(&value, data.data(), 4);
                oss << value;
            }
            break;
        }
        case 3: { // 8字节
            if (data.size() >= 8) {
                uint64_t value;
                std::memcpy(&value, data.data(), 8);
                oss << value;
            }
            break;
        }
        case 4: { // 单精度浮点
            if (data.size() >= 4) {
                float value;
                std::memcpy(&value, data.data(), 4);
                oss << std::fixed << std::setprecision(6) << value;
            }
            break;
        }
        case 5: { // 双精度浮点
            if (data.size() >= 8) {
                double value;
                std::memcpy(&value, data.data(), 8);
                oss << std::fixed << std::setprecision(10) << value;
            }
            break;
        }
    }
    
    return oss.str();
}

std::string ScanWindow::formatValueOutput(const unsigned char* data, int type)
{
    if (!data) return "无效";
    
    std::ostringstream oss;
    
    switch (type) {
        case 0: { // 1字节
            oss << (int)data[0];
            break;
        }
        case 1: { // 2字节
            uint16_t value;
            std::memcpy(&value, data, 2);
            oss << value;
            break;
        }
        case 2: { // 4字节
            uint32_t value;
            std::memcpy(&value, data, 4);
            oss << value;
            break;
        }
        case 3: { // 8字节
            uint64_t value;
            std::memcpy(&value, data, 8);
            oss << value;
            break;
        }
        case 4: { // 单精度浮点
            float value;
            std::memcpy(&value, data, 4);
            oss << std::fixed << std::setprecision(6) << value;
            break;
        }
        case 5: { // 双精度浮点
            double value;
            std::memcpy(&value, data, 8);
            oss << std::fixed << std::setprecision(10) << value;
            break;
        }
    }
    
    return oss.str();
}

void ScanWindow::drawScanPanel()
{
    ImGui::BeginChild("ScanPanel", ImVec2(300, 0), ImGuiChildFlags_Borders);
    ImGui::Text("扫描设置");
    ImGui::Separator();

    // 根据扫描类型动态显示输入框
    bool needValue1 = true;
    bool needValue2 = false;
    
    // 检查是否需要第二个值（范围搜索或增减值搜索）
    if (scanType == BETWEEN_VAL) {
        needValue2 = true;
    }
    
    // 检查是否不需要输入值（未知值、变动值等）
    if (scanType == UNKNOW_VAL || scanType == ADD_UNKNOW_VAL || 
        scanType == SUB_UNKNOW_VAL || scanType == CHANGED_VAL || 
        scanType == UNCHANGED_VAL) {
        needValue1 = false;
    }

    if (needValue1) {
        const char* valueLabel = "数值:";
        if (scanType == ADD_ACCURATE_VAL) valueLabel = "增加值:";
        else if (scanType == SUB_ACCURATE_VAL) valueLabel = "减少值:";
        else if (scanType == LARGER_THAN_VAL) valueLabel = "大于值:";
        else if (scanType == LESS_THAN_VAL) valueLabel = "小于值:";
        else if (scanType == BETWEEN_VAL) valueLabel = "最小值:";
        
        ImGui::Text("%s", valueLabel);
    ImGui::PushItemWidth(-1);
    ImGui::InputText("##value", valueBuf, IM_ARRAYSIZE(valueBuf));
    ImGui::PopItemWidth();
    }
    
    if (needValue2) {
        ImGui::Text("最大值:");
        ImGui::PushItemWidth(-1);
        ImGui::InputText("##value2", value2Buf, IM_ARRAYSIZE(value2Buf));
        ImGui::PopItemWidth();
    }

    ImGui::Spacing();
    ImGui::Text("扫描类型:");
    ImGui::PushItemWidth(-1);
    const char* scan_types[] = {
        "未知初始值",      // UNKNOW_VAL = 0
        "精确数值",        // ACCURATE_VAL
        "大于数值",        // LARGER_THAN_VAL
        "小于数值",        // LESS_THAN_VAL
        "值在范围内",      // BETWEEN_VAL
        "值增加了未知值",  // ADD_UNKNOW_VAL
        "值增加了精确值",  // ADD_ACCURATE_VAL
        "值减少了未知值",  // SUB_UNKNOW_VAL
        "值减少了精确值",  // SUB_ACCURATE_VAL
        "变动的数值",      // CHANGED_VAL
        "未变动的数值"     // UNCHANGED_VAL
    };
    ImGui::Combo("##scantype", &scanType, scan_types, IM_ARRAYSIZE(scan_types));
    ImGui::PopItemWidth();
    
    // 显示扫描类型说明
    const char* scanTypeHelp = "";
    switch (scanType) {
        case UNKNOW_VAL: scanTypeHelp = "搜索所有内存值，用于首次扫描"; break;
        case ACCURATE_VAL: scanTypeHelp = "搜索与指定值完全相等的内存"; break;
        case LARGER_THAN_VAL: scanTypeHelp = "搜索大于指定值的内存"; break;
        case LESS_THAN_VAL: scanTypeHelp = "搜索小于指定值的内存"; break;
        case BETWEEN_VAL: scanTypeHelp = "搜索在指定范围内的内存值"; break;
        case ADD_UNKNOW_VAL: scanTypeHelp = "搜索值增加了的内存（不知道增加多少）"; break;
        case ADD_ACCURATE_VAL: scanTypeHelp = "搜索值增加了指定数量的内存"; break;
        case SUB_UNKNOW_VAL: scanTypeHelp = "搜索值减少了的内存（不知道减少多少）"; break;
        case SUB_ACCURATE_VAL: scanTypeHelp = "搜索值减少了指定数量的内存"; break;
        case CHANGED_VAL: scanTypeHelp = "搜索值发生改变的内存"; break;
        case UNCHANGED_VAL: scanTypeHelp = "搜索值没有改变的内存"; break;
    }
    
    if (strlen(scanTypeHelp) > 0) {
        ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
        ImGui::TextColored(ColorScheme::TextSecondary, "%s", scanTypeHelp);
        ImGui::PopTextWrapPos();
    }

    ImGui::Text("数值类型:");
    ImGui::PushItemWidth(-1);
    const char* value_types[] = {
        "1字节 (BYTE)",     // _1 = 0
        "2字节 (WORD)",     // _2
        "4字节 (DWORD)",    // _4
        "8字节 (QWORD)",    // _8
        "单精度浮点(FLOAT)",       // _FLOAT
        "双精度浮点(DOUBLE)"        // _DOUBLE
    };
    ImGui::Combo("##valuetype", &valueType, value_types, IM_ARRAYSIZE(value_types));
    ImGui::PopItemWidth();
    
    ImGui::Spacing();
    drawScanRangeSettings();
    
    ImGui::Spacing();
    ImGui::Checkbox("十六进制输入", &hexInput);
    
    ImGui::Spacing();
    ImGui::Separator();
    
    // 根据扫描类型显示适当的按钮
    bool isFirstScan = (totalScanResults == 0);
    bool canUseUnknownValue = (scanType == UNKNOW_VAL || scanType == ADD_UNKNOW_VAL || 
                              scanType == SUB_UNKNOW_VAL || scanType == CHANGED_VAL || 
                              scanType == UNCHANGED_VAL);
    
    // 检查是否可以执行扫描
    bool canScan = (selectedMemoryTypes != 0) && !scanInProgress;
    
    // 扫描按钮区域
    if (isFirstScan) {
        // 首次扫描
        if (!canScan) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("首次扫描", ImVec2(-1, 0))) {
            performFirstScanAsync();  // 使用异步版本
        }
        if (!canScan) {
            ImGui::EndDisabled();
            if (scanInProgress) {
                ImGui::TextColored(ColorScheme::WarningBright, "扫描正在进行中...");
            } else if (selectedMemoryTypes == 0) {
                ImGui::TextColored(ColorScheme::ErrorLight, "请先选择内存类型");
            }
        } else if (!canUseUnknownValue && needValue1) {
            // 检查是否为只能在再次扫描中使用的类型
            if (scanType == ADD_UNKNOW_VAL || scanType == SUB_UNKNOW_VAL || 
                scanType == CHANGED_VAL || scanType == UNCHANGED_VAL) {
                ImGui::TextColored(ColorScheme::ErrorLight, "此扫描类型只能在再次扫描中使用");
            }
        }
    } else {
        // 有结果后的操作
        if (!canScan) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("再次扫描", ImVec2(-1, 0))) {
            performNextScanAsync();  // 使用异步版本
        }
        if (!canScan) {
            ImGui::EndDisabled();
            if (scanInProgress) {
                ImGui::TextColored(ColorScheme::WarningBright, "扫描正在进行中...");
            }
        }
        
        if (scanInProgress) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("新建扫描", ImVec2(-1, 0))) {
            performNewScan();
        }
        if (scanInProgress) {
            ImGui::EndDisabled();
        }
        
        // 结果管理按钮
        if (scanInProgress) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("清空结果", ImVec2(-1, 0))) {
            if (ClearScanResult()) {
                std::lock_guard<std::mutex> lock(scanResultsMutex);
                scanResults.clear();
                selectedScanResults.clear();
                totalScanResults = 0;
                resultOffset = 0;
                Gui::log("已清空所有扫描结果");
            } else {
                Gui::log("清空扫描结果失败");
            }
        }
        if (scanInProgress) {
            ImGui::EndDisabled();
        }
        
        // 移除选中的结果
        int selectedCount = 0;
        {
            std::lock_guard<std::mutex> lock(scanResultsMutex);
            for (char selected : selectedScanResults) {
                if (selected) selectedCount++;
            }
        }
        
        if (selectedCount > 0) {
            if (ImGui::Button("移除选中", ImVec2(-1, 0))) {
                std::vector<uint64_t> addressesToRemove;
                {
                    std::lock_guard<std::mutex> lock(scanResultsMutex);
                    for (int idx = 0; idx < selectedScanResults.size() && idx < scanResults.size(); idx++) {
                        if (selectedScanResults[idx]) {
                            addressesToRemove.push_back(scanResults[idx].address);
                        }
                    }
                }
                
                if (!addressesToRemove.empty() && RemoveScanResult(addressesToRemove)) {
                    Gui::log("已移除 %d 个结果", (int)addressesToRemove.size());
                    // 更新总结果数
                    totalScanResults = GetScanResultCount();
                    loadScanResults(); // 重新加载结果
                } else {
                    Gui::log("移除结果失败");
                }
                
                // 清空选择状态
                {
                    std::lock_guard<std::mutex> lock(scanResultsMutex);
                    std::fill(selectedScanResults.begin(), selectedScanResults.end(), 0);
                }
            }
        }
        
        if (!canScan) {
            ImGui::TextColored(ColorScheme::WarningBright, "警告: 未选择内存类型");
        }
    }

    drawScanProgressBar();
    
    ImGui::Separator();
    
    // 显示扫描结果状态和操作
    ImGui::Separator();
    ImGui::Text("扫描结果:");
    
    if (totalScanResults == 0) {
        if (isFirstScan && !scanCompleted) {
            ImGui::TextDisabled("尚未扫描");
        } else {
            ImGui::TextColored(ColorScheme::ErrorLight, "未找到匹配结果");
        }
    } else {
        ImGui::TextColored(ColorScheme::SuccessBright, "找到: %d 个结果", totalScanResults);
        
        // 根据结果数量显示不同的建议
        if (totalScanResults > 100000) {
            ImGui::TextColored(ColorScheme::ErrorLight, "结果过多，强烈建议继续筛选");
        } else if (totalScanResults > 10000) {
            ImGui::TextColored(ColorScheme::WarningBright, "结果较多，建议继续筛选");
        } else if (totalScanResults > 1000) {
            ImGui::TextColored(ColorScheme::SuccessLight, "结果适中，可以查看或继续筛选");
        } else {
            ImGui::TextColored(ColorScheme::SuccessLight, "结果较少，适合详细分析");
        }
        
        // 结果操作按钮
        if (ImGui::Button("加载结果", ImVec2(-1, 0))) {
            loadScanResults();
        }
        
        if (ImGui::Button("导出结果", ImVec2(-1, 0))) {
            // TODO: 实现导出功能
            Gui::log("导出功能待实现");
        }
        
        if (ImGui::Button("添加选中到地址列表", ImVec2(-1, 0))) {
            // 添加选中的结果到地址列表
            int addCount = 0;
            int selectedCount = 0;
            
            // 先获取 scanResultsMutex，再获取 addressListMutex（固定顺序避免死锁）
            std::lock_guard<std::mutex> scanLock(scanResultsMutex);
            std::lock_guard<std::mutex> addrLock(addressListMutex);
            
            for (int i = 0; i < selectedScanResults.size() && i < scanResults.size(); i++) {
                if (selectedScanResults[i]) {
                    selectedCount++;
                    
                    bool alreadyExists = false;
                    for (const auto& item : addressList) {
                        if (item.address == scanResults[i].address) {
                            alreadyExists = true;
                            break;
                        }
                    }
                    
                    if (!alreadyExists) {
                        AddressListItem newItem;
                        newItem.address = scanResults[i].address;
                        newItem.valueType = valueType;
                        newItem.currentValue = scanResults[i].valueStr;
                        newItem.description = "选中添加 " + std::to_string(addressList.size() + 1);
                        addressList.push_back(newItem);
                        addCount++;
                    }
                }
            }
            
            if (selectedCount == 0) {
                Gui::log("请先选择要添加的结果");
            } else {
                Gui::log("已添加 %d 个地址到列表 (选中了 %d 个)", addCount, selectedCount);
                if (addCount < selectedCount) {
                    Gui::log("注意: %d 个地址已存在于列表中", selectedCount - addCount);
                }
            }
        }
        
        if (ImGui::Button("全部添加到地址列表", ImVec2(-1, 0))) {
            // 添加所有结果到地址列表（限制数量）
            int addCount = 0;
            int maxAdd = 100; // 最多添加100个
            
            // 先获取 scanResultsMutex，再获取 addressListMutex（固定顺序避免死锁）
            std::lock_guard<std::mutex> scanLock(scanResultsMutex);
            std::lock_guard<std::mutex> addrLock(addressListMutex);
            
            for (const auto& result : scanResults) {
                if (addCount >= maxAdd) break;
                
                bool alreadyExists = false;
                for (const auto& item : addressList) {
                    if (item.address == result.address) {
                        alreadyExists = true;
                        break;
                    }
                }
                
                if (!alreadyExists) {
                    AddressListItem newItem;
                    newItem.address = result.address;
                    newItem.valueType = valueType;
                    newItem.currentValue = result.valueStr;
                    newItem.description = "批量添加 " + std::to_string(addressList.size() + 1);
                    addressList.push_back(newItem);
                    addCount++;
                }
            }
            
            Gui::log("已添加 %d 个地址到列表", addCount);
            if (addCount >= maxAdd) {
                Gui::log("注意: 限制最多添加 %d 个地址", maxAdd);
            }
        }
    }

    ImGui::EndChild();
}

void ScanWindow::drawResultsPanel()
{
    ImGui::BeginChild("ScanResults", ImVec2(0, ImGui::GetContentRegionAvail().y * 0.5f), ImGuiChildFlags_Borders);
    
    // 自动刷新控制（使用异步版本，避免卡顿）
    // 注意：扫描进行时不允许刷新，防止Socket操作冲突
    bool shouldRefresh = false;
    {
        std::lock_guard<std::mutex> lock(scanResultsMutex);
        shouldRefresh = autoRefreshScanResults && !scanResults.empty() && !scanResultsRefreshing && !scanInProgress;
    }
    
    if (shouldRefresh) {
        timeSinceScanResultsRefresh += ImGui::GetIO().DeltaTime;
        if (timeSinceScanResultsRefresh >= scanResultsRefreshInterval) {
            refreshScanResultsValuesAsync();  // 使用异步版本
            timeSinceScanResultsRefresh = 0.0f;
        }
    }
    
    // 添加结果显示选项
    static bool showHexValues = false;
    static bool sortByValue = false;
    
    if (totalScanResults > 0) {
        // 刷新控制工具栏
        // 注意：扫描进行时禁用刷新按钮
        if (scanResultsRefreshing || scanInProgress) {
            ImGui::BeginDisabled();
            if (scanInProgress) {
                ImGui::Button("扫描中...");
            } else {
                ImGui::Button("刷新中...");
            }
            ImGui::EndDisabled();
            // 显示进度
            if (refreshTotal > 0 && scanResultsRefreshing) {
                ImGui::SameLine();
                ImGui::Text("(%d/%d)", refreshProgress.load(), refreshTotal.load());
            }
        } else {
            if (ImGui::Button("立即刷新值")) {
                refreshScanResultsValuesAsync();  // 使用异步版本
                timeSinceScanResultsRefresh = 0.0f;
            }
            if (ImGui::IsItemHovered()) {
                int currentSize = 0;
                {
                    std::lock_guard<std::mutex> lock(scanResultsMutex);
                    currentSize = (int)scanResults.size();
                }
                ImGui::SetTooltip("刷新当前页的所有结果值\n只刷新可见的 %d 个结果\n使用异步方式，不会阻塞UI", currentSize);
            }
        }
        
        ImGui::SameLine();
        ImGui::Checkbox("自动刷新", &autoRefreshScanResults);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("启用后将定期自动刷新结果值");
        }
        
        if (autoRefreshScanResults) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(60);
            ImGui::InputFloat("##scanInterval", &scanResultsRefreshInterval, 0, 0, "%.1f");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("自动刷新间隔（秒）\n建议值: 3.0-10.0秒\n默认: 5.0秒");
            }
            ImGui::SameLine();
            ImGui::Text("秒");
        }
        
        ImGui::SameLine();
        ImGui::Separator();
        ImGui::SameLine();
        
        // 批量选择控件
        if (ImGui::Button("全选")) {
            std::lock_guard<std::mutex> lock(scanResultsMutex);
            if (selectedScanResults.size() == scanResults.size()) {
                std::fill(selectedScanResults.begin(), selectedScanResults.end(), 1);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("全不选")) {
            std::lock_guard<std::mutex> lock(scanResultsMutex);
            if (selectedScanResults.size() == scanResults.size()) {
                std::fill(selectedScanResults.begin(), selectedScanResults.end(), 0);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("反选")) {
            std::lock_guard<std::mutex> lock(scanResultsMutex);
            for (auto& selected : selectedScanResults) {
                selected = selected ? 0 : 1;
            }
        }
        
        // 显示选中数量
        int selectedCount = 0;
        {
            std::lock_guard<std::mutex> lock(scanResultsMutex);
            for (char selected : selectedScanResults) {
                if (selected) selectedCount++;
            }
        }
        if (selectedCount > 0) {
            ImGui::SameLine();
            ImGui::TextColored(ColorScheme::SuccessLight, "已选中: %d", selectedCount);
        }
        
        ImGui::Separator();
        
        // 显示选项
        ImGui::Checkbox("十六进制显示", &showHexValues);
        ImGui::SameLine();
        if (ImGui::Checkbox("按数值排序", &sortByValue)) {
            // 如果启用排序，重新加载结果
            loadScanResults();
        }
    }
    
    const char* value_types[] = {
        "1字节", "2字节", "4字节", "8字节", "浮点", "双精度"
    };
    
    if (totalScanResults > 0) {
        int totalPages = (totalScanResults + resultPageSize - 1) / resultPageSize;
        int currentPage = resultOffset / resultPageSize + 1;
        
        ImGui::Text("第 %d 页，共 %d 页 (总共 %d 个结果)", currentPage, totalPages, totalScanResults);
        
        // 显示当前页的数值范围信息
        {
            std::lock_guard<std::mutex> lock(scanResultsMutex);
            if (!scanResults.empty()) {
                uint64_t minVal = UINT64_MAX, maxVal = 0;
                for (const auto& result : scanResults) {
                    uint64_t val = result.value;
                    // 根据数值类型截断
                    switch (valueType) {
                        case 0: val &= 0xFF; break;
                        case 1: val &= 0xFFFF; break;
                        case 2: val &= 0xFFFFFFFF; break;
                        case 3: break; // 8字节不需要截断
                        case 4: case 5: break; // 浮点数单独处理
                    }
                    if (valueType <= 3) { // 整数类型
                        if (val < minVal) minVal = val;
                        if (val > maxVal) maxVal = val;
                    }
                }
                
                if (valueType <= 3 && minVal != UINT64_MAX) {
                    ImGui::SameLine();
                    ImGui::TextColored(ColorScheme::TextSecondary, " | 范围: %llu - %llu", minVal, maxVal);
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("上一页") && resultOffset > 0) {
            resultOffset = (0 > (resultOffset - resultPageSize)) ? 0 : (resultOffset - resultPageSize);
            loadScanResults();
        }
        ImGui::SameLine();
        if (ImGui::Button("下一页") && resultOffset + resultPageSize < totalScanResults) {
            resultOffset = ((totalScanResults - resultPageSize) < (resultOffset + resultPageSize)) ? (totalScanResults - resultPageSize) : (resultOffset + resultPageSize);
            loadScanResults();
        }
        ImGui::Separator();
    }
    
    int columnCount =  4;
    if (ImGui::BeginTable("ResultsTable", columnCount, ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_Sortable))
    {
        ImGui::TableSetupColumn("选择", ImGuiTableColumnFlags_WidthFixed, 50.0f);
        ImGui::TableSetupColumn("地址", ImGuiTableColumnFlags_WidthFixed, 140.0f);
        ImGui::TableSetupColumn("类型", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("数值", ImGuiTableColumnFlags_WidthStretch);
        
        ImGui::TableHeadersRow();
        
        // 使用try_lock尝试获取锁，如果失败则显示上一帧的数据，避免UI阻塞
        std::unique_lock<std::mutex> lock(scanResultsMutex, std::try_to_lock);
        
        if (!lock.owns_lock()) {
            // 无法获取锁（后台正在刷新），显示提示信息
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(1);
            ImGui::TextColored(ColorScheme::WarningBright, "正在刷新数据...");
            ImGui::EndTable();
            ImGui::EndChild();
            return;  // 暂时跳过渲染，等待下一帧
        }
        
        // 在锁内部确保selectedScanResults大小与scanResults匹配
        if (selectedScanResults.size() != scanResults.size()) {
            selectedScanResults.resize(scanResults.size(), 0);
        }
        
        // 保存当前大小，避免在循环中重复访问
        int scanResultsSize = (int)scanResults.size();
        
        // 用于右键菜单的临时存储
        static uint64_t contextMenuAddress = 0;
        static std::string contextMenuValue;
        static int contextMenuValueType = 0;
        
        ImGuiListClipper clipper;
        clipper.Begin(scanResultsSize);
        while (clipper.Step())
        {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd && i < scanResultsSize; i++)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                bool isSelected = selectedScanResults[i] != 0;
                if (ImGui::Checkbox(("##select" + std::to_string(i)).c_str(), &isSelected)) {
                    selectedScanResults[i] = isSelected ? 1 : 0;
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("0x%016llX", scanResults[i].address);
                ImGui::TableSetColumnIndex(2);
                // 根据数值类型显示不同颜色（增强对比度）
                ImVec4 typeColor = ColorScheme::TextPrimary; // 默认白色
                switch (valueType) {
                    case 0: typeColor = ColorScheme::SuccessBright; break; // 1字节（增强可见性）
                    case 1: typeColor = ColorScheme::InfoBright; break; // 2字节（增强可见性）
                    case 2: typeColor = ColorScheme::WarningBright; break; // 4字节（增强可见性）
                    case 3: typeColor = ColorScheme::StatHighlight; break; // 8字节
                    case 4: typeColor = ColorScheme::WarningBright; break; // 浮点（增强可见性）
                    case 5: typeColor = ColorScheme::ErrorBright; break; // 双精度（增强可见性）
                }
                ImGui::TextColored(typeColor, "%s", value_types[valueType]);
                ImGui::TableSetColumnIndex(3);
                
                // 检查值是否最近改变（2秒内）
                bool recentlyChanged = scanResults[i].valueChanged && 
                                      (ImGui::GetTime() - scanResults[i].changeTime < 2.0f);
                
                // 根据显示选项显示数值
                if (recentlyChanged) {
                    // 计算淡出效果
                    float timeSinceChange = ImGui::GetTime() - scanResults[i].changeTime;
                    float alpha = 1.0f - (timeSinceChange / 2.0f);  // 2秒内淡出
                    ImVec4 highlightColor = ColorScheme::WarningBright;  // 高亮
                    highlightColor.w = alpha;
                    
                    if (showHexValues && valueType <= 3) {
                        std::string hexStr = formatScanResultValueHex(scanResults[i].value, valueType);
                        ImGui::TextColored(highlightColor, "%s", hexStr.c_str());
                    } else {
                        ImGui::TextColored(highlightColor, "%s", scanResults[i].valueStr.c_str());
                    }
                    
                    // 添加变化指示器
                    ImGui::SameLine();
                    ImVec4 starColor = ColorScheme::Warning;
                    starColor.w = alpha;
                    ImGui::TextColored(starColor, "*");
                    
                    // 2秒后清除变化标记
                    if (timeSinceChange >= 2.0f) {
                        scanResults[i].valueChanged = false;
                    }
                } else {
                    if (showHexValues && valueType <= 3) {
                        std::string hexStr = formatScanResultValueHex(scanResults[i].value, valueType);
                        ImGui::Text("%s", hexStr.c_str());
                    } else {
                        ImGui::Text("%s", scanResults[i].valueStr.c_str());
                    }
                }
                

                // 双击添加到地址列表
                if (ImGui::IsItemClicked() && ImGui::IsMouseDoubleClicked(0))
                {
                    // 边界检查并复制数据
                    if (i < scanResultsSize && i < scanResults.size()) {
                        uint64_t clickedAddress = scanResults[i].address;
                        std::string clickedValue = scanResults[i].valueStr;
                        int clickedValueType = valueType;
                        
                        // 临时释放 scanResultsMutex，避免死锁
                        lock.unlock();
                        
                        // 需要访问addressList，使用局部锁
                        {
                            std::lock_guard<std::mutex> addrLock(addressListMutex);
                            
                            bool alreadyExists = false;
                            for (const auto& item : addressList) {
                                if (item.address == clickedAddress) {
                                    alreadyExists = true;
                                    break;
                                }
                            }
                            if (!alreadyExists) {
                                AddressListItem newItem;
                                newItem.address = clickedAddress;
                                newItem.valueType = clickedValueType;
                                newItem.currentValue = clickedValue;
                                newItem.description = "地址 " + std::to_string(addressList.size() + 1);
                                addressList.push_back(newItem);
                                Gui::log("已添加地址: 0x%016llX (%s)", newItem.address, newItem.currentValue.c_str());
                            } else {
                                Gui::log("地址已存在于列表中");
                            }
                        }
                        
                        // 重新获取 scanResultsMutex
                        lock.lock();
                        
                        // 重新检查边界，因为在释放锁期间 scanResults 可能已被修改
                        if (!lock.owns_lock() || i >= (int)scanResults.size()) {
                            break;  // 退出循环
                        }
                    }
                }
                
                // 右键菜单
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                    // 保存选中项的数据，避免在菜单显示时访问可能已失效的 vector
                    if (i < scanResultsSize && i < scanResults.size()) {
                        contextMenuAddress = scanResults[i].address;
                        contextMenuValue = scanResults[i].valueStr;
                        contextMenuValueType = valueType;
                        ImGui::OpenPopup("ResultContext");
                    }
                }
                
                // 使用固定的 popup ID，从存储的数据中读取
                if (ImGui::BeginPopup("ResultContext")) {
                    ImGui::Text("地址: 0x%016llX", contextMenuAddress);
                    ImGui::Text("数值: %s", contextMenuValue.c_str());
                    ImGui::Separator();
                    
                    if (ImGui::MenuItem("添加到地址列表")) {
                        std::lock_guard<std::mutex> addrLock(addressListMutex);
                        bool alreadyExists = false;
                        for (const auto& item : addressList) {
                            if (item.address == contextMenuAddress) {
                                alreadyExists = true;
                                break;
                            }
                        }
                        if (!alreadyExists) {
                            AddressListItem newItem;
                            newItem.address = contextMenuAddress;
                            newItem.valueType = contextMenuValueType;
                            newItem.currentValue = contextMenuValue;
                            newItem.description = "地址 " + std::to_string(addressList.size() + 1);
                            addressList.push_back(newItem);
                            Gui::log("已添加地址: 0x%016llX", newItem.address);
                        } else {
                            Gui::log("地址已存在于列表中");
                        }
                    }
                    
                    if (ImGui::MenuItem("复制地址")) {
                        char addrStr[32];
                        sprintf(addrStr, "%016llX", contextMenuAddress);
                        ImGui::SetClipboardText(addrStr);
                        Gui::log("已复制地址: 0x%016llX", contextMenuAddress);
                    }
                    
                    if (ImGui::MenuItem("复制数值")) {
                        ImGui::SetClipboardText(contextMenuValue.c_str());
                        Gui::log("已复制数值: %s", contextMenuValue.c_str());
                    }
                    
                    if (ImGui::MenuItem("在内存查看器中打开")) {
                        // 临时释放锁，避免死锁
                        lock.unlock();
                        
                        MemoryViewerWindow* viewer = ensureMemoryViewerWindow();
                        if (viewer) {
                            viewer->jumpToAddress(contextMenuAddress);
                        }
                        
                        // 重新获取锁
                        lock.lock();
                    }
                    
                    ImGui::EndPopup();
                }
            }
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

// 扫描功能实现
void ScanWindow::performFirstScan()
{
    // 验证扫描参数
    if (!validateScanParameters(true)) {
        return;
    }
    
    // 准备数值字节
    std::vector<unsigned char> valueBytes;
    if (!prepareValueBytes(valueBytes, true)) {
        return;
    }
    
    uint32_t flags = getScanTypeFlag() | getValueTypeFlag();
    uint32_t memoryTypeFlags = selectedMemoryTypes;
    
    // 设置内存类型范围
    if (!ScanSetRange(memoryTypeFlags)) {
        Gui::log("设置扫描范围失败");
        return;
    }
    
    // 重置进度状态
    scanInProgress = true;
    scanCompleted = false;
    scanError = false;
    scanCancelled = false;
    scanProgress = 0.0f;
    scanMatchCount = 0;
    scanScannedBytes = 0;
    scanTotalBytes = 0;
    
    Gui::log("开始首次扫描...");
    Gui::log("扫描类型: %s", getScanTypeName(scanType));
    Gui::log("数值类型: %s", getValueTypeName(valueType));
    Gui::log("内存类型标志: 0x%X", memoryTypeFlags);
    
    // 检查是否需要显示搜索值
    bool needValue1 = !(scanType == UNKNOW_VAL || scanType == ADD_UNKNOW_VAL || 
                       scanType == SUB_UNKNOW_VAL || scanType == CHANGED_VAL || 
                       scanType == UNCHANGED_VAL);
    bool needValue2 = (scanType == BETWEEN_VAL);
    
    if (needValue1) {
        Gui::log("搜索值: %s", valueBuf);
        if (needValue2) {
            Gui::log("范围: %s - %s", valueBuf, value2Buf);
        }
    }
    
    // 执行扫描（带进度回调的异步调用）
    int newResultCount = 0;
    if (scanType == UNKNOW_VAL) {//模糊扫描
        newResultCount = ScanFuzzyValueWithProgress(getValueTypeFlag(), ScanProgressCallbackStatic, this);
    } else {//精确扫描
        newResultCount = ScanValueWithProgress(flags, valueBytes, ScanProgressCallbackStatic, this);
    }
    
    scanInProgress = false;
    scanCompleted = true;
    
    if (scanCancelled) {
        Gui::log("扫描已被取消");
        totalScanResults = GetScanResultCount(); // 获取当前实际结果数
        if (totalScanResults > 0) {
            resultOffset = 0;  // 重置偏移量
            loadScanResults();
        }
    } else if (newResultCount >= 0) {
        totalScanResults = newResultCount;
        if (totalScanResults > 0) {
            Gui::log("首次扫描完成，找到 %d 个结果", totalScanResults);
            resultOffset = 0;  // 重置偏移量
            loadScanResults();
        } else {
            Gui::log("扫描完成，未找到匹配结果");
            if (selectedMemoryTypes == 0) {
                Gui::log("警告: 未选择任何内存类型");
            }
            // 清空显示的结果
            {
                std::lock_guard<std::mutex> lock(scanResultsMutex);
                scanResults.clear();
                selectedScanResults.clear();
            }
            resultOffset = 0;
        }
    } else {
        scanError = true;
        Gui::log("扫描过程中发生错误");
        totalScanResults = 0;
        {
            std::lock_guard<std::mutex> lock(scanResultsMutex);
            scanResults.clear();
            selectedScanResults.clear();
        }
        resultOffset = 0;
    }
}

// 异步版本的首次扫描
void ScanWindow::performFirstScanAsync()
{
    // 如果已经在扫描，不启动新的扫描
    if (scanInProgress) {
        Gui::log("扫描已在进行中，请等待完成");
        return;
    }
    
    // 等待之前的扫描线程完成
    if (scanThread.joinable()) {
        scanThread.join();
    }
    
    // 在主线程中进行参数验证和准备
    if (!validateScanParameters(true)) {
        return;
    }
    
    std::vector<unsigned char> valueBytes;
    if (!prepareValueBytes(valueBytes, true)) {
        return;
    }
    
    uint32_t flags = getScanTypeFlag() | getValueTypeFlag();
    uint32_t memoryTypeFlags = selectedMemoryTypes;
    int currentScanType = scanType;
    
    // 设置内存类型范围
    if (!ScanSetRange(memoryTypeFlags)) {
        Gui::log("设置扫描范围失败");
        return;
    }
    
    // 重置进度状态
    scanInProgress = true;
    scanCompleted = false;
    scanError = false;
    scanCancelled = false;
    scanProgress = 0.0f;
    scanMatchCount = 0;
    scanScannedBytes = 0;
    scanTotalBytes = 0;
    
    Gui::log("开始首次扫描（异步）...");
    Gui::log("扫描类型: %s", getScanTypeName(currentScanType));
    Gui::log("数值类型: %s", getValueTypeName(valueType));
    Gui::log("内存类型标志: 0x%X", memoryTypeFlags);
    
    // 启动异步扫描线程
    scanThread = std::thread([this, flags, valueBytes, currentScanType]() mutable {
        int newResultCount = 0;
        
        // 执行扫描（带进度回调）
        if (currentScanType == UNKNOW_VAL) {
            newResultCount = ScanFuzzyValueWithProgress(getValueTypeFlag(), ScanProgressCallbackStatic, this);
        } else {
            // lambda 内部的 valueBytes 现在是可变的
            newResultCount = ScanValueWithProgress(flags, valueBytes, ScanProgressCallbackStatic, this);
        }
        
        // 扫描完成，更新状态
        scanInProgress = false;
        scanCompleted = true;
        
        if (scanCancelled) {
            Gui::log("扫描已被取消");
            totalScanResults = GetScanResultCount();
            if (totalScanResults > 0) {
                resultOffset = 0;  // 重置偏移量
                loadScanResults();
            }
        } else if (newResultCount >= 0) {
            totalScanResults = newResultCount;
            if (totalScanResults > 0) {
                Gui::log("首次扫描完成，找到 %d 个结果", totalScanResults);
                resultOffset = 0;  // 重置偏移量
                loadScanResults();
            } else {
                Gui::log("扫描完成，未找到匹配结果");
                std::lock_guard<std::mutex> lock(scanResultsMutex);
                scanResults.clear();
                selectedScanResults.clear();
                resultOffset = 0;
            }
        } else {
            scanError = true;
            Gui::log("扫描过程中发生错误");
            totalScanResults = 0;
            std::lock_guard<std::mutex> lock(scanResultsMutex);
            scanResults.clear();
            selectedScanResults.clear();
            resultOffset = 0;
        }
    });
}

void ScanWindow::performNextScan()
{
    // 验证扫描参数
    if (!validateScanParameters(false)) {
        return;
    }
    
    // 准备数值字节
    std::vector<unsigned char> valueBytes;
    if (!prepareValueBytes(valueBytes, false)) {
        return;
    }
    
    uint32_t flags = getScanTypeFlag() | getValueTypeFlag();
    
    // 重置进度状态
    scanInProgress = true;
    scanCompleted = false;
    scanError = false;
    scanCancelled = false;
    scanProgress = 0.0f;
    scanMatchCount = 0;
    scanScannedBytes = 0;
    scanTotalBytes = 0;
    
    Gui::log("开始再次扫描...");
    Gui::log("扫描类型: %s", getScanTypeName(scanType));
    Gui::log("数值类型: %s", getValueTypeName(valueType));
    Gui::log("当前结果数: %d", totalScanResults);
    
    // 检查是否需要显示搜索值
    bool needValue1 = !(scanType == ADD_UNKNOW_VAL || scanType == SUB_UNKNOW_VAL || 
                       scanType == CHANGED_VAL || scanType == UNCHANGED_VAL);
    bool needValue2 = (scanType == BETWEEN_VAL);
    
    if (needValue1) {
        Gui::log("搜索值: %s", valueBuf);
        if (needValue2) {
            Gui::log("范围: %s - %s", valueBuf, value2Buf);
        }
    }
    
    // 执行再次扫描（带进度回调的异步调用）
    int newResultCount = ScanNextValueWithProgress(valueBytes, flags, ScanProgressCallbackStatic, this);
    
    scanInProgress = false;
    scanCompleted = true;
    
    if (scanCancelled) {
        Gui::log("扫描已被取消");
        totalScanResults = GetScanResultCount(); // 获取当前实际结果数
        if (totalScanResults > 0) {
            resultOffset = 0;  // 重置偏移量
            loadScanResults();
        }
    } else if (newResultCount >= 0) {
        totalScanResults = newResultCount;
        if (totalScanResults > 0) {
            Gui::log("再次扫描完成，找到 %d 个结果", totalScanResults);
            resultOffset = 0;  // 重置偏移量
            loadScanResults();
        } else {
            Gui::log("扫描完成，未找到匹配结果");
            // 清空显示的结果
            {
                std::lock_guard<std::mutex> lock(scanResultsMutex);
                scanResults.clear();
                selectedScanResults.clear();
            }
            resultOffset = 0;
        }
    } else {
        scanError = true;
        Gui::log("扫描过程中发生错误");
    }
}

// 异步版本的再次扫描
void ScanWindow::performNextScanAsync()
{
    // 如果已经在扫描，不启动新的扫描
    if (scanInProgress) {
        Gui::log("扫描已在进行中，请等待完成");
        return;
    }
    
    // 等待之前的扫描线程完成
    if (scanThread.joinable()) {
        scanThread.join();
    }
    
    // 在主线程中进行参数验证和准备
    if (!validateScanParameters(false)) {
        return;
    }
    
    std::vector<unsigned char> valueBytes;
    if (!prepareValueBytes(valueBytes, false)) {
        return;
    }
    
    uint32_t flags = getScanTypeFlag() | getValueTypeFlag();
    int currentScanType = scanType;
    int currentValueType = valueType;
    
    // 重置进度状态
    scanInProgress = true;
    scanCompleted = false;
    scanError = false;
    scanCancelled = false;
    scanProgress = 0.0f;
    scanMatchCount = 0;
    scanScannedBytes = 0;
    scanTotalBytes = 0;
    
    Gui::log("开始再次扫描（异步）...");
    Gui::log("扫描类型: %s", getScanTypeName(currentScanType));
    Gui::log("数值类型: %s", getValueTypeName(currentValueType));
    Gui::log("当前结果数: %d", totalScanResults);
    
    // 启动异步扫描线程
    scanThread = std::thread([this, flags, valueBytes]() mutable {
        // 执行再次扫描（带进度回调）
        // lambda 内部的 valueBytes 现在是可变的
        int newResultCount = ScanNextValueWithProgress(valueBytes, flags, ScanProgressCallbackStatic, this);
        
        // 扫描完成，更新状态
        scanInProgress = false;
        scanCompleted = true;
        
        if (scanCancelled) {
            Gui::log("扫描已被取消");
            totalScanResults = GetScanResultCount();
            if (totalScanResults > 0) {
                resultOffset = 0;  // 重置偏移量
                loadScanResults();
            }
        } else if (newResultCount >= 0) {
            totalScanResults = newResultCount;
            if (totalScanResults > 0) {
                Gui::log("再次扫描完成，找到 %d 个结果", totalScanResults);
                resultOffset = 0;  // 重置偏移量
                loadScanResults();
            } else {
                Gui::log("扫描完成，未找到匹配结果");
                std::lock_guard<std::mutex> lock(scanResultsMutex);
                scanResults.clear();
                selectedScanResults.clear();
                resultOffset = 0;
            }
        } else {
            scanError = true;
            Gui::log("扫描过程中发生错误");
        }
    });
}

void ScanWindow::performNewScan()
{
    // 清除之前的扫描结果
    if (!ClearScanResult()) {
        Gui::log("清除扫描结果失败");
    }
    
    {
        std::lock_guard<std::mutex> lock(scanResultsMutex);
        scanResults.clear();
        selectedScanResults.clear();
    }
    
    totalScanResults = 0;
    resultOffset = 0;
    
    // 重置进度状态
    scanProgress = 0.0f;
    scanMatchCount = 0;
    scanScannedBytes = 0;
    scanTotalBytes = 0;
    scanCompleted = false;
    scanError = false;
    scanCancelled = false;
    
    Gui::log("已清除扫描结果，准备新建扫描");
}

void ScanWindow::loadScanResults()
{
    if (totalScanResults == 0) return;
    
    int count = (resultPageSize < (totalScanResults - resultOffset)) ? resultPageSize : (totalScanResults - resultOffset);
    if (count <= 0) return;  // 防止无效的count值
    
    std::vector<std::pair<uint64_t, uint64_t>> rawResults;
    // 不预先resize，让GetScanResult根据实际返回的数量来设置大小
    // rawResults.clear();
    rawResults.reserve(count);
    Gui::log("loadScanResults: resultOffset %d, count %d", resultOffset, count);
    
    if (GetScanResult(resultOffset, count, rawResults)) {
        // 使用互斥锁保护，因为此函数可能从扫描线程中调用
        std::lock_guard<std::mutex> lock(scanResultsMutex);
        
        scanResults.clear();
        scanResults.reserve(rawResults.size());
        
        for (const auto& result : rawResults) {
            ScanResultItem item;
            item.address = result.first;
            item.value = result.second; // 保持原始值
            item.previousValue = result.second;  // 初始化previousValue，避免首次刷新误报变化
            item.valueChanged = false;
            item.changeTime = 0.0f;
            
            // 根据数值类型正确截断和格式化数据
            item.valueStr = formatScanResultValue(result.second, valueType);
            scanResults.push_back(item);
        }
        
        // 重置选择状态
        selectedScanResults.clear();
        selectedScanResults.resize(scanResults.size(), 0);
        
        Gui::log("已加载 %d 个扫描结果 (偏移: %d)", (int)scanResults.size(), resultOffset);
    } else {
        Gui::log("加载扫描结果失败");
    }
}

void ScanWindow::drawAddressListPanel()
{
    const char* value_types[] = {
        "1字节", "2字节", "4字节", "8字节", "浮点", "双精度"
    };
    ImGui::BeginChild("AddressList", ImVec2(0, 0), ImGuiChildFlags_Borders);
    
    // 自动刷新控制（使用异步版本，避免卡顿）
    // 注意：扫描进行时不允许刷新，防止Socket操作冲突
    if (autoRefreshAddressList && !addressList.empty() && !addressListRefreshing && !scanInProgress) {
        timeSinceAddressListRefresh += ImGui::GetIO().DeltaTime;
        if (timeSinceAddressListRefresh >= addressListRefreshInterval) {
            refreshAddressValuesAsync();  // 使用异步版本
            timeSinceAddressListRefresh = 0.0f;
        }
    }
    
    // 工具栏
    // 注意：扫描进行时禁用刷新按钮
    if (addressListRefreshing || scanInProgress) {
        ImGui::BeginDisabled();
        if (scanInProgress) {
            ImGui::Button("扫描中...");
        } else {
            ImGui::Button("刷新中...");
        }
        ImGui::EndDisabled();
        if (refreshTotal > 0 && addressListRefreshing) {
            ImGui::SameLine();
            ImGui::Text("(%d/%d)", refreshProgress.load(), refreshTotal.load());
        }
    } else {
        if (ImGui::Button("立即刷新")) {
            refreshAddressValuesAsync();  // 使用异步版本
            timeSinceAddressListRefresh = 0.0f;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("手动刷新所有地址的值\n使用异步方式，不会阻塞UI");
        }
    }
    
    ImGui::SameLine();
    ImGui::Checkbox("自动刷新", &autoRefreshAddressList);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("启用后将定期自动刷新地址列表");
    }
    
    if (autoRefreshAddressList) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(60);
        ImGui::InputFloat("##interval", &addressListRefreshInterval, 0, 0, "%.1f");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("自动刷新间隔（秒）\n建议值: 0.5-2.0秒");
        }
        ImGui::SameLine();
        ImGui::Text("秒");
    }
    
    ImGui::SameLine();
    if (ImGui::Button("删除选中")) {
        std::lock_guard<std::mutex> lock(addressListMutex);
        // 修复：删除已勾选的项目（active == true 表示被选中要删除）
        int removedCount = 0;
        auto it = std::remove_if(addressList.begin(), addressList.end(),
            [&removedCount](const AddressListItem& item) { 
                if (item.active) {
                    removedCount++;
                    return true;
                }
                return false;
            });
        addressList.erase(it, addressList.end());
        if (removedCount > 0) {
            Gui::log("已删除 %d 个地址", removedCount);
        }
    }
    
    if (ImGui::BeginTable("AddressTable", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersOuter))
    {
        ImGui::TableSetupColumn("选中", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("描述", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("地址", ImGuiTableColumnFlags_WidthFixed, 160.0f);
        ImGui::TableSetupColumn("类型", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("数值", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableHeadersRow();

        // 使用try_lock尝试获取锁，避免UI阻塞
        std::unique_lock<std::mutex> lock(addressListMutex, std::try_to_lock);
        
        if (!lock.owns_lock()) {
            // 无法获取锁（后台正在刷新），跳过本帧渲染
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(1);
            ImGui::TextColored(ColorScheme::WarningBright, "正在刷新，请稍候...");
            ImGui::EndTable();
            ImGui::EndChild();
            return;  // 直接返回，等待下一帧
        }
        
        // 已获得锁，安全访问 addressList
        int addressListSize = (int)addressList.size();
        
        for (int i = 0; i < addressListSize; i++)
        {
            // 边界检查，防止在循环中 addressList 被修改
            if (i >= (int)addressList.size()) break;
            
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Checkbox((std::string("##active") + std::to_string(i)).c_str(), &addressList[i].active);
            ImGui::TableSetColumnIndex(1);
            static char descBuf[256];
            strncpy(descBuf, addressList[i].description.c_str(), sizeof(descBuf) - 1);
            descBuf[sizeof(descBuf) - 1] = '\0';
            if (ImGui::InputText((std::string("##desc") + std::to_string(i)).c_str(), descBuf, sizeof(descBuf))) {
                addressList[i].description = descBuf;
            }
            ImGui::TableSetColumnIndex(2);
            
            // 先复制地址，避免在 jumpToAddress 调用期间访问可能失效的引用
            uint64_t itemAddress = addressList[i].address;
            
            char addrStr[32];
            std::snprintf(addrStr, sizeof(addrStr), "0x%016llX", itemAddress);
            if (ImGui::Selectable(addrStr, false, ImGuiSelectableFlags_None)) {
                // 临时释放锁，避免死锁（ensureMemoryViewerWindow 可能需要其他锁）
                lock.unlock();
                
                MemoryViewerWindow* viewer = ensureMemoryViewerWindow();
                if (viewer) {
                    viewer->jumpToAddress(itemAddress);
                }
                
                // 重新获取锁以继续循环（如果失败则退出）
                lock.lock();
                if (!lock.owns_lock()) {
                    break;
                }
                
                // 重新检查边界，因为在释放锁期间 addressList 可能已被修改
                if (i >= (int)addressList.size()) break;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("点击跳转到内存查看器");
            }
            // 右键菜单 - 使用唯一ID避免断言失败
            char addr_popup_id[64];
            snprintf(addr_popup_id, sizeof(addr_popup_id), "AddrPopup_%d", i);
            if (ImGui::BeginPopupContextItem(addr_popup_id)) {
                // 边界检查
                if (i < (int)addressList.size()) {
                    if (ImGui::MenuItem("复制地址")) {
                        ImGui::SetClipboardText(addrStr);
                    }
                    if (ImGui::MenuItem("在内存查看器中打开")) {
                        // 临时释放锁
                        lock.unlock();
                        
                        MemoryViewerWindow* viewer = ensureMemoryViewerWindow();
                        if (viewer) {
                            viewer->jumpToAddress(itemAddress);
                        }
                        
                        // 重新获取锁
                        lock.lock();
                    }
                }
                ImGui::EndPopup();
            }
            
            // 边界检查
            if (i >= (int)addressList.size()) break;
            
            ImGui::TableSetColumnIndex(3);
            // 根据数值类型显示不同颜色（增强对比度）
            ImVec4 typeColor = ColorScheme::TextPrimary; // 默认白色
            // 先复制值，避免后续访问时 addressList 被修改导致段错误
            int itemValueType = (i < (int)addressList.size()) ? addressList[i].valueType : 0;
            switch (itemValueType) {
                case 0: typeColor = ColorScheme::SuccessBright; break; // 1字节
                case 1: typeColor = ColorScheme::InfoBright; break; // 2字节
                case 2: typeColor = ColorScheme::WarningBright; break; // 4字节
                case 3: typeColor = ColorScheme::StatHighlight; break; // 8字节
                case 4: typeColor = ColorScheme::WarningBright; break; // 浮点
                case 5: typeColor = ColorScheme::ErrorBright; break; // 双精度
            }
            ImGui::TextColored(typeColor, "%s", value_types[itemValueType]);
            ImGui::TableSetColumnIndex(4);
            
            // 复制当前值字符串，避免引用失效
            std::string currentValue = addressList[i].currentValue;
            
            // 可编辑的数值显示
            static char editBuf[64];
            std::snprintf(editBuf, sizeof(editBuf), "%s", currentValue.c_str());
            
            ImGui::PushItemWidth(-1);
            if (ImGui::InputText(("##value" + std::to_string(i)).c_str(), editBuf, sizeof(editBuf), ImGuiInputTextFlags_EnterReturnsTrue)) {
                // 写入新值到内存
                if (i < (int)addressList.size() && writeAddressValue(i, editBuf)) {
                    Gui::log("成功写入地址 0x%016llX 的值: %s", itemAddress, editBuf);
                    // 写入成功后刷新当前地址以验证
                    if (i < (int)addressList.size()) {
                        refreshSingleAddress(i);
                    }
                } else {
                    Gui::log("写入失败：地址 0x%016llX", itemAddress);
                }
            }
            ImGui::PopItemWidth();
            
            // 显示工具提示
            if (ImGui::IsItemHovered()) {
                // 确保索引有效后再访问
                if (i < (int)addressList.size()) {
                    ImGui::SetTooltip("按回车键确认修改\n地址: 0x%016llX\n类型: %s", 
                        itemAddress, value_types[itemValueType]);
                } else {
                    ImGui::SetTooltip("按回车键确认修改\n地址: 0x%016llX", itemAddress);
                }
            }
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

void ScanWindow::refreshAddressValues()
{
    // 检查是否有进程附加
    if (!selectedPid || *selectedPid == 0) {
        std::lock_guard<std::mutex> lock(addressListMutex);
        for (auto& item : addressList) {
            item.currentValue = "N/A";
        }
        return;
    }
    
    // 【优化策略】三步走，最小化锁持有时间
    struct AddressRefreshData {
        size_t index;
        uint64_t address;
        int valueType;
        std::string newValue;
        bool success;
    };
    
    std::vector<AddressRefreshData> refreshData;
    
    // 第一步：快速复制地址信息（锁持有时间 < 1ms）
    {
        std::lock_guard<std::mutex> lock(addressListMutex);
        
        if (addressList.empty()) {
            return;
        }
        
        refreshProgress = 0;
        refreshTotal = addressList.size();
        
        refreshData.reserve(addressList.size());
        for (size_t i = 0; i < addressList.size(); i++) {
            AddressRefreshData data;
            data.index = i;
            data.address = addressList[i].address;
            data.valueType = addressList[i].valueType;
            data.success = false;
            refreshData.push_back(data);
        }
    }  // 立即释放锁
    
    // 第二步：使用批量读取优化内存访问（大幅提升性能）
    // 准备批量读取的地址列表
    std::vector<std::pair<uint64_t, int32_t>> batchAddrs;
    batchAddrs.reserve(refreshData.size());
    
    for (auto& data : refreshData) {
        uint32_t size = 0;
        switch (data.valueType) {
            case 0: size = 1; break;
            case 1: size = 2; break;
            case 2: size = 4; break;
            case 3: size = 8; break;
            case 4: size = 4; break;
            case 5: size = 8; break;
            default: size = 4; break;
        }
        batchAddrs.push_back({data.address, size});
    }
    
    // 批量读取所有地址（使用调试端口进行自动刷新）
    std::vector<std::pair<uint64_t, std::vector<uint8_t>>> batchResults;
    bool batchSuccess = ReadBratchAddr(batchAddrs, batchResults, PORT_DEBUG);
    
    if (batchSuccess && batchResults.size() == refreshData.size()) {
        // 批量读取成功，解析结果
        for (size_t i = 0; i < refreshData.size(); i++) {
            auto& data = refreshData[i];
            auto& result = batchResults[i];
            
            if (result.second.size() >= batchAddrs[i].second) {
                data.newValue = formatValueOutput(result.second.data(), data.valueType);
                data.success = true;
            } else {
                data.newValue = "读取失败";
                data.success = false;
            }
            refreshProgress++;
        }
    } else {
        // 批量读取失败，回退到逐个读取
        for (auto& data : refreshData) {
            std::vector<unsigned char> buffer;
            uint32_t size = 0;
            
            switch (data.valueType) {
                case 0: size = 1; break;
                case 1: size = 2; break;
                case 2: size = 4; break;
                case 3: size = 8; break;
                case 4: size = 4; break;
                case 5: size = 8; break;
                default: size = 4; break;
            }
            
            if (ReadProcessMemoryBytes(data.address, size, buffer, PORT_DEBUG) && buffer.size() >= size) {
                data.newValue = formatValueOutput(buffer.data(), data.valueType);
                data.success = true;
            } else {
                data.newValue = "读取失败";
                data.success = false;
            }
            
            refreshProgress++;
        }
    }
    
    // 第三步：快速更新结果（锁持有时间 < 5ms）
    {
        std::lock_guard<std::mutex> lock(addressListMutex);
        
        for (const auto& data : refreshData) {
            if (data.index >= addressList.size()) continue;
            addressList[data.index].currentValue = data.newValue;
        }
    }  // 快速释放锁
}

void ScanWindow::refreshAddressValuesAsync()
{
    // 双重检查：如果正在扫描，不允许刷新（防止Socket操作冲突）
    if (scanInProgress) {
        return;
    }
    
    // 如果已经在刷新，不启动新的刷新
    if (addressListRefreshing) {
        return;
    }
    
    // 等待之前的线程完成
    if (addressListRefreshThread.joinable()) {
        addressListRefreshThread.join();
    }
    
    // 标记为正在刷新
    addressListRefreshing = true;
    
    // 启动异步刷新线程
    addressListRefreshThread = std::thread([this]() {
        refreshAddressValues();  // 调用同步版本（已有互斥锁保护）
        addressListRefreshing = false;
    });
}

void ScanWindow::refreshSingleAddress(int index)
{
    // 检查索引有效性
    if (index < 0 || index >= (int)addressList.size()) {
        return;
    }
    
    // 检查是否有进程附加
    if (!selectedPid || *selectedPid == 0) {
        addressList[index].currentValue = "N/A";
        return;
    }
    
    auto& item = addressList[index];
    std::vector<unsigned char> buffer;
    uint32_t size = 0;
    
    // 根据数值类型确定大小
    switch (item.valueType) {
        case 0: size = 1; break;  // 1字节
        case 1: size = 2; break;  // 2字节
        case 2: size = 4; break;  // 4字节
        case 3: size = 8; break;  // 8字节
        case 4: size = 4; break;  // 单精度浮点
        case 5: size = 8; break;  // 双精度浮点
        default: size = 4; break;
    }
    
    if (ReadProcessMemoryBytes(item.address, size, buffer, PORT_DEBUG) && buffer.size() >= size) {
        item.currentValue = formatValueOutput(buffer.data(), item.valueType);
    } else {
        item.currentValue = "读取失败";
    }
}

void ScanWindow::refreshScanResultsValues()
{
    // 检查是否有进程附加
    if (!selectedPid || *selectedPid == 0) {
        Gui::log("未附加进程，无法刷新");
        return;
    }
    
    // 【优化策略】三步走，最小化锁持有时间：
    // 1. 加锁 → 快速复制地址列表 → 释放锁
    // 2. 不加锁 → 读取所有内存（耗时操作）
    // 3. 加锁 → 快速更新结果 → 释放锁
    
    struct RefreshResult {
        size_t index;
        uint64_t address;
        uint64_t newValue;
        std::string newValueStr;
        bool success;
    };
    
    std::vector<RefreshResult> results;
    
    // 第一步：快速复制地址列表（锁持有时间 < 1ms）
    {
        std::lock_guard<std::mutex> lock(scanResultsMutex);
        
        if (scanResults.empty()) {
            return;
        }
        
        refreshProgress = 0;
        refreshTotal = scanResults.size();
        
        // 快速复制地址（只复制需要的数据）
        results.reserve(scanResults.size());
        for (size_t i = 0; i < scanResults.size(); i++) {
            RefreshResult r;
            r.index = i;
            r.address = scanResults[i].address;  // 复制地址
            r.success = false;
            results.push_back(r);
        }
    }  // 立即释放锁！UI不会被阻塞
    
    // 第二步：使用批量读取优化内存访问（大幅提升性能，完全不阻塞UI）
    int refreshCount = 0;
    int errorCount = 0;
    
    uint32_t size = 0;
    switch (valueType) {
        case 0: size = 1; break;
        case 1: size = 2; break;
        case 2: size = 4; break;
        case 3: size = 8; break;
        case 4: size = 4; break;
        case 5: size = 8; break;
        default: size = 4; break;
    }
    
    // 准备批量读取的地址列表（所有地址大小相同）
    std::vector<std::pair<uint64_t, int32_t>> batchAddrs;
    batchAddrs.reserve(results.size());
    for (auto& r : results) {
        batchAddrs.push_back({r.address, size});
    }
    
    // 批量读取所有地址（使用调试端口进行自动刷新）
    std::vector<std::pair<uint64_t, std::vector<uint8_t>>> batchResults;
    bool batchSuccess = ReadBratchAddr(batchAddrs, batchResults, PORT_DEBUG);
    
    if (batchSuccess && batchResults.size() == results.size()) {
        // 批量读取成功，解析结果
        for (size_t i = 0; i < results.size(); i++) {
            auto& r = results[i];
            auto& batch = batchResults[i];
            
            if (batch.second.size() >= size) {
                uint64_t newValue = 0;
                memcpy(&newValue, batch.second.data(), size);
                r.newValue = newValue;
                r.newValueStr = formatValueOutput(batch.second.data(), valueType);
                r.success = true;
                refreshCount++;
            } else {
                r.newValueStr = "读取失败";
                r.success = false;
                errorCount++;
            }
            refreshProgress++;
        }
    } else {
        // 批量读取失败，回退到逐个读取（使用调试端口）
        for (auto& r : results) {
            std::vector<unsigned char> buffer;
            if (ReadProcessMemoryBytes(r.address, size, buffer, PORT_DEBUG) && buffer.size() >= size) {
                uint64_t newValue = 0;
                memcpy(&newValue, buffer.data(), size);
                r.newValue = newValue;
                r.newValueStr = formatValueOutput(buffer.data(), valueType);
                r.success = true;
                refreshCount++;
            } else {
                r.newValueStr = "读取失败";
                r.success = false;
                errorCount++;
            }
            
            refreshProgress++;
        }
    }
    
    // 第三步：快速更新所有结果（锁持有时间 < 10ms）
    {
        std::lock_guard<std::mutex> lock(scanResultsMutex);
        
        float currentTime = ImGui::GetTime();
        for (const auto& r : results) {
            if (r.index >= scanResults.size()) continue;
            
            auto& result = scanResults[r.index];
            
            if (r.success) {
                // 检测值是否改变
                if (result.value != r.newValue && result.previousValue != 0) {
                    result.valueChanged = true;
                    result.changeTime = currentTime;
                }
                
                result.previousValue = result.value;
                result.value = r.newValue;
                result.valueStr = r.newValueStr;
            } else {
                result.valueStr = r.newValueStr;
            }
        }
    }  // 快速释放锁！
    
    // 日志和提示（不需要加锁）
    if (refreshCount > 0 || errorCount > 0) {
        static bool firstRefresh = true;
        if (firstRefresh || errorCount > 0) {
            Gui::log("已刷新当前页: %d 成功, %d 失败 (共 %d 项)", refreshCount, errorCount, (int)results.size());
            firstRefresh = false;
        }
    }
    
    if (results.size() > 500) {
        static bool warningShown = false;
        if (!warningShown && autoRefreshScanResults) {
            Gui::log("提示：当前页结果较多(%d个)，建议减少每页数量或增加刷新间隔。", (int)results.size());
            warningShown = true;
        }
    }
}

void ScanWindow::refreshScanResultsValuesAsync()
{
    // 双重检查：如果正在扫描，不允许刷新（防止Socket操作冲突）
    if (scanInProgress) {
        return;
    }
    
    // 如果已经在刷新，不启动新的刷新
    if (scanResultsRefreshing) {
        return;
    }
    
    // 等待之前的线程完成
    if (scanResultsRefreshThread.joinable()) {
        scanResultsRefreshThread.join();
    }
    
    // 标记为正在刷新
    scanResultsRefreshing = true;
    
    // 启动异步刷新线程
    scanResultsRefreshThread = std::thread([this]() {
        refreshScanResultsValues();  // 调用同步版本（已有互斥锁保护）
        scanResultsRefreshing = false;
    });
}

bool ScanWindow::writeAddressValue(int index, const std::string& value)
{
    // 检查索引有效性
    if (index < 0 || index >= (int)addressList.size()) {
        return false;
    }
    
    // 检查是否有进程附加
    if (!selectedPid || *selectedPid == 0) {
        Gui::log("错误：未附加进程");
        return false;
    }
    
    auto& item = addressList[index];
    std::vector<unsigned char> data;
    
    try {
        // 根据数值类型解析输入并转换为字节数组
        switch (item.valueType) {
            case 0: { // 1字节
                int val = std::stoi(value);
                if (val < 0 || val > 255) {
                    Gui::log("错误：值超出1字节范围 (0-255)");
                    return false;
                }
                data.push_back(static_cast<unsigned char>(val));
                break;
            }
            case 1: { // 2字节
                int val = std::stoi(value);
                if (val < 0 || val > 65535) {
                    Gui::log("错误：值超出2字节范围 (0-65535)");
                    return false;
                }
                uint16_t v = static_cast<uint16_t>(val);
                data.resize(2);
                std::memcpy(data.data(), &v, 2);
                break;
            }
            case 2: { // 4字节
                uint32_t val = static_cast<uint32_t>(std::stoul(value));
                data.resize(4);
                std::memcpy(data.data(), &val, 4);
                break;
            }
            case 3: { // 8字节
                uint64_t val = std::stoull(value);
                data.resize(8);
                std::memcpy(data.data(), &val, 8);
                break;
            }
            case 4: { // 单精度浮点
                float val = std::stof(value);
                data.resize(4);
                std::memcpy(data.data(), &val, 4);
                break;
            }
            case 5: { // 双精度浮点
                double val = std::stod(value);
                data.resize(8);
                std::memcpy(data.data(), &val, 8);
                break;
            }
            default:
                Gui::log("错误：不支持的数据类型");
                return false;
        }
        
        // 写入内存
        if (WriteProcessMemoryBytes(item.address, data.size(), data)) {
            return true;
        } else {
            Gui::log("错误：写入内存失败");
            return false;
        }
    } catch (const std::exception& e) {
        Gui::log("错误：无效的值格式 - %s", e.what());
        return false;
    }
}

std::string ScanWindow::formatScanResultValue(uint64_t value, int type)
{
    std::ostringstream oss;
    
    switch (type) {
        case 0: { // 1字节
            uint8_t byteValue = static_cast<uint8_t>(value & 0xFF);
            oss << static_cast<int>(byteValue);
            break;
        }
        case 1: { // 2字节
            uint16_t wordValue = static_cast<uint16_t>(value & 0xFFFF);
            oss << wordValue;
            break;
        }
        case 2: { // 4字节
            uint32_t dwordValue = static_cast<uint32_t>(value & 0xFFFFFFFF);
            oss << dwordValue;
            break;
        }
        case 3: { // 8字节
            oss << value;
            break;
        }
        case 4: { // 单精度浮点
            uint32_t dwordValue = static_cast<uint32_t>(value & 0xFFFFFFFF);
            float floatValue;
            std::memcpy(&floatValue, &dwordValue, sizeof(float));
            
            // 检查是否为有效的浮点数
            if (std::isfinite(floatValue)) {
                // 对于很小或很大的数使用科学计数法
                if (std::abs(floatValue) >= 1e6 || (std::abs(floatValue) < 1e-3 && floatValue != 0.0f)) {
                    oss << std::scientific << std::setprecision(3) << floatValue;
                } else {
                    oss << std::fixed << std::setprecision(6) << floatValue;
                }
            } else if (std::isnan(floatValue)) {
                oss << "NaN";
            } else if (std::isinf(floatValue)) {
                oss << (floatValue > 0 ? "+∞" : "-∞");
            } else {
                oss << "无效浮点数";
            }
            break;
        }
        case 5: { // 双精度浮点
            double doubleValue;
            std::memcpy(&doubleValue, &value, sizeof(double));
            
            // 检查是否为有效的浮点数
            if (std::isfinite(doubleValue)) {
                // 对于很小或很大的数使用科学计数法
                if (std::abs(doubleValue) >= 1e12 || (std::abs(doubleValue) < 1e-6 && doubleValue != 0.0)) {
                    oss << std::scientific << std::setprecision(6) << doubleValue;
                } else {
                    oss << std::fixed << std::setprecision(10) << doubleValue;
                }
            } else if (std::isnan(doubleValue)) {
                oss << "NaN";
            } else if (std::isinf(doubleValue)) {
                oss << (doubleValue > 0 ? "+∞" : "-∞");
            } else {
                oss << "无效浮点数";
            }
            break;
        }
        default:
            oss << "未知类型: " << value;
            break;
    }
    
    return oss.str();
}

std::string ScanWindow::formatScanResultValueHex(uint64_t value, int type)
{
    std::ostringstream oss;
    oss << std::hex << std::uppercase;
    
    switch (type) {
        case 0: { // 1字节
            uint8_t byteValue = static_cast<uint8_t>(value & 0xFF);
            oss << "0x" << std::setfill('0') << std::setw(2) << static_cast<int>(byteValue);
            break;
        }
        case 1: { // 2字节
            uint16_t wordValue = static_cast<uint16_t>(value & 0xFFFF);
            oss << "0x" << std::setfill('0') << std::setw(4) << wordValue;
            break;
        }
        case 2: { // 4字节
            uint32_t dwordValue = static_cast<uint32_t>(value & 0xFFFFFFFF);
            oss << "0x" << std::setfill('0') << std::setw(8) << dwordValue;
            break;
        }
        case 3: { // 8字节
            oss << "0x" << std::setfill('0') << std::setw(16) << value;
            break;
        }
        default:
            oss << "0x" << value;
            break;
    }
    
    return oss.str();
}

uint32_t ScanWindow::getScanTypeFlag()
{
    // 根据scanType枚举值转换为对应的标志位
    switch (scanType) {
        case UNKNOW_VAL: return _UNKNOW_VAL;
        case ACCURATE_VAL: return _ACCURATE_VAL;
        case LARGER_THAN_VAL: return _LARGER_THAN_VAL;
        case LESS_THAN_VAL: return _LESS_THAN_VAL;
        case BETWEEN_VAL: return _BETWEEN_VAL;
        case ADD_UNKNOW_VAL: return _ADD_UNKNOW_VAL;
        case ADD_ACCURATE_VAL: return _ADD_ACCURATE_VAL;
        case SUB_UNKNOW_VAL: return _SUB_UNKNOW_VAL;
        case SUB_ACCURATE_VAL: return _SUB_ACCURATE_VAL;
        case CHANGED_VAL: return _CHANGED_VAL;
        case UNCHANGED_VAL: return _UNCHANGED_VAL;
        default: return _ACCURATE_VAL;
    }
}

uint32_t ScanWindow::getValueTypeFlag()
{
    // 根据valueType转换为对应的TYPE标志位
    switch (valueType) {
        case 0: return BYTE_;      // 1字节
        case 1: return WORD_;      // 2字节
        case 2: return DWORD_;     // 4字节
        case 3: return QWORD_;     // 8字节
        case 4: return FLOAT_;     // 单精度浮点
        case 5: return DOUBLE_;    // 双精度浮点
        default: return DWORD_;
    }
}

uint32_t ScanWindow::getValueTypeSize()
{
    // 返回数值类型的字节大小
    switch (valueType) {
        case 0: return 1;          // 1字节
        case 1: return 2;          // 2字节
        case 2: return 4;          // 4字节
        case 3: return 8;          // 8字节
        case 4: return 4;          // 单精度浮点 4字节
        case 5: return 8;          // 双精度浮点 8字节
        default: return 4;
    }
}

const char* ScanWindow::getScanTypeName(int scanType) const
{
    switch (scanType) {
        case UNKNOW_VAL: return "未知初始值";
        case ACCURATE_VAL: return "精确数值";
        case LARGER_THAN_VAL: return "大于数值";
        case LESS_THAN_VAL: return "小于数值";
        case BETWEEN_VAL: return "值在范围内";
        case ADD_UNKNOW_VAL: return "值增加了未知值";
        case ADD_ACCURATE_VAL: return "值增加了精确值";
        case SUB_UNKNOW_VAL: return "值减少了未知值";
        case SUB_ACCURATE_VAL: return "值减少了精确值";
        case CHANGED_VAL: return "变动的数值";
        case UNCHANGED_VAL: return "未变动的数值";
        default: return "未知扫描类型";
    }
}

const char* ScanWindow::getValueTypeName(int valueType) const
{
    switch (valueType) {
        case 0: return "1字节 (BYTE)";
        case 1: return "2字节 (WORD)";
        case 2: return "4字节 (DWORD)";
        case 3: return "8字节 (QWORD)";
        case 4: return "单精度浮点";
        case 5: return "双精度浮点";
        default: return "未知数值类型";
    }
}

void ScanWindow::drawMemoryTypeSelectionModal()
{
    if (showMemoryTypeModal)
        ImGui::OpenPopup("Memory Type Selection");

    if (ImGui::BeginPopupModal("Memory Type Selection", &showMemoryTypeModal, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Select memory types to scan:");
        ImGui::Separator();
        
        // 快捷按钮
        if (ImGui::Button("All")) {
            selectedMemoryTypes = MemoryType::All;
        }
        ImGui::SameLine();
        if (ImGui::Button("None")) {
            selectedMemoryTypes = 0;
        }
        ImGui::SameLine();
        if (ImGui::Button("Common")) {
            selectedMemoryTypes =  MemoryType::C_Data | 
                                 MemoryType::C_Bss | MemoryType::Anonymous | 
                                 MemoryType::C_Alloc;
        }
        ImGui::SameLine();
        if (ImGui::Button("Code")) {
            selectedMemoryTypes = MemoryType::Code_App | MemoryType::C_Bss | MemoryType::C_Data;
        }
        
        ImGui::Separator();
        
        // 定义内存类型信息
        struct MemoryTypeInfo {
            MemoryType type;
            const char* name;
            const char* description;
            bool isCommon;
        };
        
        MemoryTypeInfo memoryTypes[] = {
            {MemoryType::C_Heap, "Ch", "C program heap memory", true},
            {MemoryType::Java_Heap, "Jh", "Java VM heap memory", true},
            {MemoryType::C_Alloc, "Ca", "C malloc allocated memory", true},
            {MemoryType::C_Data, "Cd", "Program data segment", true},
            {MemoryType::C_Bss, "C", "Program BSS segment", true},
            {MemoryType::Anonymous, "A", "Anonymous mapped memory", true},
            {MemoryType::Stack, "S", "Program stack memory", false},
            {MemoryType::Code_App, "xa", "Application code segment", false},
            {MemoryType::Code_System, "xs", "System library code", false},
            {MemoryType::Java, "J", "Java related memory", false},
            {MemoryType::Ashmem, "As", "Android shared memory", false},
            {MemoryType::Video, "V", "Graphics memory", false},
            {MemoryType::Bad, "B", "Invalid memory regions", false},
        };
        
        // 分两列显示
        ImGui::Columns(2, "MemoryTypeModalColumns", false);
        
        for (int i = 0; i < IM_ARRAYSIZE(memoryTypes); i++) {
            bool isSelected = (selectedMemoryTypes == MemoryType::All) || 
                             (selectedMemoryTypes & memoryTypes[i].type);
            
            // 常用类型用绿色高亮
            if (memoryTypes[i].isCommon) {
                ImGui::PushStyleColor(ImGuiCol_Text, ColorScheme::SuccessLight);
            }
            
            if (ImGui::Checkbox(memoryTypes[i].name, &isSelected)) {
                if (selectedMemoryTypes == MemoryType::All) {
                    // 如果当前是全选状态，切换为只选择这一个
                    selectedMemoryTypes = isSelected ? memoryTypes[i].type : 0;
                } else {
                    // 切换单个选项
                    if (isSelected) {
                        selectedMemoryTypes |= memoryTypes[i].type;
                    } else {
                        selectedMemoryTypes &= ~memoryTypes[i].type;
                    }
                    
                    // 检查是否选择了所有类型
                    uint32_t allTypes = 0;
                    for (int j = 0; j < IM_ARRAYSIZE(memoryTypes); j++) {
                        allTypes |= memoryTypes[j].type;
                    }
                    if ((selectedMemoryTypes & allTypes) == allTypes) {
                        selectedMemoryTypes = MemoryType::All;
                    }
                }
            }
            
            if (memoryTypes[i].isCommon) {
                ImGui::PopStyleColor();
            }
            
            // 显示工具提示
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", memoryTypes[i].description);
            }
            
            // 换列
            if (i == (IM_ARRAYSIZE(memoryTypes) - 1) / 2) {
                ImGui::NextColumn();
            }
        }
        
        ImGui::Columns(1);
        ImGui::Separator();
        
        // 显示当前选择状态
        if (selectedMemoryTypes == MemoryType::All) {
            ImGui::TextColored(ColorScheme::SuccessLight, "Selected: All memory types");
        } else if (selectedMemoryTypes == 0) {
            ImGui::TextColored(ColorScheme::ErrorLight, "Selected: No memory types");
        } else {
            int selectedCount = 0;
            for (int i = 0; i < IM_ARRAYSIZE(memoryTypes); i++) {
                if (selectedMemoryTypes & memoryTypes[i].type) {
                    selectedCount++;
                }
            }
            ImGui::TextColored(ColorScheme::InfoLight, "Selected: %d memory types", selectedCount);
        }
        
        ImGui::Separator();
        if (ImGui::Button("OK", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
            showMemoryTypeModal = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
            showMemoryTypeModal = false;
        }
        
        ImGui::EndPopup();
    }
}

// 扫描参数验证函数
bool ScanWindow::validateScanParameters(bool isFirstScan)
{
    // 检查进程是否选择
    if (!selectedPid || *selectedPid == 0) {
        Gui::log("请先选择进程");
        return false;
    }
    
    // 检查内存类型是否选择
    if (selectedMemoryTypes == 0) {
        Gui::log("请先选择内存类型");
        return false;
    }
    
    // 检查是否正在扫描
    if (scanInProgress) {
        Gui::log("扫描正在进行中，请等待完成");
        return false;
    }
    
    // 对于再次扫描，检查是否有之前的结果
    if (!isFirstScan && totalScanResults == 0) {
        Gui::log("请先执行首次扫描");
        return false;
    }
    
    // 验证扫描类型的有效性
    if (isFirstScan) {
        if (scanType == ADD_UNKNOW_VAL || scanType == SUB_UNKNOW_VAL || 
            scanType == CHANGED_VAL || scanType == UNCHANGED_VAL) {
            Gui::log("错误: '%s' 只能在再次扫描中使用，请先进行未知初始值或精确数值扫描", getScanTypeName(scanType));
            return false;
        }
    }
    
    return true;
}

bool ScanWindow::prepareValueBytes(std::vector<unsigned char>& valueBytes, bool isFirstScan)
{
    // 检查是否需要输入值的扫描类型
    bool needValue1 = !(scanType == UNKNOW_VAL || scanType == ADD_UNKNOW_VAL || 
                       scanType == SUB_UNKNOW_VAL || scanType == CHANGED_VAL || 
                       scanType == UNCHANGED_VAL);
    bool needValue2 = (scanType == BETWEEN_VAL);
    
    valueBytes.clear();
    
    // 处理第一个值
    if (needValue1) {
        // 检查输入是否为空
        if (strlen(valueBuf) == 0) {
            Gui::log("请输入数值");
            return false;
        }
        
        std::vector<unsigned char> value1Bytes = parseValueInput(valueBuf, valueType);
        if (value1Bytes.empty()) {
            Gui::log("无效的数值输入: %s", valueBuf);
            return false;
        }
        
        valueBytes = value1Bytes;
    } else {
        // 对于不需要值的扫描类型，创建一个占位字节数组
        valueBytes.resize(getValueTypeSize());
        // 填充为0
        std::fill(valueBytes.begin(), valueBytes.end(), 0);
    }
    
    // 处理第二个值（范围扫描）
    if (needValue2) {
        if (strlen(value2Buf) == 0) {
            Gui::log("范围扫描需要输入最大值");
            return false;
        }
        
        std::vector<unsigned char> value2Bytes = parseValueInput(value2Buf, valueType);
        if (value2Bytes.empty()) {
            Gui::log("无效的最大值输入: %s", value2Buf);
            return false;
        }
        
        // 验证范围是否合理
        if (!validateValueRange(valueBytes, value2Bytes)) {
            return false;
        }
        
        // 对于范围扫描，将两个值合并
        valueBytes.insert(valueBytes.end(), value2Bytes.begin(), value2Bytes.end());
    }
    
    return true;
}

bool ScanWindow::validateValueRange(const std::vector<unsigned char>& value1, const std::vector<unsigned char>& value2)
{
    if (value1.size() != value2.size()) {
        Gui::log("内部错误: 数值大小不匹配");
        return false;
    }
    
    bool isValid = false;
    switch (valueType) {
        case 0: { // 1字节
            uint8_t val1 = value1[0];
            uint8_t val2 = value2[0];
            isValid = val1 <= val2;
            if (!isValid) {
                Gui::log("错误: 最小值(%u)必须小于等于最大值(%u)", val1, val2);
            }
            break;
        }
        case 1: { // 2字节
            uint16_t val1, val2;
            std::memcpy(&val1, value1.data(), 2);
            std::memcpy(&val2, value2.data(), 2);
            isValid = val1 <= val2;
            if (!isValid) {
                Gui::log("错误: 最小值(%u)必须小于等于最大值(%u)", val1, val2);
            }
            break;
        }
        case 2: { // 4字节
            uint32_t val1, val2;
            std::memcpy(&val1, value1.data(), 4);
            std::memcpy(&val2, value2.data(), 4);
            isValid = val1 <= val2;
            if (!isValid) {
                Gui::log("错误: 最小值(%u)必须小于等于最大值(%u)", val1, val2);
            }
            break;
        }
        case 3: { // 8字节
            uint64_t val1, val2;
            std::memcpy(&val1, value1.data(), 8);
            std::memcpy(&val2, value2.data(), 8);
            isValid = val1 <= val2;
            if (!isValid) {
                Gui::log("错误: 最小值(%llu)必须小于等于最大值(%llu)", val1, val2);
            }
            break;
        }
        case 4: { // 单精度浮点
            float val1, val2;
            std::memcpy(&val1, value1.data(), 4);
            std::memcpy(&val2, value2.data(), 4);
            isValid = val1 <= val2;
            if (!isValid) {
                Gui::log("错误: 最小值(%.6f)必须小于等于最大值(%.6f)", val1, val2);
            }
            break;
        }
        case 5: { // 双精度浮点
            double val1, val2;
            std::memcpy(&val1, value1.data(), 8);
            std::memcpy(&val2, value2.data(), 8);
            isValid = val1 <= val2;
            if (!isValid) {
                Gui::log("错误: 最小值(%.10f)必须小于等于最大值(%.10f)", val1, val2);
            }
            break;
        }
        default:
            Gui::log("错误: 不支持的数值类型");
            return false;
    }
    
    return isValid;
}

// 确保内存查看器窗口存在
MemoryViewerWindow* ScanWindow::ensureMemoryViewerWindow()
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