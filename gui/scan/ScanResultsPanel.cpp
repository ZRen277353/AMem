#include "../ScanWindow.h"
#include "../AppContext.h"
#include "../ColorScheme.h"
#include "../Gui.h"
#include "../../mem/IMemService.h"
#include "../../imgui/imgui.h"
#include "../../socket/client_singleton.h"
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <utility>

namespace {
const char* getScanValueTypeLabel(int valueType)
{
    static constexpr const char* kLabels[] = {
        "1字节", "2字节", "4字节", "8字节", "浮点", "双精度"
    };
    const int labelCount = static_cast<int>(sizeof(kLabels) / sizeof(kLabels[0]));
    return (valueType >= 0 && valueType < labelCount) ? kLabels[valueType] : "Unknown";
}

bool isIntegerScanValueType(int valueType)
{
    return valueType >= 0 && valueType <= 3;
}
} // namespace

void ScanWindow::drawResultsPanel()
{
    ImGui::BeginChild("ScanResults", ImVec2(0, ImGui::GetContentRegionAvail().y * 0.5f), ImGuiChildFlags_Borders);

    if (scanResultsRefreshInterval < 0.1f) {
        scanResultsRefreshInterval = 0.1f;
    }

    // 自动刷新控制（使用异步版本，避免卡顿）
    // 注意：扫描进行时不允许刷新，防止Socket操作冲突
    bool shouldRefresh = false;
    {
        std::lock_guard<std::mutex> lock(scanResultsMutex);
        shouldRefresh = autoRefreshScanResults && !scanResults.empty() && !scanResultsRefreshThread.running() && !scanInProgress;
    }

    if (shouldRefresh) {
        timeSinceScanResultsRefresh += ImGui::GetIO().DeltaTime;
        if (timeSinceScanResultsRefresh >= scanResultsRefreshInterval) {
            refreshScanResultsValuesAsync();  // 使用异步版本
            timeSinceScanResultsRefresh = 0.0f;
        }
    }

    const int totalResults = totalScanResults.load();
    const int currentResultOffset = resultOffset.load();
    const int resultValueType = scanResultValueType.load();

    if (totalResults > 0) {
        // 刷新控制工具栏
        // 注意：扫描进行时禁用刷新按钮
        if (scanResultsRefreshThread.running() || scanInProgress) {
            ImGui::BeginDisabled();
            if (scanInProgress) {
                ImGui::Button("扫描中...");
            } else {
                ImGui::Button("刷新中...");
            }
            ImGui::EndDisabled();
            // 显示进度
            if (scanResultsRefreshTotal.load() > 0 && scanResultsRefreshThread.running()) {
                ImGui::SameLine();
                ImGui::Text("(%d/%d)", scanResultsRefreshProgress.load(), scanResultsRefreshTotal.load());
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
            if (scanResultsRefreshInterval < 0.1f) {
                scanResultsRefreshInterval = 0.1f;
            }
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
        ImGui::Checkbox("十六进制显示", &showScanResultHexValues);
        ImGui::SameLine();
        if (ImGui::Checkbox("按数值排序", &sortScanResultsByValue)) {
            loadScanResults();
        }
    }

    if (totalResults > 0) {
        int totalPages = (totalResults + resultPageSize - 1) / resultPageSize;
        int currentPage = currentResultOffset / resultPageSize + 1;

        ImGui::Text("第 %d 页，共 %d 页 (总共 %d 个结果)", currentPage, totalPages, totalResults);

        // 显示当前页的数值范围信息
        {
            std::lock_guard<std::mutex> lock(scanResultsMutex);
            if (!scanResults.empty()) {
                uint64_t minVal = UINT64_MAX, maxVal = 0;
                for (const auto& result : scanResults) {
                    uint64_t val = result.value;
                    // 根据数值类型截断
                    switch (resultValueType) {
                        case 0: val &= 0xFF; break;
                        case 1: val &= 0xFFFF; break;
                        case 2: val &= 0xFFFFFFFF; break;
                        case 3: break; // 8字节不需要截断
                        case 4: case 5: break; // 浮点数单独处理
                    }
                    if (isIntegerScanValueType(resultValueType)) { // 整数类型
                        if (val < minVal) minVal = val;
                        if (val > maxVal) maxVal = val;
                    }
                }

                if (isIntegerScanValueType(resultValueType) && minVal != UINT64_MAX) {
                    ImGui::SameLine();
                    ImGui::TextColored(ColorScheme::TextSecondary, " | 范围: %llu - %llu", minVal, maxVal);
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("上一页") && currentResultOffset > 0) {
            int previousOffset = currentResultOffset - resultPageSize;
            resultOffset = previousOffset > 0 ? previousOffset : 0;
            loadScanResults();
        }
        ImGui::SameLine();
        if (ImGui::Button("下一页") && currentResultOffset + resultPageSize < totalResults) {
            int lastOffset = totalResults - resultPageSize;
            if (lastOffset < 0) lastOffset = 0;
            int nextOffset = currentResultOffset + resultPageSize;
            resultOffset = nextOffset < lastOffset ? nextOffset : lastOffset;
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

        ImGuiListClipper clipper;
        clipper.Begin(scanResultsSize);
        while (clipper.Step())
        {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd && i < scanResultsSize; i++)
            {
                if (i >= (int)scanResults.size() || i >= (int)selectedScanResults.size()) {
                    break;
                }

                ImGui::PushID(i);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                bool isSelected = selectedScanResults[i] != 0;
                if (ImGui::Checkbox("##select", &isSelected)) {
                    selectedScanResults[i] = isSelected ? 1 : 0;
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("0x%016llX", scanResults[i].address);
                ImGui::TableSetColumnIndex(2);
                // 根据数值类型显示不同颜色（增强对比度）
                ImVec4 typeColor = ColorScheme::TextPrimary; // 默认白色
                switch (resultValueType) {
                    case 0: typeColor = ColorScheme::SuccessBright; break; // 1字节（增强可见性）
                    case 1: typeColor = ColorScheme::InfoBright; break; // 2字节（增强可见性）
                    case 2: typeColor = ColorScheme::WarningBright; break; // 4字节（增强可见性）
                    case 3: typeColor = ColorScheme::StatHighlight; break; // 8字节
                    case 4: typeColor = ColorScheme::WarningBright; break; // 浮点（增强可见性）
                    case 5: typeColor = ColorScheme::ErrorBright; break; // 双精度（增强可见性）
                }
                ImGui::TextColored(typeColor, "%s", getScanValueTypeLabel(resultValueType));
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

                    if (showScanResultHexValues && isIntegerScanValueType(resultValueType)) {
                        std::string hexStr = formatScanResultValueHex(scanResults[i].value, resultValueType);
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
                    if (showScanResultHexValues && isIntegerScanValueType(resultValueType)) {
                        std::string hexStr = formatScanResultValueHex(scanResults[i].value, resultValueType);
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
                        int clickedValueType = resultValueType;

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
                        resultContextMenuAddress = scanResults[i].address;
                        resultContextMenuValue = scanResults[i].valueStr;
                        resultContextMenuValueType = resultValueType;
                        ImGui::OpenPopup("ResultContext");
                    }
                }

                // 使用固定的 popup ID，从存储的数据中读取
                if (ImGui::BeginPopup("ResultContext")) {
                    ImGui::Text("地址: 0x%016llX", resultContextMenuAddress);
                    ImGui::Text("数值: %s", resultContextMenuValue.c_str());
                    ImGui::Separator();

                    if (ImGui::MenuItem("添加到地址列表")) {
                        std::lock_guard<std::mutex> addrLock(addressListMutex);
                        bool alreadyExists = false;
                        for (const auto& item : addressList) {
                            if (item.address == resultContextMenuAddress) {
                                alreadyExists = true;
                                break;
                            }
                        }
                        if (!alreadyExists) {
                            AddressListItem newItem;
                            newItem.address = resultContextMenuAddress;
                            newItem.valueType = resultContextMenuValueType;
                            newItem.currentValue = resultContextMenuValue;
                            newItem.description = "地址 " + std::to_string(addressList.size() + 1);
                            addressList.push_back(newItem);
                            Gui::log("已添加地址: 0x%016llX", newItem.address);
                        } else {
                            Gui::log("地址已存在于列表中");
                        }
                    }

                    if (ImGui::MenuItem("复制地址")) {
                        char addrStr[32];
                        std::snprintf(addrStr, sizeof(addrStr), "%016llX", resultContextMenuAddress);
                        ImGui::SetClipboardText(addrStr);
                        Gui::log("已复制地址: 0x%016llX", resultContextMenuAddress);
                    }

                    if (ImGui::MenuItem("复制数值")) {
                        ImGui::SetClipboardText(resultContextMenuValue.c_str());
                        Gui::log("已复制数值: %s", resultContextMenuValue.c_str());
                    }

                    if (ImGui::MenuItem("在内存查看器中打开")) {
                        // 临时释放锁，避免死锁
                        lock.unlock();

                        navigateToAddress(resultContextMenuAddress);

                        // 重新获取锁
                        lock.lock();
                    }

                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

void ScanWindow::sortScanResultsForDisplay()
{
    if (!sortScanResultsByValue || scanResults.size() < 2) {
        return;
    }

    if (selectedScanResults.size() != scanResults.size()) {
        selectedScanResults.resize(scanResults.size(), 0);
    }

    std::vector<std::pair<ScanResultItem, char>> sorted;
    sorted.reserve(scanResults.size());
    for (size_t i = 0; i < scanResults.size(); ++i) {
        sorted.emplace_back(std::move(scanResults[i]), selectedScanResults[i]);
    }

    std::stable_sort(sorted.begin(), sorted.end(),
        [](const auto& lhs, const auto& rhs) {
            if (lhs.first.value != rhs.first.value) {
                return lhs.first.value < rhs.first.value;
            }
            return lhs.first.address < rhs.first.address;
        });

    for (size_t i = 0; i < sorted.size(); ++i) {
        scanResults[i] = std::move(sorted[i].first);
        selectedScanResults[i] = sorted[i].second;
    }
}

void ScanWindow::loadScanResults()
{
    loadScanResultsForRevision(AppContext::Get().processRevision.load(std::memory_order_acquire));
}

void ScanWindow::loadScanResultsForRevision(uint64_t expectedProcessRevision)
{
    int totalResults = totalScanResults.load();
    if (totalResults <= 0) return;
    if (AppContext::Get().processRevision.load(std::memory_order_acquire) != expectedProcessRevision) {
        return;
    }

    int offset = resultOffset.load();
    if (offset < 0) {
        offset = 0;
        resultOffset = 0;
    }
    if (offset >= totalResults) {
        offset = totalResults - resultPageSize;
        if (offset < 0) offset = 0;
        resultOffset = offset;
    }

    const int remaining = totalResults - offset;
    const int count = resultPageSize < remaining ? resultPageSize : remaining;
    if (count <= 0) return;

    const uint64_t expectedScanEpoch = scanEpoch.load();
    Mem::ScanResultsRequest request;
    request.expectedEpoch = expectedScanEpoch;
    request.offset = static_cast<size_t>(offset);
    request.limit = static_cast<size_t>(count);
    const auto response = memService_.scanResults(
        memService_.captureContext(true), request);
    if (!response.ok()) {
        Gui::log("加载扫描结果失败 [%s]: %s",
                 Mem::errorCodeName(response.error().code),
                 response.error().message.c_str());
        if (response.error().code == Mem::ErrorCode::ScanSessionChanged ||
            response.error().code == Mem::ErrorCode::NoScanSession) {
            scanEpoch = 0;
            totalScanResults = 0;
            resultOffset = 0;
            std::lock_guard<std::mutex> lock(scanResultsMutex);
            scanResults.clear();
            selectedScanResults.clear();
        }
        return;
    }

    const auto& page = response.value();
    if (AppContext::Get().processRevision.load(std::memory_order_acquire) !=
            expectedProcessRevision ||
        page.session.target.processRevision != expectedProcessRevision ||
        scanEpoch.load() != expectedScanEpoch) {
        return;
    }

    std::lock_guard<std::mutex> lock(scanResultsMutex);
    if (AppContext::Get().processRevision.load(std::memory_order_acquire) !=
            expectedProcessRevision ||
        scanEpoch.load() != expectedScanEpoch) {
        return;
    }

    totalScanResults = static_cast<int>(page.total);
    scanEpoch = page.session.epoch;
    resultOffset = static_cast<int>(page.offset);
    scanResults.clear();
    scanResults.reserve(page.items.size());
    for (const auto& result : page.items) {
        ScanResultItem item;
        item.address = result.address;
        item.value = result.value;
        item.previousValue = result.value;
        item.valueChanged = false;
        item.changeTime = 0.0f;
        item.valueStr = formatScanResultValue(
            result.value, scanResultValueType.load());
        scanResults.push_back(std::move(item));
    }

    selectedScanResults.assign(scanResults.size(), 0);
    sortScanResultsForDisplay();
    Gui::log("已加载 %d 个扫描结果 (偏移: %d)",
             static_cast<int>(scanResults.size()), resultOffset.load());
}

void ScanWindow::refreshScanResultsValues(int refreshValueType, bool warnLargePage)
{
    refreshScanResultsValuesForRevision(
        refreshValueType,
        warnLargePage,
        AppContext::Get().processRevision.load(std::memory_order_acquire));
}

void ScanWindow::refreshScanResultsValuesForRevision(int refreshValueType, bool warnLargePage, uint64_t expectedProcessRevision)
{
    // 检查是否有进程附加
    if (!AppContext::Get().hasProcess()) {
        Gui::log("未附加进程，无法刷新");
        return;
    }
    if (AppContext::Get().processRevision.load(std::memory_order_acquire) != expectedProcessRevision) {
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
        if (AppContext::Get().processRevision.load(std::memory_order_acquire) != expectedProcessRevision) {
            return;
        }

        if (scanResults.empty()) {
            return;
        }
        if (refreshValueType != scanResultValueType.load()) {
            return;
        }

        scanResultsRefreshProgress = 0;
        scanResultsRefreshTotal = static_cast<int>(scanResults.size());

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
    switch (refreshValueType) {
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
    if (AppContext::Get().processRevision.load(std::memory_order_acquire) != expectedProcessRevision) {
        return;
    }

    if (batchSuccess && batchResults.size() == results.size()) {
        // 批量读取成功，解析结果
        for (size_t i = 0; i < results.size(); i++) {
            auto& r = results[i];
            auto& batch = batchResults[i];

            if (batch.second.size() >= size) {
                uint64_t newValue = 0;
                memcpy(&newValue, batch.second.data(), size);
                r.newValue = newValue;
                r.newValueStr = formatValueOutput(batch.second.data(), refreshValueType);
                r.success = true;
                refreshCount++;
            } else {
                r.newValueStr = "读取失败";
                r.success = false;
                errorCount++;
            }
            scanResultsRefreshProgress++;
        }
    } else {
        // 批量读取失败，回退到逐个读取（使用调试端口）
        for (auto& r : results) {
            std::vector<unsigned char> buffer;
            if (ReadProcessMemoryBytes(r.address, size, buffer, PORT_DEBUG) && buffer.size() >= size) {
                uint64_t newValue = 0;
                memcpy(&newValue, buffer.data(), size);
                r.newValue = newValue;
                r.newValueStr = formatValueOutput(buffer.data(), refreshValueType);
                r.success = true;
                refreshCount++;
            } else {
                r.newValueStr = "读取失败";
                r.success = false;
                errorCount++;
            }

            scanResultsRefreshProgress++;
        }
    }

    // 第三步：快速更新所有结果（锁持有时间 < 10ms）
    {
        std::lock_guard<std::mutex> lock(scanResultsMutex);
        if (AppContext::Get().processRevision.load(std::memory_order_acquire) != expectedProcessRevision) {
            return;
        }
        if (refreshValueType != scanResultValueType.load()) {
            return;
        }

        float currentTime = ImGui::GetTime();
        for (const auto& r : results) {
            auto resultIt = scanResults.end();
            if (r.index < scanResults.size() && scanResults[r.index].address == r.address) {
                resultIt = scanResults.begin() + r.index;
            } else {
                resultIt = std::find_if(scanResults.begin(), scanResults.end(),
                    [&r](const ScanResultItem& item) {
                        return item.address == r.address;
                    });
            }
            if (resultIt == scanResults.end()) continue;

            auto& result = *resultIt;

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

        sortScanResultsForDisplay();
    }  // 快速释放锁！

    // 日志和提示（不需要加锁）
    if (refreshCount > 0 || errorCount > 0) {
        if (scanResultsFirstRefreshLog || errorCount > 0) {
            Gui::log("已刷新当前页: %d 成功, %d 失败 (共 %d 项)", refreshCount, errorCount, (int)results.size());
            scanResultsFirstRefreshLog = false;
        }
    }

    if (results.size() > 500) {
        if (!scanResultsLargePageWarningShown && warnLargePage) {
            Gui::log("提示：当前页结果较多(%d个)，建议减少每页数量或增加刷新间隔。", (int)results.size());
            scanResultsLargePageWarningShown = true;
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
    if (scanResultsRefreshThread.running()) {
        return;
    }

    const int refreshValueType = scanResultValueType.load();
    const bool warnLargePage = autoRefreshScanResults;
    const uint64_t expectedProcessRevision = AppContext::Get().processRevision.load(std::memory_order_acquire);

    // 启动异步刷新线程
    scanResultsRefreshThread.launch([this, refreshValueType, warnLargePage, expectedProcessRevision](const std::atomic<bool>& /*cancel*/) {
        refreshScanResultsValuesForRevision(refreshValueType, warnLargePage, expectedProcessRevision);  // 调用同步版本（已有互斥锁保护）
    });
}
