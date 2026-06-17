#include "../ScanWindow.h"
#include "../AppContext.h"
#include "../ColorScheme.h"
#include "../Gui.h"
#include "../../imgui/imgui.h"
#include "../../socket/client_singleton.h"
#include <cstring>
#include <cstdint>

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
        float progress = 0.0f;
        uint64_t matchCount = 0;
        uint64_t totalBytes = 0;
        uint64_t scannedBytes = 0;
        {
            std::lock_guard<std::mutex> lock(scanProgressMutex);
            progress = scanProgress;
            matchCount = scanMatchCount;
            totalBytes = scanTotalBytes;
            scannedBytes = scanScannedBytes;
        }

        ImGui::Separator();
        ImGui::Text("正在扫描...");

        // 显示真实的进度条
        ImVec4 progressColor = ColorScheme::SuccessBright;
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, progressColor);

        // 进度显示文本
        char progressBuf[64];
        if (totalBytes > 0) {
            std::snprintf(progressBuf, sizeof(progressBuf), "%.1f%% (%llu / %llu MB)",
                progress * 100.0f,
                (unsigned long long)(scannedBytes / (1024 * 1024)),
                (unsigned long long)(totalBytes / (1024 * 1024)));
        } else {
            std::snprintf(progressBuf, sizeof(progressBuf), "%.1f%%", progress * 100.0f);
        }

        ImGui::ProgressBar(progress, ImVec2(-1, 0), progressBuf);
        ImGui::PopStyleColor();

        // 显示匹配计数和扫描速度
        if (matchCount > 0) {
            ImGui::Text("已找到匹配: %llu 个", matchCount);
        }

        // 显示扫描速度和预计剩余时间
        uint64_t& lastScannedBytes = progressLastScannedBytes;
        float& lastUpdateTime = progressLastUpdateTime;
        float& scanStartTime = progressStartTime;

        float currentTime = ImGui::GetTime();

        // 记录扫描开始时间
        if (progress == 0.0f || scanStartTime == 0.0f) {
            scanStartTime = currentTime;
            lastUpdateTime = currentTime;
            lastScannedBytes = 0;
        }

        // 更新速度显示
        if (currentTime - lastUpdateTime > 0.5f && scannedBytes > lastScannedBytes) {
            float timeDelta = currentTime - lastUpdateTime;
            uint64_t bytesDelta = scannedBytes - lastScannedBytes;
            float speed = (float)bytesDelta / timeDelta / (1024 * 1024); // MB/s

            // 显示速度
            char speedBuf[64];
            std::snprintf(speedBuf, sizeof(speedBuf), "扫描速度: %.1f MB/s", speed);
            ImGui::Text("%s", speedBuf);

            // 计算并显示预计剩余时间
            if (progress > 0.01f && speed > 0.1f) {
                float totalElapsed = currentTime - scanStartTime;
                float estimatedTotal = totalElapsed / progress;
                float estimatedRemaining = estimatedTotal - totalElapsed;

                if (estimatedRemaining > 0) {
                    int remainingMinutes = (int)(estimatedRemaining / 60);
                    int remainingSeconds = (int)estimatedRemaining % 60;
                    ImGui::SameLine();
                    ImGui::TextColored(ColorScheme::InfoLight,
                        "预计剩余: %d:%02d", remainingMinutes, remainingSeconds);
                }
            }

            lastScannedBytes = scannedBytes;
            lastUpdateTime = currentTime;
        }

        ImGui::Text("扫描类型: %s", getScanTypeName(activeScanType.load()));
        ImGui::Text("数值类型: %s", getValueTypeName(activeScanValueType.load()));

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

        int totalResults = totalScanResults.load();
        if (totalResults > 0) {
            ImGui::Text("找到 %d 个匹配结果", totalResults);
            uint64_t completedTotalBytes = 0;
            {
                std::lock_guard<std::mutex> lock(scanProgressMutex);
                completedTotalBytes = scanTotalBytes;
            }
            if (completedTotalBytes > 0) {
                ImGui::Text("已扫描 %llu MB 内存", completedTotalBytes / (1024 * 1024));
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

void ScanWindow::drawScanPanel()
{
    ImGui::BeginChild("ScanPanel", ImVec2(300, 0), ImGuiChildFlags_Borders);
    ImGui::Text("扫描设置");
    ImGui::Separator();
    bool parametersDisabled = scanInProgress.load();
    if (parametersDisabled) {
        ImGui::BeginDisabled();
    }

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
    if (parametersDisabled) {
        ImGui::EndDisabled();
    }

    // 根据扫描类型显示适当的按钮
    const int totalResults = totalScanResults.load();
    const int resultValueType = scanResultValueType.load();
    bool isFirstScan = (totalResults == 0);
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
                    int updatedCount = GetScanResultCount();
                    if (updatedCount < 0) {
                        Gui::log("获取扫描结果数量失败");
                    } else {
                        totalScanResults = updatedCount;
                        loadScanResults(); // 重新加载结果
                    }
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

    if (totalResults == 0) {
        if (isFirstScan && !scanCompleted) {
            ImGui::TextDisabled("尚未扫描");
        } else {
            ImGui::TextColored(ColorScheme::ErrorLight, "未找到匹配结果");
        }
    } else {
        ImGui::TextColored(ColorScheme::SuccessBright, "找到: %d 个结果", totalResults);

        // 根据结果数量显示不同的建议
        if (totalResults > 100000) {
            ImGui::TextColored(ColorScheme::ErrorLight, "结果过多，强烈建议继续筛选");
        } else if (totalResults > 10000) {
            ImGui::TextColored(ColorScheme::WarningBright, "结果较多，建议继续筛选");
        } else if (totalResults > 1000) {
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
                        newItem.valueType = resultValueType;
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

        if (ImGui::Button("当前页全部添加到地址列表", ImVec2(-1, 0))) {
            // 添加当前页结果到地址列表（限制数量）
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
                    newItem.valueType = resultValueType;
                    newItem.currentValue = result.valueStr;
                    newItem.description = "当前页添加 " + std::to_string(addressList.size() + 1);
                    addressList.push_back(newItem);
                    addCount++;
                }
            }

            Gui::log("已从当前页添加 %d 个地址到列表", addCount);
            if (addCount >= maxAdd) {
                Gui::log("注意: 限制最多添加 %d 个地址", maxAdd);
            }
        }
    }

    ImGui::EndChild();
}

void ScanWindow::drawMemoryTypeSelectionModal()
{
    if (showMemoryTypeModal)
        ImGui::OpenPopup("Memory Type Selection");

    if (ImGui::BeginPopupModal("Memory Type Selection", &showMemoryTypeModal, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Select memory types to scan:");
        ImGui::Separator();
        bool memoryTypesDisabled = scanInProgress.load();
        if (memoryTypesDisabled) {
            ImGui::BeginDisabled();
        }

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

        if (memoryTypesDisabled) {
            ImGui::EndDisabled();
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
