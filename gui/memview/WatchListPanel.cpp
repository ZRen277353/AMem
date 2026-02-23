#include "../MemoryViewerWindow.h"
#include "../AppContext.h"
#include "../Gui.h"
#include "../ColorScheme.h"
#include "../../imgui/imgui.h"
#include "../../socket/client_singleton.h"
#include <algorithm>
#include <cstdint>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <fstream>

// 辅助：判断类型是否可冻结（固定大小 ≤ 8 字节的数值类型）
static bool isFreezableType(FieldType type) {
    switch (type) {
        case FieldType::BYTE:
        case FieldType::WORD:
        case FieldType::DWORD:
        case FieldType::QWORD:
        case FieldType::FLOAT:
        case FieldType::DOUBLE:
        case FieldType::POINTER:
            return true;
        default:
            return false;
    }
}

// 辅助：获取类型对应的字节大小（仅用于可冻结类型）
static uint8_t getFreezeDataSize(FieldType type) {
    switch (type) {
        case FieldType::BYTE:    return 1;
        case FieldType::WORD:    return 2;
        case FieldType::DWORD:   return 4;
        case FieldType::FLOAT:   return 4;
        case FieldType::QWORD:   return 8;
        case FieldType::DOUBLE:  return 8;
        case FieldType::POINTER: return 8;
        default:                 return 0;
    }
}

