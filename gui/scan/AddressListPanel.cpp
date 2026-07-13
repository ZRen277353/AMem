#include "../ScanWindow.h"
#include "../AppContext.h"
#include "../ColorScheme.h"
#include "../Gui.h"
#include "../../imgui/imgui.h"
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cerrno>
#include <cstdlib>

static uint32_t GetAddressValueSize(int valueType)
{
    switch (valueType) {
        case 0: return 1;
        case 1: return 2;
        case 2: return 4;
        case 3: return 8;
        case 4: return 4;
        case 5: return 8;
        default: return 4;
    }
}

static bool ParseAddressListUnsigned(const std::string& input, uint64_t maxValue, uint64_t& value)
{
    size_t begin = input.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos || input[begin] == '-') {
        return false;
    }

    const char* str = input.c_str() + begin;
    char* end = nullptr;
    errno = 0;
    value = std::strtoull(str, &end, 0);
    if (end == str || errno == ERANGE || value > maxValue) {
        return false;
    }

    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') {
        ++end;
    }

    return *end == '\0';
}

static bool ParseAddressListFloat(const std::string& input, float& value)
{
    const char* str = input.c_str();
    char* end = nullptr;
    errno = 0;
    value = std::strtof(str, &end);
    if (end == str || errno == ERANGE) {
        return false;
    }
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') {
        ++end;
    }
    return *end == '\0';
}

static bool ParseAddressListDouble(const std::string& input, double& value)
{
    const char* str = input.c_str();
    char* end = nullptr;
    errno = 0;
    value = std::strtod(str, &end);
    if (end == str || errno == ERANGE) {
        return false;
    }
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') {
        ++end;
    }
    return *end == '\0';
}

