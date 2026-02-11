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
#include <cmath>

void MemoryViewerWindow::drawStructAnalyzerPanel()
{
    // 工具栏
    if (ImGui::Button("新建结构体")) {
        showStructEditor = true;
        memset(newStructName, 0, sizeof(newStructName));
    }
    
    ImGui::SameLine();
    if (ImGui::Button("自动分析")) {
        if (!structBuffer.empty()) {
            // 使用已读取的内存数据进行自动分析
            autoAnalyzeStructure(structBuffer);
            Gui::log("自动分析完成");
        } else if (structBaseAddress != 0) {
            // 还没读取内存，先读取一页（4096字节）
            std::vector<unsigned char> analyzeData;
            ReadProcessMemoryBytes(structBaseAddress, 4096, analyzeData);
            if (!analyzeData.empty()) {
                structBuffer = analyzeData;  // 保存到缓存
                autoAnalyzeStructure(analyzeData);
                Gui::log("自动分析完成（已读取 4096 字节）");
            }
        } else {
            Gui::log("请先输入地址并读取内存");
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("基于内存数据自动识别可能的数据类型");
    
    ImGui::SameLine();
    if (ImGui::Button("保存定义")) {
        saveStructDefinitions();
        Gui::log("结构体定义已保存");
    }
    
    ImGui::SameLine();
    if (ImGui::Button("加载定义")) {
        loadStructDefinitions();
        Gui::log("结构体定义已加载");
    }
    
    ImGui::Separator();
    
    // 上部分：地址输入和控制
    ImGui::BeginGroup();
    ImGui::Text("分析地址:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160);
    ImGui::InputScalar("##struct_addr", ImGuiDataType_U64, &structBaseAddress, nullptr, nullptr, "%llX", ImGuiInputTextFlags_CharsHexadecimal);
    
    ImGui::SameLine();
    if (ImGui::Button("读取内存")) {
        if (structBaseAddress != 0) {
            ReadProcessMemoryBytes(structBaseAddress, 512, structBuffer);
            if (selectedStructIndex >= 0 && selectedStructIndex < (int)structDefinitions.size()) {
                const auto& structDef = structDefinitions[selectedStructIndex];
                if (structDef.totalSize > (int)structBuffer.size()) {
                    ReadProcessMemoryBytes(structBaseAddress, structDef.totalSize, structBuffer);
                }
            }
            Gui::log("已读取 %d 字节", (int)structBuffer.size());
        }
    }
    
    ImGui::SameLine();
    if (ImGui::Button("清空缓存")) {
        structBuffer.clear();
    }
    
    ImGui::EndGroup();
    
    ImGui::Separator();
    
    // 分割布局：左侧结构体列表，右侧字段视图
    ImGui::BeginChild("StructList", ImVec2(200, 0), true);
    {
        ImGui::Text("结构体模板:");
        ImGui::Separator();
        
        for (size_t i = 0; i < structDefinitions.size(); i++)
        {
            bool isSelected = (selectedStructIndex == (int)i);
            if (ImGui::Selectable(structDefinitions[i].name.c_str(), isSelected))
            {
                selectedStructIndex = (int)i;
                // 自动读取该结构体大小的内存
                if (structBaseAddress != 0) {
                    ReadProcessMemoryBytes(structBaseAddress, structDefinitions[i].totalSize, structBuffer);
                }
            }
            
            // 右键菜单 - 使用唯一ID避免断言失败
            char struct_popup_id[64];
            snprintf(struct_popup_id, sizeof(struct_popup_id), "StructPopup_%zu", i);
            if (ImGui::BeginPopupContextItem(struct_popup_id)) {
                if (ImGui::MenuItem("编辑")) {
                    showStructEditor = true;
                }
                if (ImGui::MenuItem("删除")) {
                    structDefinitions.erase(structDefinitions.begin() + i);
                    selectedStructIndex = -1;
                }
                if (ImGui::MenuItem("复制")) {
                    StructDefinition copy = structDefinitions[i];
                    copy.name += " (副本)";
                    structDefinitions.push_back(copy);
                }
                ImGui::EndPopup();
            }
        }
    }
    ImGui::EndChild();
    
    ImGui::SameLine();
    
    // 右侧：字段详细视图
    ImGui::BeginChild("StructDetails", ImVec2(0, 0), true);
    {
        if (selectedStructIndex >= 0 && selectedStructIndex < (int)structDefinitions.size())
        {
            auto& structDef = structDefinitions[selectedStructIndex];
            
            ImGui::Text("结构体: %s (大小: %d 字节)", structDef.name.c_str(), structDef.totalSize);
            if (structBaseAddress != 0) {
                ImGui::SameLine();
                ImGui::TextColored(ColorScheme::SuccessBright, "@ 0x%llX", structBaseAddress);
            }
            
            if (ImGui::Button("编辑结构")) {
                showStructEditor = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("添加到监控")) {
                addStructToWatchList(structDef);
                Gui::log("已将结构体字段添加到监控列表");
            }
            
            ImGui::Separator();
            
            // 显示字段表格
            drawStructInstanceViewer();
        }
        else
        {
            // 没有选择结构体，但如果有内存数据，显示原始预览
            if (!structBuffer.empty() && structBaseAddress != 0) {
                ImGui::Text("内存数据预览");
                ImGui::SameLine();
                ImGui::TextColored(ColorScheme::SuccessBright, "@ 0x%llX (%d 字节)", structBaseAddress, (int)structBuffer.size());
                
                if (ImGui::Button("自动分析此数据")) {
                    autoAnalyzeStructure(structBuffer);
                    Gui::log("自动分析完成");
                }
                
                ImGui::Separator();
                
                // 显示简单的数据预览表格
                if (ImGui::BeginTable("RawDataPreview", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY))
                {
                    ImGui::TableSetupColumn("偏移", ImGuiTableColumnFlags_WidthFixed, 60);
                    ImGui::TableSetupColumn("十六进制", ImGuiTableColumnFlags_WidthFixed, 200);
                    ImGui::TableSetupColumn("数值", ImGuiTableColumnFlags_WidthFixed, 150);
                    ImGui::TableSetupColumn("ASCII", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableHeadersRow();
                    
                    int bytesPerRow = 16;
                    for (size_t row = 0; row < structBuffer.size(); row += bytesPerRow) {
                        ImGui::TableNextRow();
                        
                        // 偏移
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextColored(ColorScheme::InfoLight, "+0x%X", (int)row);
                        
                        // 十六进制
                        ImGui::TableSetColumnIndex(1);
                        std::string hexStr;
                        for (int i = 0; i < bytesPerRow && row + i < structBuffer.size(); i++) {
                            char buf[4];
                            sprintf(buf, "%02X ", structBuffer[row + i]);
                            hexStr += buf;
                        }
                        ImGui::Text("%s", hexStr.c_str());
                        
                        // 数值（尝试解析为常见类型）
                        ImGui::TableSetColumnIndex(2);
                        if (row + 3 < structBuffer.size()) {
                            uint32_t dword = *(uint32_t*)&structBuffer[row];
                            float fval = *(float*)&structBuffer[row];
                            if (!std::isnan(fval) && !std::isinf(fval) && fabs(fval) < 1e6) {
                                ImGui::Text("%u / %.2f", dword, fval);
                            } else {
                                ImGui::Text("%u", dword);
                            }
                        }
                        
                        // ASCII
                        ImGui::TableSetColumnIndex(3);
                        std::string asciiStr;
                        for (int i = 0; i < bytesPerRow && row + i < structBuffer.size(); i++) {
                            unsigned char c = structBuffer[row + i];
                            asciiStr += (c >= 32 && c < 127) ? (char)c : '.';
                        }
                        ImGui::Text("%s", asciiStr.c_str());
                    }
                    
                    ImGui::EndTable();
                }
            } else {
                // 没有数据，显示提示
                ImGui::TextDisabled("请选择一个结构体模板或读取内存数据");
                ImGui::Separator();
                ImGui::TextWrapped("提示:\n"
                                 "1. 输入要分析的内存地址\n"
                                 "2. 点击'读取内存'加载数据\n"
                                 "3. 点击'自动分析'识别数据类型\n"
                                 "或\n"
                                 "1. 点击'新建结构体'创建模板\n"
                                 "2. 从左侧列表选择已有模板");
            }
        }
    }
    ImGui::EndChild();
    
    // 结构体定义编辑器窗口
    if (showStructEditor) {
        drawStructDefinitionEditor();
    }
}

void MemoryViewerWindow::drawStructDefinitionEditor()
{
    if (ImGui::Begin("结构体定义编辑器", &showStructEditor))
    {
        ImGui::InputText("结构体名称", newStructName, sizeof(newStructName));
        
        if (ImGui::Button("创建结构体")) {
            if (strlen(newStructName) > 0) {
                StructDefinition newStruct;
                newStruct.name = newStructName;
                structDefinitions.push_back(newStruct);
                selectedStructIndex = (int)structDefinitions.size() - 1;
                memset(newStructName, 0, sizeof(newStructName));
            }
        }
        
        if (selectedStructIndex >= 0 && selectedStructIndex < (int)structDefinitions.size())
        {
            auto& currentStruct = structDefinitions[selectedStructIndex];
            
            ImGui::Separator();
            ImGui::Text("编辑结构体: %s", currentStruct.name.c_str());
            
            // 字段列表
            if (ImGui::BeginTable("FieldsTable", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
            {
                ImGui::TableSetupColumn("名称");
                ImGui::TableSetupColumn("类型");
                ImGui::TableSetupColumn("偏移");
                ImGui::TableSetupColumn("大小");
                ImGui::TableSetupColumn("数组");
                ImGui::TableSetupColumn("操作");
                ImGui::TableHeadersRow();
                
                for (size_t i = 0; i < currentStruct.fields.size(); i++)
                {
                    auto& field = currentStruct.fields[i];
                    ImGui::TableNextRow();
                    
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("%s", field.name.c_str());
                    
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%s", getFieldTypeName(field.type));
                    
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("0x%X", field.offset);
                    
                    ImGui::TableSetColumnIndex(3);
                    ImGui::Text("%d", field.size);
                    
                    ImGui::TableSetColumnIndex(4);
                    if (field.arrayCount > 1) {
                        ImGui::Text("[%d]", field.arrayCount);
                    } else {
                        ImGui::Text("-");
                    }
                    
                    ImGui::TableSetColumnIndex(5);
                    ImGui::PushID((int)i);
                    if (ImGui::Button("删除")) {
                        currentStruct.fields.erase(currentStruct.fields.begin() + i);
                        currentStruct.calculateSize();
                        i--;
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            
            ImGui::Separator();
            ImGui::Text("添加新字段:");
            
            ImGui::InputText("字段名", newFieldName, sizeof(newFieldName));
            
            const char* fieldTypes[] = { "BYTE", "WORD", "DWORD", "QWORD", "FLOAT", "DOUBLE", "POINTER", "STRING", "ARRAY", "STRUCT" };
            ImGui::Combo("类型", &newFieldType, fieldTypes, IM_ARRAYSIZE(fieldTypes));
            
            ImGui::InputInt("偏移", &newFieldOffset);
            ImGui::InputInt("数组个数", &newFieldArrayCount);
            if (newFieldArrayCount < 1) newFieldArrayCount = 1;
            
            ImGui::InputText("描述", newFieldDescription, sizeof(newFieldDescription));
            ImGui::Checkbox("是指针", &newFieldIsPointer);
            
            if (newFieldType == (int)FieldType::STRUCT) {
                ImGui::InputText("结构体类型", newFieldStructType, sizeof(newFieldStructType));
            }
            
            if (ImGui::Button("添加字段")) {
                if (strlen(newFieldName) > 0) {
                    StructField newField;
                    newField.name = newFieldName;
                    newField.type = (FieldType)newFieldType;
                    newField.offset = newFieldOffset;
                    newField.size = getFieldTypeSize((FieldType)newFieldType);
                    newField.arrayCount = newFieldArrayCount;
                    newField.description = newFieldDescription;
                    newField.isPointer = newFieldIsPointer;
                    if (newFieldType == (int)FieldType::STRUCT) {
                        newField.structTypeName = newFieldStructType;
                    }
                    
                    currentStruct.fields.push_back(newField);
                    currentStruct.calculateSize();
                    
                    // 清空输入
                    memset(newFieldName, 0, sizeof(newFieldName));
                    newFieldOffset = 0;
                    newFieldArrayCount = 1;
                    memset(newFieldDescription, 0, sizeof(newFieldDescription));
                    newFieldIsPointer = false;
                    memset(newFieldStructType, 0, sizeof(newFieldStructType));
                }
            }
            
            ImGui::Text("结构体总大小: %d 字节", currentStruct.totalSize);
        }
    }
    ImGui::End();
}

void MemoryViewerWindow::drawStructInstanceViewer()
{
    if (selectedStructIndex < 0 || selectedStructIndex >= (int)structDefinitions.size()) {
        return;
    }
    
    auto& structDef = structDefinitions[selectedStructIndex];
    
    // 如果缓存为空且有地址，自动读取
    if (structBuffer.empty() && structBaseAddress != 0) {
        ReadProcessMemoryBytes(structBaseAddress, structDef.totalSize, structBuffer);
    }
    
    if (structBuffer.empty()) {
        ImGui::TextDisabled("无内存数据 - 请点击上方'读取内存'按钮");
        return;
    }
    
    if (ImGui::BeginTable("StructInstance", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | 
                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable))
    {
        ImGui::TableSetupColumn("字段名", ImGuiTableColumnFlags_WidthFixed, 150);
        ImGui::TableSetupColumn("偏移", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("类型", ImGuiTableColumnFlags_WidthFixed, 120);
        ImGui::TableSetupColumn("值", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("操作", ImGuiTableColumnFlags_WidthFixed, 100);
        ImGui::TableHeadersRow();
        
        for (size_t i = 0; i < structDef.fields.size(); i++)
        {
            auto& field = structDef.fields[i];
            ImGui::TableNextRow();
            ImGui::PushID((int)i);
            
            // 字段名（可编辑）
            ImGui::TableSetColumnIndex(0);
            char nameBuf[128];
            strncpy(nameBuf, field.name.c_str(), sizeof(nameBuf) - 1);
            nameBuf[sizeof(nameBuf) - 1] = '\0';
            ImGui::PushItemWidth(-1);
            if (ImGui::InputText("##name", nameBuf, sizeof(nameBuf))) {
                field.name = nameBuf;
            }
            ImGui::PopItemWidth();
            if (!field.description.empty() && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", field.description.c_str());
            }
            
            // 偏移（可编辑）
            ImGui::TableSetColumnIndex(1);
            int offsetValue = field.offset;
            ImGui::PushItemWidth(-1);
            if (ImGui::InputInt("##offset", &offsetValue, 0, 0, ImGuiInputTextFlags_CharsHexadecimal)) {
                if (offsetValue >= 0) {
                    field.offset = offsetValue;
                    structDef.calculateSize();
                }
            }
            ImGui::PopItemWidth();
            
            // 类型（可修改的下拉框）
            ImGui::TableSetColumnIndex(2);
            const char* typeNames[] = {
                "BYTE", "WORD", "DWORD", "QWORD",
                "FLOAT", "DOUBLE", "POINTER",
                "STRING", "UTF-8", "UTF-16"
            };
            int currentType = (int)field.type;
            ImGui::PushItemWidth(-1);
            if (ImGui::Combo("##type", &currentType, typeNames, IM_ARRAYSIZE(typeNames))) {
                field.type = (FieldType)currentType;
                field.size = getFieldTypeSize(field.type);
                structDef.calculateSize();
            }
            ImGui::PopItemWidth();
            
            // 值（可编辑）
            ImGui::TableSetColumnIndex(3);
            std::string value = readFieldValue(structBuffer, field, structBaseAddress);
            
            char valueBuf[256];
            strncpy(valueBuf, value.c_str(), sizeof(valueBuf) - 1);
            valueBuf[sizeof(valueBuf) - 1] = '\0';
            
            ImGui::PushItemWidth(-1);
            if (ImGui::InputText("##value", valueBuf, sizeof(valueBuf), ImGuiInputTextFlags_EnterReturnsTrue)) {
                writeStructFieldValue((int)i, valueBuf);
                // 重新读取内存以更新显示
                ReadProcessMemoryBytes(structBaseAddress, structDef.totalSize, structBuffer);
            }
            ImGui::PopItemWidth();
            
            // 操作
            ImGui::TableSetColumnIndex(4);
            if (ImGui::SmallButton("浏览")) {
                uint64_t addr = structBaseAddress + field.offset;
                jumpToAddress(addr);
            }
            
            ImGui::SameLine();
            if (ImGui::SmallButton("监控")) {
                // 添加单个字段到监控列表
                MemoryWatchItem item;
                item.description = structDef.name + "." + field.name;
                item.address = structBaseAddress + field.offset;
                item.type = field.type;
                item.enabled = true;
                watchItems.push_back(item);
                
                // 立即读取一次值
                if (AppContext::Get().hasProcess()) {
                    watchItems.back().cachedValue = readWatchItemValue(watchItems.back());
                }
                
                Gui::log("已添加到监控: %s, 当前值: %s", 
                        item.description.c_str(), watchItems.back().cachedValue.c_str());
            }
            
            // 右键菜单
            if (ImGui::BeginPopupContextItem("FieldContext")) {
                ImGui::Text("字段: %s", field.name.c_str());
                ImGui::Text("地址: 0x%llX", structBaseAddress + field.offset);
                ImGui::Separator();
                
                if (ImGui::BeginMenu("更改类型为...")) {
                    const char* typeNames[] = {
                        "BYTE", "WORD", "DWORD", "QWORD",
                        "FLOAT", "DOUBLE", "POINTER",
                        "STRING", "UTF-8", "UTF-16"
                    };
                    for (int t = 0; t < IM_ARRAYSIZE(typeNames); t++) {
                        if (ImGui::MenuItem(typeNames[t], nullptr, (int)field.type == t)) {
                            field.type = (FieldType)t;
                            field.size = getFieldTypeSize(field.type);
                            structDef.calculateSize();
                        }
                    }
                    ImGui::EndMenu();
                }
                
                ImGui::Separator();
                
                if (ImGui::MenuItem("添加到监控")) {
                    MemoryWatchItem item;
                    item.description = structDef.name + "." + field.name;
                    item.address = structBaseAddress + field.offset;
                    item.type = field.type;
                    item.enabled = true;
                    watchItems.push_back(item);
                    
                    // 立即读取一次值
                    if (AppContext::Get().hasProcess()) {
                        watchItems.back().cachedValue = readWatchItemValue(watchItems.back());
                    }
                    
                    Gui::log("已添加到监控: %s, 当前值: %s", 
                            item.description.c_str(), watchItems.back().cachedValue.c_str());
                }
                
                if (ImGui::MenuItem("浏览内存")) {
                    jumpToAddress(structBaseAddress + field.offset);
                }
                
                ImGui::Separator();
                
                if (ImGui::MenuItem("删除字段")) {
                    structDef.fields.erase(structDef.fields.begin() + i);
                    structDef.calculateSize();
                }
                
                ImGui::EndPopup();
            }
            
            ImGui::PopID();
        }
        
        ImGui::EndTable();
    }
}

const char* MemoryViewerWindow::getFieldTypeName(FieldType type)
{
    switch (type) {
        case FieldType::BYTE: return "BYTE";
        case FieldType::WORD: return "WORD";
        case FieldType::DWORD: return "DWORD";
        case FieldType::QWORD: return "QWORD";
        case FieldType::FLOAT: return "FLOAT";
        case FieldType::DOUBLE: return "DOUBLE";
        case FieldType::POINTER: return "POINTER";
        case FieldType::STRING: return "STRING";
        case FieldType::STRING_UTF8: return "UTF-8";
        case FieldType::STRING_UTF16: return "UTF-16";
        case FieldType::ARRAY: return "ARRAY";
        case FieldType::STRUCT: return "STRUCT";
        default: return "UNKNOWN";
    }
}

int MemoryViewerWindow::getFieldTypeSize(FieldType type)
{
    switch (type) {
        case FieldType::BYTE: return 1;
        case FieldType::WORD: return 2;
        case FieldType::DWORD: return 4;
        case FieldType::QWORD: return 8;
        case FieldType::FLOAT: return 4;
        case FieldType::DOUBLE: return 8;
        case FieldType::POINTER: return 8; // 假设64位指针
        case FieldType::STRING: return 1; // 字符串按字节计算
        case FieldType::ARRAY: return 1; // 数组元素大小需要根据具体类型
        case FieldType::STRUCT: return 0; // 结构体大小需要计算
        default: return 1;
    }
}

std::string MemoryViewerWindow::readFieldValue(const std::vector<unsigned char>& data, const StructField& field, uint64_t baseAddr)
{
    if (field.offset + field.size * field.arrayCount > (int)data.size()) {
        return "超出范围";
    }
    
    std::stringstream ss;
    
    if (field.arrayCount > 1) {
        ss << "{ ";
        for (int i = 0; i < field.arrayCount && i < 10; i++) { // 限制显示前10个元素
            if (i > 0) ss << ", ";
            
            int elemOffset = field.offset + i * field.size;
            if (elemOffset + field.size <= (int)data.size()) {
                ss << readSingleFieldValue(data, field.type, elemOffset, field.size);
            }
        }
        if (field.arrayCount > 10) {
            ss << ", ...";
        }
        ss << " }";
        return ss.str();
    } else {
        return readSingleFieldValue(data, field.type, field.offset, field.size);
    }
}

std::string MemoryViewerWindow::readSingleFieldValue(const std::vector<unsigned char>& data, FieldType type, int offset, int size)
{
    std::stringstream ss;
    
    switch (type) {
        case FieldType::BYTE: {
            if (offset < (int)data.size()) {
                ss << "0x" << std::hex << std::uppercase << (int)data[offset] << " (" << (int)data[offset] << ")";
            }
            break;
        }
        case FieldType::WORD: {
            if (offset + 1 < (int)data.size()) {
                uint16_t val = *(uint16_t*)&data[offset];
                ss << "0x" << std::hex << std::uppercase << val << " (" << val << ")";
            }
            break;
        }
        case FieldType::DWORD: {
            if (offset + 3 < (int)data.size()) {
                uint32_t val = *(uint32_t*)&data[offset];
                ss << "0x" << std::hex << std::uppercase << val << " (" << val << ")";
            }
            break;
        }
        case FieldType::QWORD:
        case FieldType::POINTER: {
            if (offset + 7 < (int)data.size()) {
                uint64_t val = *(uint64_t*)&data[offset];
                ss << "0x" << std::hex << std::uppercase << val;
            }
            break;
        }
        case FieldType::FLOAT: {
            if (offset + 3 < (int)data.size()) {
                float val = *(float*)&data[offset];
                ss << std::fixed << std::setprecision(6) << val;
            }
            break;
        }
        case FieldType::DOUBLE: {
            if (offset + 7 < (int)data.size()) {
                double val = *(double*)&data[offset];
                ss << std::fixed << std::setprecision(6) << val;
            }
            break;
        }
        case FieldType::STRING: {
            // 读取以null结尾的字符串
            std::string str;
            for (int i = offset; i < (int)data.size() && i < offset + 256; i++) {
                if (data[i] == 0) break;
                if (data[i] >= 32 && data[i] < 127) {
                    str += (char)data[i];
                } else {
                    str += '.';
                }
            }
            ss << "\"" << str << "\"";
            break;
        }
        default:
            ss << "未知类型";
            break;
    }
    
    return ss.str();
}

void MemoryViewerWindow::saveStructDefinitions()
{
    std::ofstream file("struct_definitions.dat", std::ios::binary);
    if (!file.is_open()) return;
    
    size_t count = structDefinitions.size();
    file.write((char*)&count, sizeof(count));
    
    for (const auto& structDef : structDefinitions) {
        // 保存结构体名称
        size_t nameLen = structDef.name.size();
        file.write((char*)&nameLen, sizeof(nameLen));
        file.write(structDef.name.c_str(), nameLen);
        
        // 保存字段数量
        size_t fieldCount = structDef.fields.size();
        file.write((char*)&fieldCount, sizeof(fieldCount));
        
        // 保存每个字段
        for (const auto& field : structDef.fields) {
            size_t fieldNameLen = field.name.size();
            file.write((char*)&fieldNameLen, sizeof(fieldNameLen));
            file.write(field.name.c_str(), fieldNameLen);
            
            file.write((char*)&field.type, sizeof(field.type));
            file.write((char*)&field.offset, sizeof(field.offset));
            file.write((char*)&field.size, sizeof(field.size));
            file.write((char*)&field.arrayCount, sizeof(field.arrayCount));
            file.write((char*)&field.isPointer, sizeof(field.isPointer));
            
            size_t descLen = field.description.size();
            file.write((char*)&descLen, sizeof(descLen));
            file.write(field.description.c_str(), descLen);
            
            size_t structTypeLen = field.structTypeName.size();
            file.write((char*)&structTypeLen, sizeof(structTypeLen));
            file.write(field.structTypeName.c_str(), structTypeLen);
        }
        
        file.write((char*)&structDef.totalSize, sizeof(structDef.totalSize));
    }
}

void MemoryViewerWindow::loadStructDefinitions()
{
    std::ifstream file("struct_definitions.dat", std::ios::binary);
    if (!file.is_open()) return;
    
    size_t count;
    if (!file.read((char*)&count, sizeof(count))) return;
    
    structDefinitions.clear();
    structDefinitions.reserve(count);
    
    for (size_t i = 0; i < count; i++) {
        StructDefinition structDef;
        
        // 读取结构体名称
        size_t nameLen;
        if (!file.read((char*)&nameLen, sizeof(nameLen))) break;
        structDef.name.resize(nameLen);
        if (!file.read(&structDef.name[0], nameLen)) break;
        
        // 读取字段数量
        size_t fieldCount;
        if (!file.read((char*)&fieldCount, sizeof(fieldCount))) break;
        
        // 读取每个字段
        for (size_t j = 0; j < fieldCount; j++) {
            StructField field;
            
            size_t fieldNameLen;
            if (!file.read((char*)&fieldNameLen, sizeof(fieldNameLen))) break;
            field.name.resize(fieldNameLen);
            if (!file.read(&field.name[0], fieldNameLen)) break;
            
            if (!file.read((char*)&field.type, sizeof(field.type))) break;
            if (!file.read((char*)&field.offset, sizeof(field.offset))) break;
            if (!file.read((char*)&field.size, sizeof(field.size))) break;
            if (!file.read((char*)&field.arrayCount, sizeof(field.arrayCount))) break;
            if (!file.read((char*)&field.isPointer, sizeof(field.isPointer))) break;
            
            size_t descLen;
            if (!file.read((char*)&descLen, sizeof(descLen))) break;
            field.description.resize(descLen);
            if (descLen > 0 && !file.read(&field.description[0], descLen)) break;
            
            size_t structTypeLen;
            if (!file.read((char*)&structTypeLen, sizeof(structTypeLen))) break;
            field.structTypeName.resize(structTypeLen);
            if (structTypeLen > 0 && !file.read(&field.structTypeName[0], structTypeLen)) break;
            
            structDef.fields.push_back(field);
        }
        
        if (!file.read((char*)&structDef.totalSize, sizeof(structDef.totalSize))) break;
        
        structDefinitions.push_back(structDef);
    }
}

std::string MemoryViewerWindow::readUTF8String(const std::vector<unsigned char>& data, size_t offset, size_t maxLength)
{
    std::string result;
    size_t i = offset;
    size_t end = (offset + maxLength < data.size()) ? (offset + maxLength) : data.size();
    
    while (i < end) {
        unsigned char c = data[i];
        
        // 检查是否是终止符
        if (c == 0) break;
        
        // 单字节字符 (ASCII: 0x00-0x7F)
        if (c <= 0x7F) {
            if (c >= 32 && c < 127) {
                result += (char)c;
            } else if (c == '\n' || c == '\r' || c == '\t') {
                result += ' '; // 空白字符显示为空格
            } else {
                result += '.';
            }
            i++;
        }
        // 2字节UTF-8 (0xC0-0xDF)
        else if ((c & 0xE0) == 0xC0) {
            if (i + 1 < end && (data[i + 1] & 0xC0) == 0x80) {
                result += (char)c;
                result += (char)data[i + 1];
                i += 2;
            } else {
                result += '.';
                i++;
            }
        }
        // 3字节UTF-8 (0xE0-0xEF)
        else if ((c & 0xF0) == 0xE0) {
            if (i + 2 < end && (data[i + 1] & 0xC0) == 0x80 && (data[i + 2] & 0xC0) == 0x80) {
                result += (char)c;
                result += (char)data[i + 1];
                result += (char)data[i + 2];
                i += 3;
            } else {
                result += '.';
                i++;
            }
        }
        // 4字节UTF-8 (0xF0-0xF7)
        else if ((c & 0xF8) == 0xF0) {
            if (i + 3 < end && (data[i + 1] & 0xC0) == 0x80 && (data[i + 2] & 0xC0) == 0x80 && (data[i + 3] & 0xC0) == 0x80) {
                result += (char)c;
                result += (char)data[i + 1];
                result += (char)data[i + 2];
                result += (char)data[i + 3];
                i += 4;
            } else {
                result += '.';
                i++;
            }
        }
        // 无效的UTF-8序列
        else {
            result += '.';
            i++;
        }
    }
    
    return result;
}

std::string MemoryViewerWindow::utf16ToUtf8(const uint16_t* utf16Str, size_t length)
{
    std::string result;
    
    for (size_t i = 0; i < length; i++) {
        uint16_t c = utf16Str[i];
        
        if (c == 0) break;
        
        // BMP字符 (0x0000-0xD7FF, 0xE000-0xFFFF)
        if (c < 0xD800 || c > 0xDFFF) {
            if (c <= 0x7F) {
                // ASCII
                result += (char)c;
            } else if (c <= 0x7FF) {
                // 2字节UTF-8
                result += (char)(0xC0 | (c >> 6));
                result += (char)(0x80 | (c & 0x3F));
            } else {
                // 3字节UTF-8
                result += (char)(0xE0 | (c >> 12));
                result += (char)(0x80 | ((c >> 6) & 0x3F));
                result += (char)(0x80 | (c & 0x3F));
            }
        }
        // 代理对 (0xD800-0xDFFF)
        else if (c >= 0xD800 && c <= 0xDBFF && i + 1 < length) {
            uint16_t c2 = utf16Str[i + 1];
            if (c2 >= 0xDC00 && c2 <= 0xDFFF) {
                // 解码代理对为Unicode码点
                uint32_t codepoint = 0x10000 + ((c & 0x3FF) << 10) + (c2 & 0x3FF);
                
                // 4字节UTF-8
                result += (char)(0xF0 | (codepoint >> 18));
                result += (char)(0x80 | ((codepoint >> 12) & 0x3F));
                result += (char)(0x80 | ((codepoint >> 6) & 0x3F));
                result += (char)(0x80 | (codepoint & 0x3F));
                i++; // 跳过下一个字符
            } else {
                result += '.';
            }
        } else {
            result += '.';
        }
    }
    
    return result;
}

std::string MemoryViewerWindow::readUTF16String(const std::vector<unsigned char>& data, size_t offset, size_t maxLength)
{
    std::string result;
    size_t maxChars = maxLength / 2; // UTF-16每个字符至少2字节
    
    if (offset + 1 >= data.size()) {
        return result;
    }
    
    std::vector<uint16_t> utf16Chars;
    for (size_t i = 0; i < maxChars; i++) {
        size_t byteIdx = offset + i * 2;
        if (byteIdx + 1 >= data.size()) break;
        
        // 小端序读取UTF-16字符
        uint16_t c = data[byteIdx] | (data[byteIdx + 1] << 8);
        
        if (c == 0) break;
        utf16Chars.push_back(c);
    }
    
    if (!utf16Chars.empty()) {
        result = utf16ToUtf8(utf16Chars.data(), utf16Chars.size());
    }
    
    return result;
}


void MemoryViewerWindow::autoAnalyzeStructure(const std::vector<unsigned char>& data)
{
    if (data.empty()) return;
    
    StructDefinition autoStruct;
    autoStruct.name = "自动分析_" + std::to_string(structBaseAddress);
    
    int offset = 0;
    int fieldIndex = 1;
    
    // 简单的启发式分析
    while (offset < (int)data.size() - 8) {
        StructField field;
        field.offset = offset;
        
        // 检查是否是指针（地址通常很大）
        if (offset + 7 < (int)data.size()) {
            uint64_t val64 = *(uint64_t*)&data[offset];
            uint64_t tmp = val64;
            if((val64&0xffff00000000) == 0xb40000000000)
            {
                tmp = val64&0xffffffffffff;//取低位 预防b4
            }

            if (tmp > 0x4FFFFFFFFF && tmp < 0x7FFFFFFFFFFF) {
                field.type = FieldType::POINTER;
                field.size = 8;
                field.name = "ptr_" + std::to_string(fieldIndex++);
                
                // 添加详细标签：指针值
                char labelBuf[128];
                snprintf(labelBuf, sizeof(labelBuf), "指针 -> 0x%llX", (unsigned long long)val64);
                field.description = labelBuf;
                
                autoStruct.fields.push_back(field);
                offset += 8;
                continue;
            }
        }
        
        // 检查是否是浮点数
        if (offset + 3 < (int)data.size()) {
            float fval = *(float*)&data[offset];
            if (!std::isnan(fval) && !std::isinf(fval) && fval > -1000000 && fval < 1000000) {
                // 如果值在合理范围且不是整数，可能是浮点
                uint32_t ival = *(uint32_t*)&data[offset];
                if (ival > 0x1000 && (ival & 0xFF) != 0) {  // 不太像整数
                    field.type = FieldType::FLOAT;
                    field.size = 4;
                    field.name = "float_" + std::to_string(fieldIndex++);
                    
                    // 添加详细标签：浮点值
                    char labelBuf[128];
                    snprintf(labelBuf, sizeof(labelBuf), "浮点 = %.6f", fval);
                    field.description = labelBuf;
                    
                    autoStruct.fields.push_back(field);
                    offset += 4;
                    continue;
                }
            }
        }
        
        // 检查是否是字符串
        bool isString = true;
        int strLen = 0;
        std::string strPreview;
        for (int i = offset; i < (int)data.size() && i < offset + 32; i++) {
            unsigned char c = data[i];
            if (c == 0) {
                strLen = i - offset;
                break;
            }
            if (c < 32 || c > 126) {
                isString = false;
                break;
            }
            strPreview += (char)c;
            strLen++;
        }
        
        if (isString && strLen >= 4) {
            field.type = FieldType::STRING;
            field.size = strLen + 1;
            field.name = "str_" + std::to_string(fieldIndex++);
            
            // 添加详细标签：字符串内容预览
            if (strPreview.length() > 20) {
                strPreview = strPreview.substr(0, 20) + "...";
            }
            field.description = "字符串 = \"" + strPreview + "\"";
            
            autoStruct.fields.push_back(field);
            offset += strLen + 1;
            continue;
        }
        
        // 检查是否为小整数（可能是标志、枚举或计数器）
        if (offset + 3 < (int)data.size()) {
            uint32_t dval = *(uint32_t*)&data[offset];
            
            // 检查是否为布尔值或小标志
            if (dval <= 1) {
                field.type = FieldType::DWORD;
                field.size = 4;
                field.name = "flag_" + std::to_string(fieldIndex++);
                
                char labelBuf[128];
                snprintf(labelBuf, sizeof(labelBuf), "标志/布尔 = %u (%s)", dval, dval ? "true" : "false");
                field.description = labelBuf;
            }
            // 检查是否为小整数（可能是枚举、计数器等）
            else if (dval < 1000) {
                field.type = FieldType::DWORD;
                field.size = 4;
                field.name = "int_" + std::to_string(fieldIndex++);
                
                char labelBuf[128];
                snprintf(labelBuf, sizeof(labelBuf), "整数 = %u (可能是计数/ID/枚举)", dval);
                field.description = labelBuf;
            }
            // 大整数
            else {
                field.type = FieldType::DWORD;
                field.size = 4;
                field.name = "dword_" + std::to_string(fieldIndex++);
                
                char labelBuf[128];
                snprintf(labelBuf, sizeof(labelBuf), "整数 = %u (0x%X)", dval, dval);
                field.description = labelBuf;
            }
            
            autoStruct.fields.push_back(field);
            offset += 4;
        } else {
            break;
        }
    }
    
    autoStruct.calculateSize();
    
    if (!autoStruct.fields.empty()) {
        structDefinitions.push_back(autoStruct);
        selectedStructIndex = (int)structDefinitions.size() - 1;
        
        // 如果读取的数据不够，重新读取完整大小
        if (structBaseAddress != 0 && (int)structBuffer.size() < autoStruct.totalSize) {
            ReadProcessMemoryBytes(structBaseAddress, autoStruct.totalSize, structBuffer);
        }
        
        Gui::log("自动分析发现 %d 个字段，总大小 %d 字节", (int)autoStruct.fields.size(), autoStruct.totalSize);
    } else {
        Gui::log("自动分析未发现有效字段");
    }
}

void MemoryViewerWindow::addStructToWatchList(const StructDefinition& structDef)
{
    if (structBaseAddress == 0) return;
    
    int addedCount = 0;
    for (const auto& field : structDef.fields) {
        MemoryWatchItem item;
        item.description = structDef.name + "." + field.name;
        item.address = structBaseAddress + field.offset;
        item.type = field.type;
        item.enabled = true;
        watchItems.push_back(item);
        
        // 立即读取一次值
        if (AppContext::Get().hasProcess()) {
            watchItems.back().cachedValue = readWatchItemValue(watchItems.back());
        }
        
        addedCount++;
    }
    
    Gui::log("已添加 %d 个字段到监控列表", addedCount);
    
    // 触发一次立即更新
    if (AppContext::Get().hasProcess() && addedCount > 0) {
        timeSinceWatchUpdate = 0.0f;  // 重置计时器，立即触发更新
    }
}

bool MemoryViewerWindow::writeStructFieldValue(int fieldIndex, const std::string& value)
{
    if (selectedStructIndex < 0 || selectedStructIndex >= (int)structDefinitions.size()) {
        return false;
    }
    
    const auto& structDef = structDefinitions[selectedStructIndex];
    if (fieldIndex < 0 || fieldIndex >= (int)structDef.fields.size()) {
        return false;
    }
    
    const auto& field = structDef.fields[fieldIndex];
    uint64_t addr = structBaseAddress + field.offset;
    
    std::vector<unsigned char> data;
    
    try {
        switch (field.type) {
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
            
            default:
                return false;
        }
        
        // 写入内存
        if (WriteProcessMemoryBytes(addr, data.size(), data)) {
            // 更新本地缓存
            if (field.offset + data.size() <= structBuffer.size()) {
                memcpy(&structBuffer[field.offset], data.data(), data.size());
            }
            
            Gui::log("成功写入 %s.%s @ 0x%llX: %s", structDef.name.c_str(), field.name.c_str(), addr, value.c_str());
            return true;
        } else {
            Gui::log("错误：写入内存失败 - 地址 0x%llX", addr);
            return false;
        }
    } catch (const std::exception& e) {
        Gui::log("错误：无效的值格式 - %s", e.what());
        return false;
    }
}