void MemoryViewerWindow::drawAddressList()
{
    // 工具栏
    if (ImGui::Button("添加地址")) {
        showAddItemDialog = true;
        memset(newItemDesc, 0, sizeof(newItemDesc));
        memset(newItemAddress, 0, sizeof(newItemAddress));
        memset(newItemOffsets, 0, sizeof(newItemOffsets));
        newItemType = 3; // 默认DWORD
        newItemIsPointer = false;
    }
    
    ImGui::SameLine();
    if (ImGui::Button("删除选中")) {
        if (selectedWatchIndex >= 0 && selectedWatchIndex < (int)watchItems.size()) {
            watchItems.erase(watchItems.begin() + selectedWatchIndex);
            selectedWatchIndex = -1;
        }
    }
    
    ImGui::SameLine();
    if (ImGui::Button("立即刷新")) {
        updateWatchItems();  // 手动触发刷新
        timeSinceWatchUpdate = 0.0f;  // 重置计时器
        Gui::log("已刷新监控项");
    }
    
    ImGui::SameLine();
    if (ImGui::Button("删除全部")) {
        watchItems.clear();
        selectedWatchIndex = -1;
    }
    
    ImGui::SameLine();
    if (ImGui::Button("保存列表")) {
        saveWatchList();
        Gui::log("监控列表已保存");
    }
    
    ImGui::SameLine();
    if (ImGui::Button("加载列表")) {
        loadWatchList();
        Gui::log("监控列表已加载");
    }
    
    ImGui::SameLine();
    ImGui::Text("更新间隔:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    ImGui::InputFloat("##interval", &watchUpdateInterval, 0, 0, "%.2f");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("自动更新间隔（秒）\n建议值: 0.5-2.0秒\n监控项多时建议增加间隔");
    }
    
    ImGui::Separator();
    
    // 自动更新监控项
    timeSinceWatchUpdate += ImGui::GetIO().DeltaTime;
    if (timeSinceWatchUpdate >= watchUpdateInterval) {
        updateWatchItems();
        timeSinceWatchUpdate = 0.0f;
    }
    
    // 地址列表表格
    if (ImGui::BeginTable("AddressList", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | 
                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 30);  // 启用/冻结
        ImGui::TableSetupColumn("描述", ImGuiTableColumnFlags_WidthFixed, 150);
        ImGui::TableSetupColumn("地址", ImGuiTableColumnFlags_WidthFixed, 140);
        ImGui::TableSetupColumn("类型", ImGuiTableColumnFlags_WidthFixed, 100);
        ImGui::TableSetupColumn("值", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("操作", ImGuiTableColumnFlags_WidthFixed, 120);
        ImGui::TableHeadersRow();
        
        for (size_t i = 0; i < watchItems.size(); i++) {
            auto& item = watchItems[i];
            ImGui::TableNextRow();
            ImGui::PushID((int)i);

            // 选中行高亮背景
            bool isSelected = (selectedWatchIndex == (int)i);
            if (isSelected) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImGuiCol_Header));
            }

            // 选择列：复选框（启用/禁用）
            ImGui::TableSetColumnIndex(0);
            ImGui::Checkbox("##enabled", &item.enabled);
            
            // 描述
            ImGui::TableSetColumnIndex(1);
            char descBuf[256];
            strncpy(descBuf, item.description.c_str(), sizeof(descBuf) - 1);
            descBuf[sizeof(descBuf) - 1] = '\0';
            ImGui::PushItemWidth(-1);
            if (ImGui::InputText("##desc", descBuf, sizeof(descBuf))) {
                item.description = descBuf;
            }
            ImGui::PopItemWidth();
            
            // 地址（可编辑）
            ImGui::TableSetColumnIndex(2);
            char addrBuf[32];
            snprintf(addrBuf, sizeof(addrBuf), "%016llX", item.address);
            ImGui::PushItemWidth(-1);
            if (ImGui::InputText("##addr", addrBuf, sizeof(addrBuf), ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue)) {
                uint64_t newAddr = 0;
                sscanf(addrBuf, "%llx", &newAddr);
                item.address = newAddr;
            }
            ImGui::PopItemWidth();
            
            // 类型（可修改下拉框）
            ImGui::TableSetColumnIndex(3);
            const char* typeNames[] = {
                "BYTE", "WORD", "DWORD", "QWORD",
                "FLOAT", "DOUBLE", "POINTER",
                "STRING", "UTF-8", "UTF-16"
            };
            int currentType = (int)item.type;
            ImGui::PushItemWidth(-1);
            if (ImGui::Combo("##type", &currentType, typeNames, IM_ARRAYSIZE(typeNames))) {
                item.type = (FieldType)currentType;
            }
            ImGui::PopItemWidth();
            
            // 值（使用缓存值，避免每帧读取）
            ImGui::TableSetColumnIndex(4);
            if (item.enabled) {
                // 使用缓存的值，updateWatchItems会定期更新
                std::string& value = item.cachedValue;
                
                // 可编辑的值
                char valueBuf[256];
                strncpy(valueBuf, value.c_str(), sizeof(valueBuf) - 1);
                valueBuf[sizeof(valueBuf) - 1] = '\0';
                
                ImGui::PushItemWidth(-1);
                if (ImGui::InputText("##value", valueBuf, sizeof(valueBuf), ImGuiInputTextFlags_EnterReturnsTrue)) {
                    if (writeWatchItemValue(item, valueBuf)) {
                        item.cachedValue = valueBuf;  // 立即更新缓存
                        // 如果该项正在冻结，同步更新服务端冻结值
                        if (item.frozen && item.frozenDataSize > 0) {
                            std::vector<unsigned char> rawData;
                            if (ReadProcessMemoryBytes(item.address, item.frozenDataSize, rawData) &&
                                rawData.size() >= item.frozenDataSize) {
                                memcpy(item.frozenData, rawData.data(), item.frozenDataSize);
                                FreezeUpdate(item.address, item.frozenData);
                            }
                        }
                    }
                }
                ImGui::PopItemWidth();
                
                // 值变化时高亮显示
                static std::vector<std::string> lastValues;
                if (lastValues.size() <= i) lastValues.resize(watchItems.size());
                if (lastValues[i] != value) {
                    ImGui::SameLine();
                    ImGui::TextColored(ColorScheme::WarningBright, "*");
                    lastValues[i] = value;
                }
            } else {
                ImGui::TextDisabled("禁用");
            }
            
            // 操作
            ImGui::TableSetColumnIndex(5);
            if (ImGui::SmallButton("浏览")) {
                jumpToAddress(item.address);
            }
            ImGui::SameLine();
            // 冻结：仅数值类型（≤8字节）可冻结
            if (!isFreezableType(item.type)) {
                ImGui::BeginDisabled();
                bool dummy = false;
                ImGui::Checkbox("冻结", &dummy);
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    ImGui::SetTooltip("该类型不支持冻结");
                }
            } else if (ImGui::Checkbox("冻结", &item.frozen)) {
                if (item.frozen) {
                    // 读取当前原始字节
                    uint8_t dataSize = getFreezeDataSize(item.type);
                    std::vector<unsigned char> rawData;
                    if (dataSize > 0 && dataSize <= 8 &&
                        ReadProcessMemoryBytes(item.address, dataSize, rawData) &&
                        rawData.size() >= dataSize) {
                        item.frozenDataSize = dataSize;
                        memset(item.frozenData, 0, sizeof(item.frozenData));
                        memcpy(item.frozenData, rawData.data(), dataSize);
                        // 委托服务端冻结
                        if (!FreezeAdd(item.address, dataSize, item.frozenData)) {
                            Gui::log("冻结失败: 0x%llX", (unsigned long long)item.address);
                            item.frozen = false;
                            item.frozenDataSize = 0;
                        }
                    } else {
                        Gui::log("冻结失败: 无法读取地址 0x%llX", (unsigned long long)item.address);
                        item.frozen = false;
                    }
                } else {
                    // 取消冻结
                    FreezeRemove(item.address);
                    item.frozenDataSize = 0;
                    memset(item.frozenData, 0, sizeof(item.frozenData));
                }
            }
            
            // 点击行空白区域选中（右键也可选中）
            if (ImGui::IsItemClicked(0) || ImGui::IsItemClicked(1)) {
                selectedWatchIndex = (int)i;
            }

            // 右键菜单
            if (ImGui::BeginPopupContextItem("WatchItemContext")) {
                ImGui::Text("地址: 0x%llX", item.address);
                ImGui::Separator();
                
                if (ImGui::BeginMenu("更改类型为...")) {
                    const char* typeNames[] = {
                        "BYTE", "WORD", "DWORD", "QWORD",
                        "FLOAT", "DOUBLE", "POINTER",
                        "STRING", "UTF-8", "UTF-16"
                    };
                    for (int t = 0; t < IM_ARRAYSIZE(typeNames); t++) {
                        if (ImGui::MenuItem(typeNames[t], nullptr, (int)item.type == t)) {
                            item.type = (FieldType)t;
                        }
                    }
                    ImGui::EndMenu();
                }
                
                ImGui::Separator();
                
                if (ImGui::MenuItem("复制地址")) {
                    char addrStr[32];
                    sprintf(addrStr, "%llX", item.address);
                    ImGui::SetClipboardText(addrStr);
                }
                
                if (ImGui::MenuItem("复制值")) {
                    std::string value = readWatchItemValue(item);
                    ImGui::SetClipboardText(value.c_str());
                }
                
                ImGui::Separator();
                
                if (ImGui::MenuItem("删除此项")) {
                    watchItems.erase(watchItems.begin() + i);
                    selectedWatchIndex = -1;
                }
                
                ImGui::EndPopup();
            }
            
            ImGui::PopID();
        }
        
        ImGui::EndTable();
    }
    
    // 添加项对话框
    if (showAddItemDialog) {
        drawAddItemDialog();
    }
}

