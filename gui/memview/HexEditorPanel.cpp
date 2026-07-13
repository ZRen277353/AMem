#include "../MemoryViewerWindow.h"
#include "../AppContext.h"
#include "../Gui.h"
#include "../ColorScheme.h"
#include "../../imgui/imgui.h"
#include "../../mem/IMemService.h"
#include <algorithm>
#include <cstdint>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cerrno>

namespace {
bool addAddressOffset(uint64_t base, uint64_t offset, uint64_t& result)
{
    if (base > UINT64_MAX - offset) {
        result = 0;
        return false;
    }

    result = base + offset;
    return true;
}

bool addressSpanEndInclusive(uint64_t start, size_t size, uint64_t& end)
{
    if (size == 0) {
        end = start;
        return true;
    }

    const uint64_t lastOffset = static_cast<uint64_t>(size - 1);
    return addAddressOffset(start, lastOffset, end);
}

bool addressInSpan(uint64_t start, size_t size, uint64_t address)
{
    return address >= start && static_cast<uint64_t>(address - start) < static_cast<uint64_t>(size);
}

bool parseHexStrict(const char* text, uint64_t& value)
{
    if (!text) {
        return false;
    }

    const char* begin = text;
    while (*begin == ' ' || *begin == '\t' || *begin == '\r' || *begin == '\n') {
        ++begin;
    }
    if (*begin == '\0' || *begin == '-') {
        return false;
    }

    char* end = nullptr;
    errno = 0;
    value = std::strtoull(begin, &end, 16);
    if (end == begin || errno == ERANGE) {
        return false;
    }

    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') {
        ++end;
    }

    return *end == '\0';
}

void resetHexInput(char* buffer, size_t size)
{
    std::snprintf(buffer, size, "%s", "0");
}

template <typename T>
bool readScalar(const std::vector<unsigned char>& data, int offset, T& value)
{
    if (offset < 0 || (size_t)offset + sizeof(T) > data.size()) {
        return false;
    }

    std::memcpy(&value, data.data() + offset, sizeof(T));
    return true;
}
}