void ScanWindow::drawAddressListPanel()
{
    const char* value_types[] = {
        "1字节", "2字节", "4字节", "8字节", "浮点", "双精度"
    };
    ImGui::BeginChild("AddressList", ImVec2(0, 0), ImGuiChildFlags_Borders);

    if (addressListRefreshInterval < 0.1f) {
        addressListRefreshInterval = 0.1f;
    }

    // 自动刷新控制（使用异步版本，避免卡顿）
    // 注意：扫描进行时不允许刷新，防止Socket操作冲突
    if (autoRefreshAddressList && !addressList.empty() && !addressListRefreshThread.running() && !scanInProgress) {
        timeSinceAddressListRefresh += ImGui::GetIO().DeltaTime;
        if (timeSinceAddressListRefresh >= addressListRefreshInterval) {
            refreshAddressValuesAsync();  // 使用异步版本
            timeSinceAddressListRefresh = 0.0f;
        }
    }

    // 工具栏
    // 注意：扫描进行时禁用刷新按钮
    if (addressListRefreshThread.running() || scanInProgress) {
        ImGui::BeginDisabled();
        if (scanInProgress) {
            ImGui::Button("扫描中...");
        } else {
            ImGui::Button("刷新中...");
        }
        ImGui::EndDisabled();
        if (addressListRefreshTotal.load() > 0 && addressListRefreshThread.running()) {
            ImGui::SameLine();
            ImGui::Text("(%d/%d)", addressListRefreshProgress.load(), addressListRefreshTotal.load());
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
        if (addressListRefreshInterval < 0.1f) {
            addressListRefreshInterval = 0.1f;
        }
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

        ImGuiListClipper clipper;
        clipper.Begin(addressListSize);
        while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++)
        {
            // 边界检查，防止在循环中 addressList 被修改
            if (i >= (int)addressList.size()) break;

            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Checkbox("##active", &addressList[i].active);
            ImGui::TableSetColumnIndex(1);
            char descBuf[256];
            std::snprintf(descBuf, sizeof(descBuf), "%s", addressList[i].description.c_str());
            if (ImGui::InputText("##desc", descBuf, sizeof(descBuf))) {
                addressList[i].description = descBuf;
            }
            ImGui::TableSetColumnIndex(2);

            // 先复制地址，避免在 jumpToAddress 调用期间访问可能失效的引用
            uint64_t itemAddress = addressList[i].address;

            char addrStr[32];
            std::snprintf(addrStr, sizeof(addrStr), "0x%016llX", itemAddress);
            if (ImGui::Selectable(addrStr, false, ImGuiSelectableFlags_None)) {
                // 临时释放锁，避免死锁
                lock.unlock();

                navigateToAddress(itemAddress, memService_);

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
            // 右键菜单 - PushID already provides unique scope
            if (ImGui::BeginPopupContextItem("AddrPopup")) {
                // 边界检查
                if (i < (int)addressList.size()) {
                    if (ImGui::MenuItem("复制地址")) {
                        ImGui::SetClipboardText(addrStr);
                    }
                    if (ImGui::MenuItem("在内存查看器中打开")) {
                        // 临时释放锁
                        lock.unlock();

                        navigateToAddress(itemAddress, memService_);

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
            const int valueTypeLabelCount = static_cast<int>(sizeof(value_types) / sizeof(value_types[0]));
            const bool hasValidValueTypeLabel = itemValueType >= 0 && itemValueType < valueTypeLabelCount;
            const char* itemValueTypeLabel = hasValidValueTypeLabel ? value_types[itemValueType] : "Unknown";
            switch (itemValueType) {
                case 0: typeColor = ColorScheme::SuccessBright; break; // 1字节
                case 1: typeColor = ColorScheme::InfoBright; break; // 2字节
                case 2: typeColor = ColorScheme::WarningBright; break; // 4字节
                case 3: typeColor = ColorScheme::StatHighlight; break; // 8字节
                case 4: typeColor = ColorScheme::WarningBright; break; // 浮点
                case 5: typeColor = ColorScheme::ErrorBright; break; // 双精度
            }
            ImGui::TextColored(typeColor, "%s", itemValueTypeLabel);
            ImGui::TableSetColumnIndex(4);

            auto& item = addressList[i];
            if (!item.valueEditActive) {
                std::snprintf(item.editValueBuffer, sizeof(item.editValueBuffer), "%s", item.currentValue.c_str());
            }

            ImGui::PushItemWidth(-1);
            bool valueSubmitted = ImGui::InputText(
                "##value",
                item.editValueBuffer,
                sizeof(item.editValueBuffer),
                ImGuiInputTextFlags_EnterReturnsTrue);
            if (ImGui::IsItemActivated()) {
                item.valueEditActive = true;
            }
            bool valueDeactivated = ImGui::IsItemDeactivated();
            if (valueSubmitted) {
                std::string submittedValue = item.editValueBuffer;
                item.valueEditActive = false;
                lock.unlock();

                bool writeOk = writeAddressValue(itemAddress, itemValueType, submittedValue);
                std::string refreshedValue;
                if (writeOk) {
                    refreshedValue = readAddressValue(itemAddress, itemValueType);
                }

                lock.lock();
                if (i < (int)addressList.size() &&
                    addressList[i].address == itemAddress &&
                    addressList[i].valueType == itemValueType) {
                    addressList[i].valueEditActive = false;
                    if (writeOk) {
                        addressList[i].currentValue = refreshedValue;
                    }
                }

                if (writeOk) {
                    Gui::log("成功写入地址 0x%016llX 的值: %s", itemAddress, submittedValue.c_str());
                } else {
                    Gui::log("写入失败：地址 0x%016llX", itemAddress);
                }
            } else if (valueDeactivated && i < (int)addressList.size()) {
                addressList[i].valueEditActive = false;
            }
            ImGui::PopItemWidth();

            // 显示工具提示
            if (ImGui::IsItemHovered()) {
                // 确保索引有效后再访问
                if (i < (int)addressList.size()) {
                    ImGui::SetTooltip("按回车键确认修改\n地址: 0x%016llX\n类型: %s",
                        itemAddress, itemValueTypeLabel);
                } else {
                    ImGui::SetTooltip("按回车键确认修改\n地址: 0x%016llX", itemAddress);
                }
            }
            ImGui::PopID();
        }
        } // clipper.Step()
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

void ScanWindow::refreshAddressValues()
{
    refreshAddressValuesForRevision(AppContext::Get().processRevision.load(std::memory_order_acquire));
}

void ScanWindow::refreshAddressValuesForRevision(uint64_t expectedProcessRevision)
{
    // 检查是否有进程附加
    if (!AppContext::Get().hasProcess()) {
        std::lock_guard<std::mutex> lock(addressListMutex);
        for (auto& item : addressList) {
            item.currentValue = "N/A";
        }
        return;
    }
    if (AppContext::Get().processRevision.load(std::memory_order_acquire) != expectedProcessRevision) {
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
        if (AppContext::Get().processRevision.load(std::memory_order_acquire) != expectedProcessRevision) {
            return;
        }

        if (addressList.empty()) {
            return;
        }

        addressListRefreshProgress = 0;
        addressListRefreshTotal = static_cast<int>(addressList.size());

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
    std::vector<Mem::MemoryReadRequest> batchAddrs;
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
        Mem::MemoryReadRequest request;
        request.address = data.address;
        request.size = size;
        request.channel = Mem::MemoryReadChannel::Background;
        batchAddrs.push_back(request);
    }

    // 批量读取所有地址（使用调试端口进行自动刷新）
    std::vector<Mem::MemoryBlock> batchResults;
    bool batchSuccess = readTargetMemoryBatch(
        batchAddrs, batchResults, Mem::MemoryReadChannel::Background);
    if (AppContext::Get().processRevision.load(std::memory_order_acquire) != expectedProcessRevision) {
        return;
    }

    if (batchSuccess && batchResults.size() == refreshData.size()) {
        // 批量读取成功，解析结果
        for (size_t i = 0; i < refreshData.size(); i++) {
            auto& data = refreshData[i];
            auto& result = batchResults[i];

            if (result.bytes.size() >= batchAddrs[i].size) {
                data.newValue = formatValueOutput(result.bytes.data(), data.valueType);
                data.success = true;
            } else {
                data.newValue = "读取失败";
                data.success = false;
            }
            addressListRefreshProgress++;
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

            if (readTargetMemory(
                    data.address, size, buffer,
                    Mem::MemoryReadChannel::Background) &&
                buffer.size() >= size) {
                data.newValue = formatValueOutput(buffer.data(), data.valueType);
                data.success = true;
            } else {
                data.newValue = "读取失败";
                data.success = false;
            }

            addressListRefreshProgress++;
        }
    }

    // 第三步：快速更新结果（锁持有时间 < 5ms）
    {
        std::lock_guard<std::mutex> lock(addressListMutex);
        if (AppContext::Get().processRevision.load(std::memory_order_acquire) != expectedProcessRevision) {
            return;
        }

        for (const auto& data : refreshData) {
            auto itemIt = addressList.end();
            if (data.index < addressList.size() &&
                addressList[data.index].address == data.address &&
                addressList[data.index].valueType == data.valueType) {
                itemIt = addressList.begin() + data.index;
            } else {
                itemIt = std::find_if(addressList.begin(), addressList.end(),
                    [&data](const AddressListItem& item) {
                        return item.address == data.address && item.valueType == data.valueType;
                    });
            }
            if (itemIt == addressList.end()) continue;
            itemIt->currentValue = data.newValue;
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
    if (addressListRefreshThread.running()) {
        return;
    }

    const uint64_t expectedProcessRevision = AppContext::Get().processRevision.load(std::memory_order_acquire);

    // 启动异步刷新线程
    addressListRefreshThread.launch([this, expectedProcessRevision](const std::atomic<bool>& /*cancel*/) {
        refreshAddressValuesForRevision(expectedProcessRevision);  // 调用同步版本（已有互斥锁保护）
    });
}

std::string ScanWindow::readAddressValue(uint64_t address, int valueType)
{
    // 检查是否有进程附加
    if (!AppContext::Get().hasProcess()) {
        return "N/A";
    }

    std::vector<unsigned char> buffer;
    uint32_t size = GetAddressValueSize(valueType);

    if (readTargetMemory(
            address, size, buffer,
            Mem::MemoryReadChannel::Background) &&
        buffer.size() >= size) {
        return formatValueOutput(buffer.data(), valueType);
    }

    return "读取失败";
}

bool ScanWindow::writeAddressValue(uint64_t address, int valueType, const std::string& value)
{
    // 检查是否有进程附加
    if (!AppContext::Get().hasProcess()) {
        Gui::log("错误：未附加进程");
        return false;
    }

    std::vector<unsigned char> data;

    try {
        // 根据数值类型解析输入并转换为字节数组
        switch (valueType) {
            case 0: { // 1字节
                uint64_t parsed = 0;
                if (!ParseAddressListUnsigned(value, 0xFF, parsed)) {
                    Gui::log("错误：值超出1字节范围 (0-255)");
                    return false;
                }
                data.push_back(static_cast<unsigned char>(parsed));
                break;
            }
            case 1: { // 2字节
                uint64_t parsed = 0;
                if (!ParseAddressListUnsigned(value, 0xFFFF, parsed)) {
                    Gui::log("错误：值超出2字节范围 (0-65535)");
                    return false;
                }
                uint16_t v = static_cast<uint16_t>(parsed);
                data.resize(2);
                std::memcpy(data.data(), &v, 2);
                break;
            }
            case 2: { // 4字节
                uint64_t parsed = 0;
                if (!ParseAddressListUnsigned(value, 0xFFFFFFFFULL, parsed)) {
                    Gui::log("错误：值超出4字节范围 (0-4294967295)");
                    return false;
                }
                uint32_t val = static_cast<uint32_t>(parsed);
                data.resize(4);
                std::memcpy(data.data(), &val, 4);
                break;
            }
            case 3: { // 8字节
                uint64_t val = 0;
                if (!ParseAddressListUnsigned(value, UINT64_MAX, val)) {
                    Gui::log("错误：无效的8字节整数");
                    return false;
                }
                data.resize(8);
                std::memcpy(data.data(), &val, 8);
                break;
            }
            case 4: { // 单精度浮点
                float val = 0.0f;
                if (!ParseAddressListFloat(value, val)) {
                    Gui::log("错误：无效的浮点数");
                    return false;
                }
                data.resize(4);
                std::memcpy(data.data(), &val, 4);
                break;
            }
            case 5: { // 双精度浮点
                double val = 0.0;
                if (!ParseAddressListDouble(value, val)) {
                    Gui::log("错误：无效的双精度浮点数");
                    return false;
                }
                data.resize(8);
                std::memcpy(data.data(), &val, 8);
                break;
            }
            default:
                Gui::log("错误：不支持的数据类型");
                return false;
        }

        // 写入内存
        Mem::Error error;
        if (writeTargetMemory(address, data, &error)) {
            return true;
        } else {
            Gui::log("错误：写入内存失败 - 地址 0x%llX [%s]: %s",
                     (unsigned long long)address,
                     Mem::errorCodeName(error.code), error.message.c_str());
            return false;
        }
    } catch (const std::exception& e) {
        Gui::log("错误：无效的值格式 - %s", e.what());
        return false;
    }
}