void MemoryViewerWindow::drawAddItemDialog()
{
    ImGui::OpenPopup("添加监控地址");
    
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    
    if (ImGui::BeginPopupModal("添加监控地址", &showAddItemDialog, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("添加新的内存监控地址");
        ImGui::Separator();
        
        ImGui::InputText("描述", newItemDesc, sizeof(newItemDesc));
        ImGui::InputText("地址 (十六进制)", newItemAddress, sizeof(newItemAddress), ImGuiInputTextFlags_CharsHexadecimal);
        
        const char* typeNames[] = {
            "BYTE", "WORD", "DWORD", "QWORD", 
            "FLOAT", "DOUBLE", "POINTER", 
            "STRING", "UTF-8", "UTF-16"
        };
        ImGui::Combo("类型", &newItemType, typeNames, IM_ARRAYSIZE(typeNames));
        
        ImGui::Checkbox("指针", &newItemIsPointer);
        if (newItemIsPointer) {
            ImGui::InputText("偏移链 (十六进制, 逗号分隔)", newItemOffsets, sizeof(newItemOffsets));
        }
        
        ImGui::Separator();
        
        if (ImGui::Button("确定", ImVec2(120, 0))) {
            MemoryWatchItem newItem;
            newItem.description = newItemDesc;
            
            // 解析地址
            uint64_t addr = 0;
            sscanf(newItemAddress, "%llx", &addr);
            newItem.address = addr;
            
            // 设置类型
            newItem.type = (FieldType)newItemType;
            newItem.isPointer = newItemIsPointer;
            
            // 解析偏移链
            if (newItemIsPointer && strlen(newItemOffsets) > 0) {
                std::string s = newItemOffsets;
                size_t start = 0;
                while (start < s.size()) {
                    size_t comma = s.find(',', start);
                    std::string tok = (comma == std::string::npos) ? s.substr(start) : s.substr(start, comma - start);
                    uint64_t offset = 0;
                    sscanf(tok.c_str(), "%llx", &offset);
                    newItem.offsets.push_back(offset);
                    if (comma == std::string::npos) break;
                    start = comma + 1;
                }
            }
            
            watchItems.push_back(newItem);
            
            // 立即读取一次值
            if (AppContext::Get().hasProcess()) {
                watchItems.back().cachedValue = readWatchItemValue(watchItems.back());
            }
            
            showAddItemDialog = false;
            Gui::log("已添加监控地址: %s, 当前值: %s", 
                    newItem.description.c_str(), watchItems.back().cachedValue.c_str());
        }
        
        ImGui::SameLine();
        if (ImGui::Button("取消", ImVec2(120, 0))) {
            showAddItemDialog = false;
        }
        
        ImGui::EndPopup();
    }
}

std::string MemoryViewerWindow::readWatchItemValue(MemoryWatchItem& item)
{
    std::vector<unsigned char> data;
    uint64_t addr = item.address;
    
    // 如果是指针，先解引用
    if (item.isPointer && !item.offsets.empty()) {
        for (size_t i = 0; i < item.offsets.size(); i++) {
            std::vector<unsigned char> ptrData;
            ReadProcessMemoryBytes(addr, 8, ptrData);
            if (ptrData.size() < 8) return "??";
            
            addr = *(uint64_t*)&ptrData[0];
            if (i < item.offsets.size() - 1 || item.offsets.size() > 0) {
                addr += item.offsets[i];
            }
        }
        item.address = addr;  // 更新实际地址
    }
    
    // 根据类型读取数据
    int dataSize = getFieldTypeSize(item.type);
    if (item.type == FieldType::STRING || item.type == FieldType::STRING_UTF8 || item.type == FieldType::STRING_UTF16) {
        dataSize = 256;  // 字符串读取更多
    }
    
    ReadProcessMemoryBytes(addr, dataSize, data);
    if (data.empty()) return "??";
    
    std::stringstream ss;
    
    switch (item.type) {
        case FieldType::BYTE:
            if (data.size() >= 1) {
                ss << (int)data[0] << " (0x" << std::hex << std::uppercase << (int)data[0] << ")";
            }
            break;
            
        case FieldType::WORD:
            if (data.size() >= 2) {
                uint16_t val = *(uint16_t*)&data[0];
                ss << val << " (0x" << std::hex << std::uppercase << val << ")";
            }
            break;
            
        case FieldType::DWORD:
            if (data.size() >= 4) {
                uint32_t val = *(uint32_t*)&data[0];
                ss << val << " (0x" << std::hex << std::uppercase << val << ")";
            }
            break;
            
        case FieldType::QWORD:
        case FieldType::POINTER:
            if (data.size() >= 8) {
                uint64_t val = *(uint64_t*)&data[0];
                ss << "0x" << std::hex << std::uppercase << val;
            }
            break;
            
        case FieldType::FLOAT:
            if (data.size() >= 4) {
                float val = *(float*)&data[0];
                ss << std::fixed << std::setprecision(6) << val;
            }
            break;
            
        case FieldType::DOUBLE:
            if (data.size() >= 8) {
                double val = *(double*)&data[0];
                ss << std::fixed << std::setprecision(10) << val;
            }
            break;
            
        case FieldType::STRING:
            ss << readUTF8String(data, 0, 64);
            break;
            
        case FieldType::STRING_UTF8:
            ss << readUTF8String(data, 0, 128);
            break;
            
        case FieldType::STRING_UTF16:
            ss << readUTF16String(data, 0, 128);
            break;
            
        default:
            ss << "??";
            break;
    }
    
    return ss.str();
}

bool MemoryViewerWindow::writeWatchItemValue(MemoryWatchItem& item, const std::string& value)
{
    std::vector<unsigned char> data;
    
    try {
        switch (item.type) {
            case FieldType::BYTE: {
                int val = std::stoi(value);
                data.push_back((unsigned char)val);
                break;
            }
            
            case FieldType::WORD: {
                uint16_t val = (uint16_t)std::stoul(value);
                data.resize(2);
                *(uint16_t*)&data[0] = val;
                break;
            }
            
            case FieldType::DWORD: {
                uint32_t val = (uint32_t)std::stoul(value);
                data.resize(4);
                *(uint32_t*)&data[0] = val;
                break;
            }
            
            case FieldType::QWORD:
            case FieldType::POINTER: {
                uint64_t val = std::stoull(value, nullptr, 16);
                data.resize(8);
                *(uint64_t*)&data[0] = val;
                break;
            }
            
            case FieldType::FLOAT: {
                float val = std::stof(value);
                data.resize(4);
                *(float*)&data[0] = val;
                break;
            }
            
            case FieldType::DOUBLE: {
                double val = std::stod(value);
                data.resize(8);
                *(double*)&data[0] = val;
                break;
            }
            
            case FieldType::STRING:
            case FieldType::STRING_UTF8: {
                for (char c : value) {
                    data.push_back((unsigned char)c);
                }
                data.push_back(0);  // null terminator
                break;
            }
            
            default:
                return false;
        }
        
        // 写入内存
        if (WriteProcessMemoryBytes(item.address, data.size(), data)) {
            Gui::log("成功写入地址 0x%llX: %s", item.address, value.c_str());
            return true;
        } else {
            Gui::log("错误：写入内存失败 - 地址 0x%llX", item.address);
            return false;
        }
    } catch (const std::exception& e) {
        Gui::log("错误：无效的值格式 - %s", e.what());
        return false;
    }
}

void MemoryViewerWindow::updateWatchItems()
{
    // 如果没有附加进程，不执行任何操作
    if (!AppContext::Get().hasProcess()) {
        // 清空所有缓存值
        for (auto& item : watchItems) {
            item.cachedValue = "N/A";
        }
        return;
    }
    
    // 遍历所有监控项
    for (auto& item : watchItems) {
        if (!item.enabled) {
            item.cachedValue = "已禁用";
            continue;
        }
        
        // 冻结项和非冻结项统一读取当前值显示
        item.cachedValue = readWatchItemValue(item);
    }
}

void MemoryViewerWindow::saveWatchList()
{
    std::ofstream file("watch_list.dat", std::ios::binary);
    if (!file.is_open()) return;

    size_t count = watchItems.size();
    file.write((char*)&count, sizeof(count));

    for (const auto& item : watchItems) {
        size_t descLen = item.description.size();
        file.write((char*)&descLen, sizeof(descLen));
        file.write(item.description.c_str(), descLen);

        file.write((char*)&item.address, sizeof(item.address));
        file.write((char*)&item.type, sizeof(item.type));
        file.write((char*)&item.enabled, sizeof(item.enabled));
        file.write((char*)&item.frozen, sizeof(item.frozen));

        file.write((char*)&item.frozenDataSize, sizeof(item.frozenDataSize));
        file.write((char*)item.frozenData, sizeof(item.frozenData));

        file.write((char*)&item.isPointer, sizeof(item.isPointer));

        size_t offsetCount = item.offsets.size();
        file.write((char*)&offsetCount, sizeof(offsetCount));
        for (uint64_t offset : item.offsets) {
            file.write((char*)&offset, sizeof(offset));
        }
    }
}

void MemoryViewerWindow::loadWatchList()
{
    std::ifstream file("watch_list.dat", std::ios::binary);
    if (!file.is_open()) return;

    watchItems.clear();

    size_t count;
    if (!file.read((char*)&count, sizeof(count))) return;

    for (size_t i = 0; i < count; i++) {
        MemoryWatchItem item;

        size_t descLen;
        if (!file.read((char*)&descLen, sizeof(descLen))) break;
        if (descLen > 4096) break;
        item.description.resize(descLen);
        if (!file.read(&item.description[0], descLen)) break;

        if (!file.read((char*)&item.address, sizeof(item.address))) break;
        if (!file.read((char*)&item.type, sizeof(item.type))) break;
        if (!file.read((char*)&item.enabled, sizeof(item.enabled))) break;
        if (!file.read((char*)&item.frozen, sizeof(item.frozen))) break;

        if (!file.read((char*)&item.frozenDataSize, sizeof(item.frozenDataSize))) break;
        if (!file.read((char*)item.frozenData, sizeof(item.frozenData))) break;

        if (!file.read((char*)&item.isPointer, sizeof(item.isPointer))) break;

        size_t offsetCount;
        if (!file.read((char*)&offsetCount, sizeof(offsetCount))) break;
        if (offsetCount > 1024) break;
        for (size_t j = 0; j < offsetCount; j++) {
            uint64_t offset;
            if (!file.read((char*)&offset, sizeof(offset))) break;
            item.offsets.push_back(offset);
        }

        watchItems.push_back(item);
    }

    // 加载后，对冻结项重新注册到服务端
    for (auto& item : watchItems) {
        if (item.frozen && item.frozenDataSize > 0 && isFreezableType(item.type)) {
            if (!FreezeAdd(item.address, item.frozenDataSize, item.frozenData)) {
                item.frozen = false;
                item.frozenDataSize = 0;
            }
        }
    }
}