void MemoryViewerWindow::drawMemoryViewerPanel()
{
    auto navigateToHistoryIndex = [this](int newHistoryIndex) {
        if (newHistoryIndex < 0 || newHistoryIndex >= (int)addressHistory.size()) {
            return;
        }

        historyIndex = newHistoryIndex;
        uint64_t historyAddr = addressHistory[historyIndex];
        targetAddress = historyAddr;
        pageBaseAddress = (historyAddr / pageSize) * pageSize;
        viewAddress = pageBaseAddress;
        viewSize = pageSize;
        buffer.resize(viewSize);

        if (!readTargetMemory(
                viewAddress, static_cast<uint32_t>(viewSize), buffer,
                Mem::MemoryReadChannel::Background)) {
            std::fill(buffer.begin(), buffer.end(), 0);
            Gui::log("读取历史地址失败: 0x%llX", (unsigned long long)historyAddr);
        }
        scrollToTarget = true;
    };

    // 窗口聚焦时的键盘快捷键
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && !ImGui::GetIO().WantTextInput) {
        ImGuiIO& io = ImGui::GetIO();
        if (io.KeyAlt) {
            // Alt+Left: 后退
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) && historyIndex > 0) {
                navigateToHistoryIndex(historyIndex - 1);
            }
            // Alt+Right: 前进
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) && historyIndex < (int)addressHistory.size() - 1) {
                navigateToHistoryIndex(historyIndex + 1);
            }
        }
        if (io.KeyCtrl) {
            // Ctrl+G: 聚焦地址输入框
            if (ImGui::IsKeyPressed(ImGuiKey_G)) {
                ImGui::SetKeyboardFocusHere(8); // 跳到地址输入框（近似偏移）
            }
            // Ctrl+R: 刷新内存
            if (ImGui::IsKeyPressed(ImGuiKey_R)) {
                refreshMemory();
            }
        }
        // F5: 刷新内存
        if (ImGui::IsKeyPressed(ImGuiKey_F5)) {
            refreshMemory();
        }
    }

    // 工具栏 - 第一行：导航和地址
    ImGui::BeginGroup();
    
    // 计算当前单元大小用于对齐（在开始处计算，供后续使用）
    int bytesPerUnit = 1;
    switch ((DisplayFormat)displayFormat) {
        case DisplayFormat::Hex_Byte:
        case DisplayFormat::Dec_Byte:
        case DisplayFormat::Binary_Byte:
            bytesPerUnit = 1; break;
        case DisplayFormat::Hex_2Bytes:
        case DisplayFormat::Dec_2Bytes:
            bytesPerUnit = 2; break;
        case DisplayFormat::Hex_4Bytes:
        case DisplayFormat::Dec_4Bytes:
        case DisplayFormat::Float_4Bytes:
            bytesPerUnit = 4; break;
        case DisplayFormat::Hex_8Bytes:
        case DisplayFormat::Dec_8Bytes:
        case DisplayFormat::Double_8Bytes:
            bytesPerUnit = 8; break;
    }
    
    // 导航按钮
    ImGui::BeginDisabled(historyIndex <= 0);
    if (ImGui::ArrowButton("##back", ImGuiDir_Left)) {
        if (historyIndex > 0) {
            navigateToHistoryIndex(historyIndex - 1);
        }
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("后退 (Alt+Left)");

    ImGui::SameLine();
    ImGui::BeginDisabled(historyIndex >= (int)addressHistory.size() - 1);
    if (ImGui::ArrowButton("##forward", ImGuiDir_Right)) {
        if (historyIndex < (int)addressHistory.size() - 1) {
            navigateToHistoryIndex(historyIndex + 1);
        }
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("前进 (Alt+Right)");
    
    ImGui::SameLine();
    ImGui::Separator();
    ImGui::SameLine();
    
    // 地址输入 - 支持表达式计算
    ImGui::Text("地址:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200);
    
    // 使用targetAddress作为显示地址（目标地址而不是页首）
    // 当目标地址改变时，更新输入框显示
    if (targetAddress != lastHexAddressInputTarget) {
        snprintf(hexAddressInputBuf, sizeof(hexAddressInputBuf), "%llX", targetAddress);
        lastHexAddressInputTarget = targetAddress;
    }
    
    if (ImGui::InputText("##addr", hexAddressInputBuf, sizeof(hexAddressInputBuf), ImGuiInputTextFlags_EnterReturnsTrue)) {
        // 解析地址表达式
        uint64_t newAddress = 0;
        if (parseAddressExpression(hexAddressInputBuf, newAddress)) {
            jumpToAddress(newAddress);
            // 更新输入框显示为计算后的地址
            snprintf(hexAddressInputBuf, sizeof(hexAddressInputBuf), "%llX", newAddress);
        } else {
            Gui::log("无效的地址表达式: %s", hexAddressInputBuf);
            // 恢复为上次有效的地址
            snprintf(hexAddressInputBuf, sizeof(hexAddressInputBuf), "%llX", targetAddress);
        }
    }
    
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("支持十六进制运算\n例如: 1000+200, 5000-100\n按回车确认 (Ctrl+G 聚焦)");
    }
    
    ImGui::SameLine();
    if (ImGui::Button("读取")) {
        uint64_t addr = 0;
        if (parseAddressExpression(hexAddressInputBuf, addr)) {
            jumpToAddress(addr);
        }
    }
    
    ImGui::SameLine();
    if (ImGui::Button("刷新")) {
        refreshMemory();
    }
    
    // 翻页按钮
    ImGui::SameLine();
    ImGui::Separator();
    ImGui::SameLine();
    
    if (ImGui::Button("上一页")) {
        if (pageBaseAddress >= pageSize) {
            uint64_t newAddress = pageBaseAddress - pageSize;
            jumpToAddress(newAddress);
        } else {
            jumpToAddress(0);
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("向上翻页");
    
    ImGui::SameLine();
    if (ImGui::Button("下一页")) {
        if (pageBaseAddress <= UINT64_MAX - (uint64_t)pageSize) {
            uint64_t newAddress = pageBaseAddress + pageSize;
            jumpToAddress(newAddress);
        } else {
            Gui::log("已到达地址空间末尾，无法继续下一页");
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("向下翻页");
    
    // 页面信息显示
    ImGui::SameLine();
    ImGui::Separator();
    ImGui::SameLine();
    
    uint64_t pageNumber = pageBaseAddress / pageSize;
    uint64_t pageEndAddress = pageBaseAddress;
    if (!addressSpanEndInclusive(pageBaseAddress, static_cast<size_t>(pageSize), pageEndAddress)) {
        pageEndAddress = UINT64_MAX;
    }
    ImGui::TextColored(ColorScheme::SuccessLight, "页#%llu", pageNumber);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("当前页号: %llu\n页首: 0x%llX\n页范围: 0x%llX - 0x%llX\n页大小: %d 字节", 
                         pageNumber, pageBaseAddress, 
                         pageBaseAddress, pageEndAddress, pageSize);
    }
    
    if (addressInSpan(pageBaseAddress, static_cast<size_t>(pageSize), targetAddress)) {
        ImGui::SameLine();
        uint64_t offsetInPage = targetAddress - pageBaseAddress;
        ImGui::TextColored(ColorScheme::WarningLight, "+0x%llX", offsetInPage);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("页内偏移\n目标: 0x%llX", targetAddress);
        }
    }
    
    // 换行 - 第二行：显示选项
    ImGui::Spacing();
    
    // 页大小设置（以KB为单位显示）
    ImGui::Text("页大小:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    int pageSizeKB = pageSize / 1024;
    if (ImGui::InputInt("##size", &pageSizeKB)) {
        if (pageSizeKB < 1) pageSizeKB = 1;
        if (pageSizeKB > 64) pageSizeKB = 64;
        pageSize = pageSizeKB * 1024;
        viewSize = pageSize;
        buffer.resize(viewSize);
        pageBaseAddress = (targetAddress / pageSize) * pageSize;
        viewAddress = pageBaseAddress;
        refreshMemory();
    }
    ImGui::SameLine();
    ImGui::Text("KB");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("页大小 (范围: 1-64 KB)");
    
    ImGui::SameLine();
    ImGui::Text("字节/行:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    if (ImGui::InputInt("##bpr", &bytesPerRow)) {
        if (bytesPerRow < 8) bytesPerRow = 8;
        if (bytesPerRow > 32) bytesPerRow = 32;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("每行显示字节数");
    
    ImGui::SameLine();
    ImGui::Separator();
    ImGui::SameLine();
    
    // 显示格式选择
    ImGui::Text("格式:");
    ImGui::SameLine();
    const char* formatNames[] = {
        "十六进制 Byte",
        "十六进制 2 Bytes",
        "十六进制 4 Bytes",
        "十六进制 8 Bytes",
        "十进制 Byte",
        "十进制 2 Bytes",
        "十进制 4 Bytes",
        "十进制 8 Bytes",
        "浮点 4 Bytes",
        "双精度 8 Bytes",
        "二进制 Byte"
    };
    ImGui::SetNextItemWidth(140);
    int prevFormat = displayFormat;
    ImGui::Combo("##format", &displayFormat, formatNames, IM_ARRAYSIZE(formatNames));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("显示格式");
    
    // 根据格式自动调整每行字节数
    if (prevFormat != displayFormat) {
        switch ((DisplayFormat)displayFormat) {
            case DisplayFormat::Hex_2Bytes:
            case DisplayFormat::Dec_2Bytes:
                bytesPerRow = 16; break;
            case DisplayFormat::Hex_4Bytes:
            case DisplayFormat::Dec_4Bytes:
            case DisplayFormat::Float_4Bytes:
                bytesPerRow = 16; break;
            case DisplayFormat::Hex_8Bytes:
            case DisplayFormat::Dec_8Bytes:
            case DisplayFormat::Double_8Bytes:
                bytesPerRow = 16; break;
            case DisplayFormat::Binary_Byte:
                bytesPerRow = 8; break;
            default:
                bytesPerRow = 16; break;
        }
    }
    
    ImGui::SameLine();
    ImGui::Separator();
    ImGui::SameLine();
    
    ImGui::Checkbox("自动刷新", &autoRefresh);
    if (autoRefresh) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(50);
        ImGui::InputFloat("##interval", &refreshInterval, 0, 0, "%.1f");
        if (refreshInterval < 0.1f) refreshInterval = 0.1f;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("刷新间隔(秒)");
        ImGui::SameLine();
        ImGui::Text("秒");
    }
    
    ImGui::EndGroup();
    
    // 自动刷新逻辑
    if (autoRefresh) {
        timeSinceRefresh += ImGui::GetIO().DeltaTime;
        if (timeSinceRefresh >= refreshInterval) {
            refreshMemory();
            timeSinceRefresh = 0.0f;
        }
    }
    
    ImGui::Separator();
    
    // 模块 + 偏移链（折叠）
    if (ImGui::CollapsingHeader("模块 + 偏移链", ImGuiTreeNodeFlags_None))
    {
        ImGui::Indent();
        
        // 模块名和基址偏移
        ImGui::Text("模块名:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##module", moduleNameBuf, IM_ARRAYSIZE(moduleNameBuf));
        
        ImGui::Text("基址偏移:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150);
        ImGui::InputText("##baseoffset", baseOffsetBuf, IM_ARRAYSIZE(baseOffsetBuf), ImGuiInputTextFlags_CharsHexadecimal);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("十六进制格式，例如: 1000");
        
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        
        // 偏移链列表
        ImGui::Text("偏移链:");
        ImGui::SameLine();
        if (ImGui::SmallButton("添加偏移")) {
            uint64_t newOffset = 0;
            if (parseHexStrict(newOffsetBuf, newOffset)) {
                offsetChain.push_back(newOffset);
                resetHexInput(newOffsetBuf, sizeof(newOffsetBuf));
            } else {
                Gui::log("无效的偏移值: %s", newOffsetBuf);
            }
        }
        
        ImGui::SameLine();
        if (ImGui::SmallButton("清空偏移链")) {
            offsetChain.clear();
            selectedOffsetIndex = -1;
        }
        
        // 偏移链列表显示
        if (ImGui::BeginChild("OffsetChainList", ImVec2(0, 150), true))
        {
            if (offsetChain.empty()) {
                ImGui::TextDisabled("偏移链为空\n点击'添加偏移'添加偏移项");
            } else {
                // 显示偏移链路径
                ImGui::TextColored(ColorScheme::InfoLight, "偏移链路径:");
                ImGui::SameLine();
                std::string chainPath = moduleNameBuf;
                chainPath += " + 0x" + std::string(baseOffsetBuf);
                for (size_t i = 0; i < offsetChain.size(); i++) {
                    char offsetStr[32];
                    std::snprintf(offsetStr, sizeof(offsetStr), " -> [+0x%llX]", offsetChain[i]);
                    chainPath += offsetStr;
                }
                ImGui::TextWrapped("%s", chainPath.c_str());
                
                ImGui::Separator();
                
                int pendingDeleteOffsetIndex = -1;
                int pendingMoveOffsetIndex = -1;
                int pendingMoveDirection = 0;
                for (size_t i = 0; i < offsetChain.size(); i++) {
                    ImGui::PushID((int)i);
                // 偏移项列表
                    
                    bool isSelected = (selectedOffsetIndex == (int)i);
                    if (isSelected) {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(ColorScheme::Info.x, ColorScheme::Info.y, ColorScheme::Info.z, 0.5f));
                    }
                    
                    char label[64];
                    std::snprintf(label, sizeof(label), "[%d] +0x%llX", (int)i, offsetChain[i]);
                    
                    if (ImGui::Selectable(label, isSelected, ImGuiSelectableFlags_AllowDoubleClick)) {
                        selectedOffsetIndex = (int)i;
                        
                        // 双击编辑
                        if (ImGui::IsMouseDoubleClicked(0)) {
                            std::snprintf(newOffsetBuf, sizeof(newOffsetBuf), "%llX", offsetChain[i]);
                        }
                    }
                    
                    if (isSelected) {
                        ImGui::PopStyleColor();
                    }
                    
                    // 右键菜单 - 使用唯一ID避免断言失败
                    char offset_popup_id[64];
                    snprintf(offset_popup_id, sizeof(offset_popup_id), "OffsetPopup_%d", (int)i);
                    if (ImGui::BeginPopupContextItem(offset_popup_id)) {
                        ImGui::Text("偏移 [%d]: 0x%llX", (int)i, offsetChain[i]);
                        ImGui::Separator();
                        
                        if (ImGui::MenuItem("编辑")) {
                            std::snprintf(newOffsetBuf, sizeof(newOffsetBuf), "%llX", offsetChain[i]);
                            selectedOffsetIndex = (int)i;
                        }
                        
                        if (ImGui::MenuItem("删除")) {
                            pendingDeleteOffsetIndex = (int)i;
                        }
                        
                        if (ImGui::MenuItem("上移", nullptr, false, i > 0)) {
                            pendingMoveOffsetIndex = (int)i;
                            pendingMoveDirection = -1;
                        }
                        
                        if (ImGui::MenuItem("下移", nullptr, false, i < offsetChain.size() - 1)) {
                            pendingMoveOffsetIndex = (int)i;
                            pendingMoveDirection = 1;
                        }
                        
                        ImGui::EndPopup();
                    }
                    
                    ImGui::PopID();
                }

                if (pendingDeleteOffsetIndex >= 0 && pendingDeleteOffsetIndex < (int)offsetChain.size()) {
                    offsetChain.erase(offsetChain.begin() + pendingDeleteOffsetIndex);
                    if (selectedOffsetIndex == pendingDeleteOffsetIndex) {
                        selectedOffsetIndex = -1;
                    } else if (selectedOffsetIndex > pendingDeleteOffsetIndex) {
                        --selectedOffsetIndex;
                    }
                } else if (pendingMoveOffsetIndex >= 0) {
                    int targetIndex = pendingMoveOffsetIndex + pendingMoveDirection;
                    if (targetIndex >= 0 && targetIndex < (int)offsetChain.size()) {
                        std::swap(offsetChain[pendingMoveOffsetIndex], offsetChain[targetIndex]);
                        selectedOffsetIndex = targetIndex;
                    }
                }
            }
        }
        ImGui::EndChild();
        
        // 偏移输入和操作
        ImGui::Text("新增偏移:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        ImGui::InputText("##newoffset", newOffsetBuf, IM_ARRAYSIZE(newOffsetBuf), ImGuiInputTextFlags_CharsHexadecimal);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("十六进制格式，例如: 10, A0, 1F8");
        
        ImGui::SameLine();
        if (ImGui::Button("添加##offset")) {
            uint64_t newOffset = 0;
            if (parseHexStrict(newOffsetBuf, newOffset)) {
                offsetChain.push_back(newOffset);
                resetHexInput(newOffsetBuf, sizeof(newOffsetBuf));
            } else {
                Gui::log("无效的偏移值: %s", newOffsetBuf);
            }
        }
        
        ImGui::SameLine();
        if (selectedOffsetIndex >= 0 && selectedOffsetIndex < (int)offsetChain.size()) {
            if (ImGui::Button("更新选中")) {
                uint64_t updatedOffset = 0;
                if (parseHexStrict(newOffsetBuf, updatedOffset)) {
                    offsetChain[selectedOffsetIndex] = updatedOffset;
                    selectedOffsetIndex = -1;
                    resetHexInput(newOffsetBuf, sizeof(newOffsetBuf));
                } else {
                    Gui::log("无效的偏移值: %s", newOffsetBuf);
                }
            }
            
            ImGui::SameLine();
            if (ImGui::Button("删除选中")) {
                offsetChain.erase(offsetChain.begin() + selectedOffsetIndex);
                selectedOffsetIndex = -1;
            }
        }
        
        ImGui::Spacing();
        ImGui::Checkbox("解引用最终指针", &derefFinal);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("如果启用，将读取最后一个指针指向的值\n如果禁用，返回最后一个指针的地址");
        }

        ImGui::Spacing();
        if (ImGui::Button("解析 & 读取", ImVec2(-1, 0))) {
            uint64_t baseOff = 0;
            uint64_t addr = 0;
            
            if (!parseHexStrict(baseOffsetBuf, baseOff)) {
                Gui::log("无效的基址偏移: %s", baseOffsetBuf);
            } else {
                Mem::PointerResolveRequest request;
                request.moduleName = moduleNameBuf;
                request.baseOffset = baseOff;
                request.offsets = offsetChain;
                request.dereferenceFinal = derefFinal;
                auto response = memService_.resolvePointer(
                    memService_.captureContext(true), request);
                if (!response.ok()) {
                    Gui::log("解析失败 [%s]: %s",
                             Mem::errorCodeName(response.error().code),
                             response.error().message.c_str());
                } else {
                addr = response.value().address;
                jumpToAddress(addr);
                
                // 构建日志信息
                std::string logMsg = "解析成功: " + std::string(moduleNameBuf) + " + 0x" + std::string(baseOffsetBuf);
                for (size_t i = 0; i < offsetChain.size(); i++) {
                    char offsetStr[32];
                    std::snprintf(offsetStr, sizeof(offsetStr), " -> [+0x%llX]", offsetChain[i]);
                    logMsg += offsetStr;
                }
                char resolvedAddrStr[32];
                std::snprintf(resolvedAddrStr, sizeof(resolvedAddrStr), "%llX", (unsigned long long)addr);
                logMsg += " = 0x";
                logMsg += resolvedAddrStr;
                Gui::log("%s", logMsg.c_str());
                }
            }
        }
        
        ImGui::Unindent();
    }

    ImGui::Separator();
    
    // 分割布局：左侧十六进制视图，右侧数据解析器
    float dataInspectorWidth = 250.0f;
    
    ImGui::BeginChild("HexView", ImVec2(-dataInspectorWidth - 5, 0), true);
    drawMemoryHexEditor();
    ImGui::EndChild();
    
    ImGui::SameLine();
    
    ImGui::BeginChild("DataInspector", ImVec2(0, 0), true);
    drawDataInspector();
    ImGui::EndChild();
} 

void MemoryViewerWindow::formatValueString(char* output, size_t outputSize, size_t bufferIndex, DisplayFormat format)
{
    switch (format) {
        case DisplayFormat::Hex_Byte:
            if (bufferIndex < buffer.size()) {
                snprintf(output, outputSize, "%02X", buffer[bufferIndex]);
            } else {
                snprintf(output, outputSize, "  ");
            }
            break;
            
        case DisplayFormat::Hex_2Bytes:
            if (bufferIndex + 1 < buffer.size()) {
                uint16_t val = 0;
                readScalar(buffer, static_cast<int>(bufferIndex), val);
                snprintf(output, outputSize, "%04X", val);
            } else {
                snprintf(output, outputSize, "    ");
            }
            break;
            
        case DisplayFormat::Hex_4Bytes:
            if (bufferIndex + 3 < buffer.size()) {
                uint32_t val = 0;
                readScalar(buffer, static_cast<int>(bufferIndex), val);
                snprintf(output, outputSize, "%08X", val);
            } else {
                snprintf(output, outputSize, "        ");
            }
            break;
            
        case DisplayFormat::Hex_8Bytes:
            if (bufferIndex + 7 < buffer.size()) {
                uint64_t val = 0;
                readScalar(buffer, static_cast<int>(bufferIndex), val);
                snprintf(output, outputSize, "%016llX", val);
            } else {
                snprintf(output, outputSize, "                ");
            }
            break;
            
        case DisplayFormat::Dec_Byte:
            if (bufferIndex < buffer.size()) {
                uint8_t val = buffer[bufferIndex];
                snprintf(output, outputSize, "%3u", val);  // 固定宽度3字符 (0-255)
            } else {
                snprintf(output, outputSize, "   ");
            }
            break;
            
        case DisplayFormat::Dec_2Bytes:
            if (bufferIndex + 1 < buffer.size()) {
                uint16_t val = 0;
                readScalar(buffer, static_cast<int>(bufferIndex), val);
                snprintf(output, outputSize, "%5u", val);  // 固定宽度5字符 (0-65535)
            } else {
                snprintf(output, outputSize, "     ");
            }
            break;
            
        case DisplayFormat::Dec_4Bytes:
            if (bufferIndex + 3 < buffer.size()) {
                uint32_t val = 0;
                readScalar(buffer, static_cast<int>(bufferIndex), val);
                snprintf(output, outputSize, "%10u", val);  // 固定宽度10字符，显示为无符号
            } else {
                snprintf(output, outputSize, "          ");
            }
            break;
            
        case DisplayFormat::Dec_8Bytes:
            if (bufferIndex + 7 < buffer.size()) {
                uint64_t val = 0;
                readScalar(buffer, static_cast<int>(bufferIndex), val);
                snprintf(output, outputSize, "%20llu", val);  // 固定宽度20字符，显示为无符号
            } else {
                snprintf(output, outputSize, "                    ");
            }
            break;
            
        case DisplayFormat::Float_4Bytes:
            if (bufferIndex + 3 < buffer.size()) {
                float val = 0.0f;
                readScalar(buffer, static_cast<int>(bufferIndex), val);
                if (std::isnan(val)) {
                    snprintf(output, outputSize, "%11s", "NaN");
                } else if (std::isinf(val)) {
                    snprintf(output, outputSize, "%11s", val > 0 ? "+Inf" : "-Inf");
                } else if (val == 0.0f) {
                    snprintf(output, outputSize, "%11s", "0.0");
                } else if (std::abs(val) >= 1e6f || (std::abs(val) < 1e-3f && val != 0.0f)) {
                    // 科学计数法
                    snprintf(output, outputSize, "%11.3e", val);
                } else {
                    // 普通格式，右对齐
                    snprintf(output, outputSize, "%11.4f", val);
                }
            } else {
                snprintf(output, outputSize, "           ");
            }
            break;
            
        case DisplayFormat::Double_8Bytes:
            if (bufferIndex + 7 < buffer.size()) {
                double val = 0.0;
                readScalar(buffer, static_cast<int>(bufferIndex), val);
                if (std::isnan(val)) {
                    snprintf(output, outputSize, "%15s", "NaN");
                } else if (std::isinf(val)) {
                    snprintf(output, outputSize, "%15s", val > 0 ? "+Inf" : "-Inf");
                } else if (val == 0.0) {
                    snprintf(output, outputSize, "%15s", "0.0");
                } else if (std::abs(val) >= 1e12 || (std::abs(val) < 1e-6 && val != 0.0)) {
                    // 科学计数法
                    snprintf(output, outputSize, "%15.6e", val);
                } else {
                    // 普通格式，右对齐
                    snprintf(output, outputSize, "%15.6f", val);
                }
            } else {
                snprintf(output, outputSize, "               ");
            }
            break;
            
        case DisplayFormat::Binary_Byte:
            if (bufferIndex < buffer.size()) {
                uint8_t val = buffer[bufferIndex];
                char binary[9];
                for (int i = 0; i < 8; i++) {
                    binary[7 - i] = (val & (1 << i)) ? '1' : '0';
                }
                binary[8] = '\0';
                snprintf(output, outputSize, "%s", binary);
            } else {
                snprintf(output, outputSize, "        ");
            }
            break;
            
        default:
            snprintf(output, outputSize, "??");
            break;
    }
}


void MemoryViewerWindow::drawMemoryHexEditor()
{
    if (buffer.empty()) {
        ImGui::TextDisabled("无数据。设置地址并点击读取。");
        return;
    }
    
    // 根据格式确定每个单元的字节数
    int bytesPerUnit = 1;
    bool showAscii = true;
    
    switch ((DisplayFormat)displayFormat) {
        case DisplayFormat::Hex_Byte:
        case DisplayFormat::Dec_Byte:
        case DisplayFormat::Binary_Byte:
            bytesPerUnit = 1; break;
        case DisplayFormat::Hex_2Bytes:
        case DisplayFormat::Dec_2Bytes:
            bytesPerUnit = 2; break;
        case DisplayFormat::Hex_4Bytes:
        case DisplayFormat::Dec_4Bytes:
        case DisplayFormat::Float_4Bytes:
            bytesPerUnit = 4; break;
        case DisplayFormat::Hex_8Bytes:
        case DisplayFormat::Dec_8Bytes:
        case DisplayFormat::Double_8Bytes:
            bytesPerUnit = 8; break;
    }
    
    // 根据格式计算合适的数据列宽度
    float dataColumnWidth = 0.0f;
    switch ((DisplayFormat)displayFormat) {
        case DisplayFormat::Hex_Byte:
        case DisplayFormat::Dec_Byte:
        case DisplayFormat::Binary_Byte:
            dataColumnWidth = -1.0f; break; // 自动
        case DisplayFormat::Hex_2Bytes:
        case DisplayFormat::Dec_2Bytes:
            dataColumnWidth = -1.0f; break;
        case DisplayFormat::Hex_4Bytes:
        case DisplayFormat::Dec_4Bytes:
        case DisplayFormat::Float_4Bytes:
            dataColumnWidth = -1.0f; break;
        case DisplayFormat::Hex_8Bytes:
        case DisplayFormat::Dec_8Bytes:
        case DisplayFormat::Double_8Bytes:
            dataColumnWidth = -1.0f; break;
        default:
            dataColumnWidth = -1.0f; break;
    }
    
    // 使用表格显示，包含地址列
    int columnCount = showAscii ? 3 : 2;
    bool openAddressContextMenu = false;
    bool openByteContextMenu = false;
    if (ImGui::BeginTable("hexeditor", columnCount, ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV))
    {
        ImGui::TableSetupColumn("地址", ImGuiTableColumnFlags_WidthFixed, 140.0f);
        if (dataColumnWidth > 0) {
            ImGui::TableSetupColumn("数据", ImGuiTableColumnFlags_WidthFixed, dataColumnWidth);
        } else {
            ImGui::TableSetupColumn("数据", ImGuiTableColumnFlags_WidthStretch);
        }
        if (showAscii) {
            ImGui::TableSetupColumn("文本", ImGuiTableColumnFlags_WidthFixed, 200.0f);
        }
        ImGui::TableHeadersRow();
        
        // 检测滚动位置，实现连续滚动
        float scrollY = ImGui::GetScrollY();
        float scrollMaxY = ImGui::GetScrollMaxY();
        // 滚动到底部 - 自动加载下一页
        if (scrollY >= scrollMaxY - 10.0f && scrollMaxY > 0 && !hexAutoScrolling) {
            hexAutoScrolling = true;
            
            // 扩展buffer，读取下一页数据
            size_t currentSize = buffer.size();
            uint64_t safeNextPageAddr = 0;
            const bool hasNextPageAddress =
                addAddressOffset(viewAddress, static_cast<uint64_t>(currentSize), safeNextPageAddr);
            size_t newSize = currentSize + pageSize;
            
            // 限制最大缓冲区大小（例如最多10页）
            if (hasNextPageAddress && newSize <= pageSize * 10) {
                buffer.resize(newSize);
                
                // 读取下一页数据
                uint64_t nextPageAddr = safeNextPageAddr;
                std::vector<unsigned char> nextPageData;
                if (readTargetMemory(
                        nextPageAddr, static_cast<uint32_t>(pageSize),
                        nextPageData, Mem::MemoryReadChannel::Background)) {
                    // 复制数据到buffer末尾
                    std::copy(nextPageData.begin(), nextPageData.end(), buffer.begin() + currentSize);
                    viewSize = newSize;
                    Gui::log("已加载下一页: 0x%llX", nextPageAddr);
                } else {
                    // 读取失败，恢复buffer大小
                    buffer.resize(currentSize);
                }
            }
            
            hexAutoScrolling = false;
        }
        
        // 滚动到顶部 - 自动加载上一页
        if (scrollY <= 10.0f && viewAddress >= pageSize && !hexAutoScrolling) {
            hexAutoScrolling = true;
            
            // 在buffer前面插入数据
            uint64_t prevPageAddr = viewAddress - pageSize;
            std::vector<unsigned char> prevPageData;
            
            // 限制最大缓冲区大小
            if (buffer.size() < pageSize * 10) {
                if (readTargetMemory(
                        prevPageAddr, static_cast<uint32_t>(pageSize),
                        prevPageData, Mem::MemoryReadChannel::Background)) {
                    // 在buffer前面插入数据
                    buffer.insert(buffer.begin(), prevPageData.begin(), prevPageData.end());
                    viewAddress = prevPageAddr;
                    viewSize = buffer.size();
                    
                    // 保持滚动位置（调整scroll以保持视觉连续性）
                    float rowHeight = ImGui::GetTextLineHeightWithSpacing();
                    int rowsPerPage = pageSize / bytesPerRow;
                    ImGui::SetScrollY(scrollY + rowsPerPage * rowHeight);
                    
                    Gui::log("已加载上一页: 0x%llX", prevPageAddr);
                }
            }
            
            hexAutoScrolling = false;
        }
        
        // 在ASCII列标题下方添加文本模式切换
        if (showAscii) {
            ImGui::TableSetColumnIndex(2);
            const char* asciiModes[] = { "ASCII", "UTF-8", "UTF-16" };
            ImGui::SetNextItemWidth(-1);
            ImGui::Combo("##asciimode", &asciiDisplayMode, asciiModes, IM_ARRAYSIZE(asciiModes));
        }
        
        for (size_t row = 0; row < buffer.size(); row += (size_t)bytesPerRow) {
            ImGui::TableNextRow();
            
            // 地址列
            ImGui::TableSetColumnIndex(0);
            uint64_t rowAddress = 0;
            if (!addAddressOffset(viewAddress, static_cast<uint64_t>(row), rowAddress)) {
                break;
            }
            
            // 检查当前行是否包含目标地址
            bool isTargetRow = addressInSpan(rowAddress, static_cast<size_t>(bytesPerRow), targetAddress);
            
            // 如果需要滚动到目标地址，并且当前行包含目标地址
            if (scrollToTarget && isTargetRow) {
                // 滚动到当前行
                ImGui::SetScrollHereY(0.3f);  // 0.3f 表示将目标行放在视口的30%位置
                scrollToTarget = false;  // 重置滚动标记
                
                // 同时选中目标地址对应的字节
                selectedByteOffset = (int)(targetAddress - viewAddress);
                selectedByteAddress = targetAddress;
            }
            
            // 高亮显示包含目标地址的行
            if (isTargetRow) {
                ImGui::TextColored(ColorScheme::WarningLight, "%016llX", rowAddress);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("目标地址所在行\n目标: 0x%llX", targetAddress);
                }
            } else {
                ImGui::TextColored(ColorScheme::InfoLight, "%016llX", rowAddress);
            }
            
            // 右键菜单
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                selectedByteAddress = rowAddress;
                openAddressContextMenu = true;
            }
            
            // 数据列（根据格式显示）
            ImGui::TableSetColumnIndex(1);
            
            // 按单元显示
            int unitsPerRow = bytesPerRow / bytesPerUnit;
            
            // 使用等宽字体以确保对齐
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 4)); // 调整间距
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(3, 2)); // 调整内边距
            
            for (int i = 0; i < unitsPerRow; ++i) {
                size_t idx = row + (size_t)(i * bytesPerUnit);
                if (idx >= buffer.size()) {
                    break;
                }
                
                // 检查是否有足够的字节
                if (idx + bytesPerUnit > buffer.size()) {
                    ImGui::TextDisabled("??");
                    if (i + 1 < unitsPerRow) {
                        ImGui::SameLine();
                    }
                    continue;
                }
                
                // 为每个单元创建可选择的按钮
                ImGui::PushID((int)idx);
                
                bool isSelected = (selectedByteOffset >= (int)idx && selectedByteOffset < (int)(idx + bytesPerUnit));
                
                // 检查当前单元是否包含目标地址
                uint64_t unitAddress = 0;
                const bool hasUnitAddress =
                    addAddressOffset(viewAddress, static_cast<uint64_t>(idx), unitAddress);
                bool isTargetUnit = hasUnitAddress &&
                    addressInSpan(unitAddress, static_cast<size_t>(bytesPerUnit), targetAddress);
                
                // 设置按钮样式
                if (isSelected) {
                    // 选中状态
                    ImGui::PushStyleColor(ImGuiCol_Button, ColorScheme::ButtonSelected);
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ColorScheme::ButtonSelectedHovered);
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ColorScheme::ButtonSelectedActive);
                } else if (isTargetUnit) {
                    // 目标地址 - 高亮
                    ImGui::PushStyleColor(ImGuiCol_Button, ColorScheme::ButtonHighlight);
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ColorScheme::ButtonHighlightHovered);
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ColorScheme::ButtonHighlightActive);
                } else {
                    // 默认状态
                    ImGui::PushStyleColor(ImGuiCol_Button, ColorScheme::ButtonDefault);
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ColorScheme::ButtonDefaultHovered);
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ColorScheme::ButtonDefaultActive);
                }
                
                char valueStr[64];
                formatValueString(valueStr, sizeof(valueStr), idx, (DisplayFormat)displayFormat);
                
                // 根据格式设置按钮最小宽度，确保对齐
                float minButtonWidth = 0.0f;
                switch ((DisplayFormat)displayFormat) {
                    case DisplayFormat::Hex_Byte:
                        minButtonWidth = 30.0f; break;
                    case DisplayFormat::Hex_2Bytes:
                        minButtonWidth = 50.0f; break;
                    case DisplayFormat::Hex_4Bytes:
                        minButtonWidth = 80.0f; break;
                    case DisplayFormat::Hex_8Bytes:
                        minButtonWidth = 140.0f; break;
                    case DisplayFormat::Dec_Byte:
                        minButtonWidth = 40.0f; break;
                    case DisplayFormat::Dec_2Bytes:
                        minButtonWidth = 60.0f; break;
                    case DisplayFormat::Dec_4Bytes:
                        minButtonWidth = 100.0f; break;
                    case DisplayFormat::Dec_8Bytes:
                        minButtonWidth = 180.0f; break;
                    case DisplayFormat::Float_4Bytes:
                        minButtonWidth = 110.0f; break;
                    case DisplayFormat::Double_8Bytes:
                        minButtonWidth = 160.0f; break;
                    case DisplayFormat::Binary_Byte:
                        minButtonWidth = 80.0f; break;
                    default:
                        minButtonWidth = 50.0f; break;
                }
                
                ImVec2 buttonSize(minButtonWidth, 0);
                if (ImGui::Button(valueStr, buttonSize)) {
                    selectedByteOffset = (int)idx;
                    if (hasUnitAddress) {
                        selectedByteAddress = unitAddress;
                    }
                }
                
                ImGui::PopStyleColor(3);
                
                // 右键菜单
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                    selectedByteOffset = (int)idx;
                    if (hasUnitAddress) {
                        selectedByteAddress = unitAddress;
                        openByteContextMenu = true;
                    }
                }
                
                // 双击编辑
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    editMode = true;
                    editingRow = (int)(row / bytesPerRow);
                    editingCol = i;
                    std::snprintf(editBuffer, sizeof(editBuffer), "%02X", buffer[idx]);
                }
                
                ImGui::PopID();
                
                // 每4个单元添加额外间距以便阅读
                if ((i + 1) % 4 == 0 && i + 1 < unitsPerRow) {
                    ImGui::SameLine(0, 12);
                } else if (i + 1 < unitsPerRow) {
                    ImGui::SameLine();
                }
            }
            
            ImGui::PopStyleVar(2);
            
            // 文本列（根据asciiDisplayMode显示不同格式）
            if (showAscii) {
                ImGui::TableSetColumnIndex(2);
                
                switch ((AsciiDisplayMode)asciiDisplayMode) {
                    case AsciiDisplayMode::ASCII: {
                        // ASCII 文本
                        char ascii[256];
                        int n = 0;
                        for (int i = 0; i < bytesPerRow; ++i) {
                            size_t idx = row + (size_t)i;
                            unsigned char c = (idx < buffer.size()) ? buffer[idx] : ' ';
                            ascii[n++] = (c >= 32 && c < 127) ? (char)c : '.';
                        }
                        ascii[n] = '\0';
                        ImGui::TextUnformatted(ascii);
                        break;
                    }
                    
                    case AsciiDisplayMode::UTF8: {
                        // UTF-8 文本
                        std::string utf8Text = readUTF8String(buffer, row, bytesPerRow);
                        ImGui::TextUnformatted(utf8Text.c_str());
                        break;
                    }
                    
                    case AsciiDisplayMode::UTF16: {
                        // UTF-16 文本
                        std::string utf8Text = readUTF16String(buffer, row, bytesPerRow);
                        ImGui::TextUnformatted(utf8Text.c_str());
                        break;
                    }
                }
            }
        }
        
        ImGui::EndTable();
    }
    
    // 上下文菜单
    if (openAddressContextMenu) {
        ImGui::OpenPopup("AddressContextMenu");
    }
    if (ImGui::BeginPopup("AddressContextMenu")) {
        ImGui::Text("地址: 0x%llX", selectedByteAddress);
        ImGui::Separator();

        if (ImGui::MenuItem("复制地址")) {
            char addrStr[32];
            std::snprintf(addrStr, sizeof(addrStr), "%llX", selectedByteAddress);
            ImGui::SetClipboardText(addrStr);
        }

        if (ImGui::MenuItem("跳转到此地址")) {
            jumpToAddress(selectedByteAddress);
        }

        ImGui::EndPopup();
    }

    if (openByteContextMenu) {
        ImGui::OpenPopup("ByteContextMenu");
    }
    if (ImGui::BeginPopup("ByteContextMenu")) {
        ImGui::Text("地址: 0x%llX", selectedByteAddress);
        ImGui::Separator();
        
        if (ImGui::MenuItem("编辑")) {
            if (selectedByteOffset >= 0 && selectedByteOffset < (int)buffer.size()) {
                std::snprintf(editBuffer, sizeof(editBuffer), "%02X", buffer[selectedByteOffset]);
            }
            editMode = true;
        }
        
        if (ImGui::MenuItem("复制地址")) {
            char addrStr[32];
            std::snprintf(addrStr, sizeof(addrStr), "%llX", selectedByteAddress);
            ImGui::SetClipboardText(addrStr);
        }
        
        if (ImGui::MenuItem("复制值")) {
            if (selectedByteOffset >= 0 && selectedByteOffset < (int)buffer.size()) {
                char valueStr[8];
                std::snprintf(valueStr, sizeof(valueStr), "%02X", buffer[selectedByteOffset]);
                ImGui::SetClipboardText(valueStr);
            }
        }
        
        if (ImGui::MenuItem("跳转到此地址")) {
            jumpToAddress(selectedByteAddress);
        }
        
        ImGui::EndPopup();
    }
    
    // 编辑对话框
    if (editMode && selectedByteOffset >= 0 && selectedByteOffset < (int)buffer.size()) {
        ImGui::OpenPopup("编辑字节");
        editMode = false;
    }
    
    if (ImGui::BeginPopupModal("编辑字节", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (selectedByteOffset < 0 || selectedByteOffset >= (int)buffer.size()) {
            ImGui::TextDisabled("选中的字节已失效");
            if (ImGui::Button("关闭", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
        } else {
            ImGui::Text("地址: 0x%llX", selectedByteAddress);
            ImGui::Text("当前值: 0x%02X (%d)", buffer[selectedByteOffset], buffer[selectedByteOffset]);
            ImGui::Separator();

            ImGui::InputText("新值 (十六进制)", editBuffer, sizeof(editBuffer), ImGuiInputTextFlags_CharsHexadecimal);

            if (ImGui::Button("确定", ImVec2(120, 0))) {
                char* end = nullptr;
                unsigned long newValue = std::strtoul(editBuffer, &end, 16);
                if (end != editBuffer && newValue <= 0xFF) {
                    buffer[selectedByteOffset] = (unsigned char)newValue;
                    writeMemoryByte(selectedByteAddress, (unsigned char)newValue);
                }
                ImGui::CloseCurrentPopup();
            }

            ImGui::SameLine();
            if (ImGui::Button("取消", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
        }
        
        ImGui::EndPopup();
    }
}

void MemoryViewerWindow::drawDataInspector()
{
    ImGui::TextColored(ColorScheme::InfoLight, "数据解析器");
    ImGui::Separator();
    
    if (selectedByteOffset < 0 || selectedByteOffset >= (int)buffer.size()) {
        ImGui::TextDisabled("点击十六进制视图中的字节\n以查看数据解析");
        return;
    }
    
    ImGui::Text("地址: 0x%llX", selectedByteAddress);
    ImGui::Text("偏移: +0x%X", selectedByteOffset);
    
    if (ImGui::Button("添加到监控", ImVec2(-1, 0))) {
        ImGui::OpenPopup("选择数据类型");
    }
    
    // 快速添加到监控的弹窗
    if (ImGui::BeginPopup("选择数据类型")) {
        ImGui::Text("选择要监控的数据类型:");
        ImGui::Separator();
        
        const char* quickTypes[] = {
            "BYTE", "WORD", "DWORD", "QWORD",
            "FLOAT", "DOUBLE", "POINTER",
            "STRING", "UTF-8", "UTF-16"
        };
        
        for (int i = 0; i < IM_ARRAYSIZE(quickTypes); i++) {
            if (ImGui::Selectable(quickTypes[i])) {
                MemoryWatchItem item;
                item.description = "监控_" + std::to_string(selectedByteAddress);
                item.address = selectedByteAddress;
                item.type = (FieldType)i;
                item.enabled = true;
                watchItems.push_back(item);
                
                // 立即读取一次值
                if (AppContext::Get().hasProcess()) {
                    watchItems.back().cachedValue = readWatchItemValue(watchItems.back());
                }
                
                Gui::log("已添加 %s 类型监控: 0x%llX, 当前值: %s", 
                        quickTypes[i], selectedByteAddress, watchItems.back().cachedValue.c_str());
            }
        }
        
        ImGui::EndPopup();
    }
    
    ImGui::Separator();
    
    // 创建表格显示各种数据类型
    if (ImGui::BeginTable("DataTypes", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
        ImGui::TableSetupColumn("类型", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("值", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        
        // Byte
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("Byte");
        ImGui::TableSetColumnIndex(1);
        if (selectedByteOffset < (int)buffer.size()) {
            uint8_t val = buffer[selectedByteOffset];
            ImGui::Text("%d (0x%02X)", val, val);
        }
        
        // Int8
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("Int8");
        ImGui::TableSetColumnIndex(1);
        if (selectedByteOffset < (int)buffer.size()) {
            int8_t val = (int8_t)buffer[selectedByteOffset];
            ImGui::Text("%d", val);
        }
        
        // Word (2 bytes)
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("Word");
        ImGui::TableSetColumnIndex(1);
        uint16_t wordVal = 0;
        if (readScalar(buffer, selectedByteOffset, wordVal)) {
            uint16_t val = wordVal;
            ImGui::Text("%u (0x%04X)", val, val);
        } else {
            ImGui::TextDisabled("超出范围");
        }
        
        // Int16
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("Int16");
        ImGui::TableSetColumnIndex(1);
        int16_t int16Val = 0;
        if (readScalar(buffer, selectedByteOffset, int16Val)) {
            int16_t val = int16Val;
            ImGui::Text("%d", val);
        } else {
            ImGui::TextDisabled("超出范围");
        }
        
        // DWord (4 bytes)
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("DWord");
        ImGui::TableSetColumnIndex(1);
        uint32_t dwordVal = 0;
        if (readScalar(buffer, selectedByteOffset, dwordVal)) {
            uint32_t val = dwordVal;
            ImGui::Text("%u (0x%08X)", val, val);
        } else {
            ImGui::TextDisabled("超出范围");
        }
        
        // Int32
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("Int32");
        ImGui::TableSetColumnIndex(1);
        int32_t int32Val = 0;
        if (readScalar(buffer, selectedByteOffset, int32Val)) {
            int32_t val = int32Val;
            ImGui::Text("%d", val);
        } else {
            ImGui::TextDisabled("超出范围");
        }
        
        // QWord (8 bytes)
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("QWord");
        ImGui::TableSetColumnIndex(1);
        uint64_t qwordVal = 0;
        if (readScalar(buffer, selectedByteOffset, qwordVal)) {
            uint64_t val = qwordVal;
            ImGui::Text("%llu (0x%016llX)", val, val);
        } else {
            ImGui::TextDisabled("超出范围");
        }
        
        // Int64
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("Int64");
        ImGui::TableSetColumnIndex(1);
        int64_t int64Val = 0;
        if (readScalar(buffer, selectedByteOffset, int64Val)) {
            int64_t val = int64Val;
            ImGui::Text("%lld", val);
        } else {
            ImGui::TextDisabled("超出范围");
        }
        
        // Float
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("Float");
        ImGui::TableSetColumnIndex(1);
        float floatVal = 0.0f;
        if (readScalar(buffer, selectedByteOffset, floatVal)) {
            float val = floatVal;
            ImGui::Text("%.6f", val);
        } else {
            ImGui::TextDisabled("超出范围");
        }
        
        // Double
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("Double");
        ImGui::TableSetColumnIndex(1);
        double doubleVal = 0.0;
        if (readScalar(buffer, selectedByteOffset, doubleVal)) {
            double val = doubleVal;
            ImGui::Text("%.10f", val);
        } else {
            ImGui::TextDisabled("超出范围");
        }
        
        // String (ASCII)
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("ASCII");
        ImGui::TableSetColumnIndex(1);
        {
            std::string str;
            for (int i = selectedByteOffset; i < (int)buffer.size() && i < selectedByteOffset + 32; i++) {
                if (buffer[i] == 0) break;
                if (buffer[i] >= 32 && buffer[i] < 127) {
                    str += (char)buffer[i];
                } else {
                    break;
                }
            }
            if (!str.empty()) {
                ImGui::Text("\"%s\"", str.c_str());
            } else {
                ImGui::TextDisabled("无效字符串");
            }
        }
        
        // UTF-8 String
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("UTF-8");
        ImGui::TableSetColumnIndex(1);
        {
            std::string str = readUTF8String(buffer, selectedByteOffset, 64);
            if (!str.empty()) {
                // 限制显示长度
                if (str.length() > 40) {
                    str = str.substr(0, 40) + "...";
                }
                ImGui::Text("\"%s\"", str.c_str());
            } else {
                ImGui::TextDisabled("无效字符串");
            }
        }
        
        // UTF-16 String
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("UTF-16");
        ImGui::TableSetColumnIndex(1);
        {
            std::string str = readUTF16String(buffer, selectedByteOffset, 64);
            if (!str.empty()) {
                // 限制显示长度
                if (str.length() > 40) {
                    str = str.substr(0, 40) + "...";
                }
                ImGui::Text("\"%s\"", str.c_str());
            } else {
                ImGui::TextDisabled("无效字符串");
            }
        }
        
        // Binary
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("二进制");
        ImGui::TableSetColumnIndex(1);
        if (selectedByteOffset < (int)buffer.size()) {
            uint8_t val = buffer[selectedByteOffset];
            char binary[9];
            for (int i = 0; i < 8; i++) {
                binary[7 - i] = (val & (1 << i)) ? '1' : '0';
            }
            binary[8] = '\0';
            ImGui::Text("%s", binary);
        }
        
        ImGui::EndTable();
    }
    
    ImGui::Separator();
    
    // 快速操作按钮
    if (ImGui::Button("跟随指针", ImVec2(-1, 0))) {
        uint64_t ptrVal = 0;
        if (readScalar(buffer, selectedByteOffset, ptrVal)) {
            if (ptrVal != 0) {
                jumpToAddress(ptrVal);
            }
        }
    }
}
