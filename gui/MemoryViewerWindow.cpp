#include "MemoryViewerWindow.h"
#include "Gui.h"
#include "DisassemblyHelper.h"
#include "../imgui/imgui.h"
#include "../socket/client_singleton.h"
#include <algorithm>
#include <cstdint>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <fstream>
#include <cmath>
#include <memory>

// 解析地址表达式，支持十六进制加减运算
// 例如: "1000", "1000+200", "5000-100", "ABCD + 10"
static bool parseAddressExpression(const char* expr, uint64_t& result)
{
    if (!expr || expr[0] == '\0') return false;
    
    std::string s = expr;
    // 去除所有空格
    s.erase(std::remove(s.begin(), s.end(), ' '), s.end());
    
    if (s.empty()) return false;
    
    // 查找+或-运算符（从索引1开始，避免将负号误认为运算符）
    size_t opPos = std::string::npos;
    char op = '\0';
    
    // 从后向前查找最后一个+或-（支持多次运算，从右向左计算）
    for (size_t i = s.length() - 1; i > 0; i--) {
        if (s[i] == '+' || s[i] == '-') {
            opPos = i;
            op = s[i];
            break;
        }
    }
    
    if (opPos == std::string::npos) {
        // 没有运算符，直接解析十六进制数
        char* endPtr = nullptr;
        result = strtoull(s.c_str(), &endPtr, 16);
        return endPtr != s.c_str() && *endPtr == '\0';
    }
    
    // 有运算符，递归解析左右两边
    std::string leftStr = s.substr(0, opPos);
    std::string rightStr = s.substr(opPos + 1);
    
    uint64_t left = 0, right = 0;
    
    // 递归解析左边（支持嵌套表达式）
    if (!parseAddressExpression(leftStr.c_str(), left)) return false;
    
    // 解析右边
    char* endPtr = nullptr;
    right = strtoull(rightStr.c_str(), &endPtr, 16);
    if (endPtr == rightStr.c_str() || *endPtr != '\0') return false;
    
    // 计算结果
    if (op == '+') {
        result = left + right;
    } else if (op == '-') {
        result = left - right;
    }
    
    return true;
}

MemoryViewerWindow::MemoryViewerWindow()
{
    name = "内存查看器";
    loadStructDefinitions();
    
    // 初始化默认地址，避免首次打开时地址为0导致无法显示
    viewAddress = 0;
    pageBaseAddress = 0;
    targetAddress = 0;
    
    // 初始化偏移链输入框
    strcpy(newOffsetBuf, "0");
    
    // 调整buffer大小为一页
    buffer.resize(viewSize);
    // 清空buffer避免显示垃圾数据
    std::fill(buffer.begin(), buffer.end(), 0);
    
    // 初始化反汇编引擎
    disassemblyHelper = std::make_unique<DisassemblyHelper>();
    // 默认使用ARM64架构（Android通常是ARM64）
    if (DisassemblyHelper::isCapstoneAvailable()) {
        disassemblyInitialized = disassemblyHelper->initialize(DisassemblyHelper::Architecture::ARM64);
        if (!disassemblyInitialized) {
            // 如果ARM64失败，尝试ARM32
            disassemblyInitialized = disassemblyHelper->initialize(DisassemblyHelper::Architecture::ARM);
        }
    }
    
    // 初始化反汇编地址输入框
    disassemblyAddressBuf[0] = '\0';
    disassemblyAddress = 0;
    disassemblyBuffer.clear();
    disassemblyBufferValid = false;
    cachedDisassemblyResult = DisassemblyResult();
    scrollToDisassemblyAddress = false;
}

unsigned int MemoryViewerWindow::getWindowFlags() const
{
    return ImGuiWindowFlags_NoDocking;
}

void MemoryViewerWindow::setProcessInfo(int* pid, std::string* processName)
{
    selectedPid = pid;
    selectedName = processName;
}

void MemoryViewerWindow::jumpToAddress(uint64_t address)
{
    // 保存目标地址
    targetAddress = address;
    
    // 计算页首地址（4KB对齐）
    pageBaseAddress = (address / pageSize) * pageSize;
    
    // 设置视图地址为页首
    viewAddress = pageBaseAddress;
    
    // 读取整页数据
    viewSize = pageSize;  // 确保读取一整页
    buffer.resize(viewSize);
    
    if (ReadProcessMemoryBytes(viewAddress, (uint32_t)viewSize, buffer)) {
        // 读取成功
        Gui::log("内存查看器已跳转到地址: 0x%llX (页首: 0x%llX, 页内偏移: 0x%llX)", 
                 address, pageBaseAddress, address - pageBaseAddress);
    } else {
        // 读取失败，清空buffer
        std::fill(buffer.begin(), buffer.end(), 0);
        Gui::log("读取内存失败: 0x%llX", address);
    }
    
    // 添加到历史记录
    addToHistory(address);
    
    // 标记需要滚动到目标地址
    scrollToTarget = true;
    
    pOpen = true;  // 确保窗口打开
    shouldBringToFront = true;  // 标记需要置于前台
}

void MemoryViewerWindow::addToHistory(uint64_t address)
{
    // 如果不是在历史记录末尾，删除后面的记录
    if (historyIndex >= 0 && historyIndex < (int)addressHistory.size() - 1) {
        addressHistory.erase(addressHistory.begin() + historyIndex + 1, addressHistory.end());
    }
    
    // 添加新地址
    addressHistory.push_back(address);
    historyIndex = (int)addressHistory.size() - 1;
    
    // 限制历史记录大小
    if (addressHistory.size() > 50) {
        addressHistory.erase(addressHistory.begin());
        historyIndex--;
    }
}

void MemoryViewerWindow::refreshMemory()
{
    if (selectedPid && *selectedPid != 0 && viewAddress != 0) {
        // 确保buffer大小正确
        buffer.resize(viewSize);
        
        // 使用调试端口进行自动刷新，避免阻塞主端口
        if (!ReadProcessMemoryBytes(viewAddress, (uint32_t)viewSize, buffer, PORT_DEBUG)) {
            // 读取失败时，清空buffer避免显示错误数据
            std::fill(buffer.begin(), buffer.end(), 0);
            Gui::log("刷新内存失败: 0x%llX", viewAddress);
        }
    } else {
        // 没有进程或地址无效时，清空buffer
        buffer.resize(viewSize);
        std::fill(buffer.begin(), buffer.end(), 0);
    }
}

void MemoryViewerWindow::writeMemoryByte(uint64_t address, unsigned char value)
{
    // 检查进程是否附加
    if (!selectedPid || *selectedPid == 0) {
        Gui::log("错误：未附加进程");
        return;
    }
    
    std::vector<unsigned char> data = { value };
    if (WriteProcessMemoryBytes(address, 1, data)) {
        Gui::log("成功写入内存: 0x%llX = 0x%02X", address, value);
        // 写入成功后刷新内存显示
        refreshMemory();
    } else {
        Gui::log("错误：写入内存失败 - 地址 0x%llX", address);
    }
}

void MemoryViewerWindow::onDraw()
{
    if (!pOpen) return;

    if (ImGui::Begin(name.c_str(), &pOpen, ImGuiWindowFlags_None))
    {
        if (selectedPid && *selectedPid != 0) {
            ImGui::TextColored(ImVec4(0.6f, 0.9f, 0.6f, 1.0f), "已附加: %s (PID %d)", 
                selectedName ? selectedName->c_str() : "Unknown", *selectedPid);
        } else {
            ImGui::TextDisabled("未附加进程");
        }
        ImGui::Separator();

        // 添加标签页
        if (ImGui::BeginTabBar("MemoryViewerTabs"))
        {
            if (ImGui::BeginTabItem("内存查看器"))
            {
                drawMemoryViewerPanel();
                ImGui::EndTabItem();
            }
            
            if (ImGui::BeginTabItem("地址列表"))
            {
                drawAddressList();
                ImGui::EndTabItem();
            }
            
            if (ImGui::BeginTabItem("数据结构分析"))
            {
                drawStructAnalyzerPanel();
                ImGui::EndTabItem();
            }
            
            if (ImGui::BeginTabItem("反汇编查看器"))
            {
                drawDisassemblyPanel();
                ImGui::EndTabItem();
            }
            
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}

void MemoryViewerWindow::drawMemoryViewerPanel()
{
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
            historyIndex--;
            uint64_t historyAddr = addressHistory[historyIndex];
            targetAddress = historyAddr;
            pageBaseAddress = (historyAddr / pageSize) * pageSize;
            viewAddress = pageBaseAddress;
            viewSize = pageSize;
            buffer.resize(viewSize);
            ReadProcessMemoryBytes(viewAddress, (uint32_t)viewSize, buffer);
            scrollToTarget = true;
        }
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("后退");
    
    ImGui::SameLine();
    ImGui::BeginDisabled(historyIndex >= (int)addressHistory.size() - 1);
    if (ImGui::ArrowButton("##forward", ImGuiDir_Right)) {
        if (historyIndex < (int)addressHistory.size() - 1) {
            historyIndex++;
            uint64_t historyAddr = addressHistory[historyIndex];
            targetAddress = historyAddr;
            pageBaseAddress = (historyAddr / pageSize) * pageSize;
            viewAddress = pageBaseAddress;
            viewSize = pageSize;
            buffer.resize(viewSize);
            ReadProcessMemoryBytes(viewAddress, (uint32_t)viewSize, buffer);
            scrollToTarget = true;
        }
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("前进");
    
    ImGui::SameLine();
    ImGui::Separator();
    ImGui::SameLine();
    
    // 地址输入 - 支持表达式计算
    ImGui::Text("地址:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200);
    
    // 使用targetAddress作为显示地址（目标地址而不是页首）
    static char addressInputBuf[64] = "";
    static uint64_t lastTargetAddress = 0;
    
    // 当目标地址改变时，更新输入框显示
    if (targetAddress != lastTargetAddress) {
        snprintf(addressInputBuf, sizeof(addressInputBuf), "%llX", targetAddress);
        lastTargetAddress = targetAddress;
    }
    
    if (ImGui::InputText("##addr", addressInputBuf, sizeof(addressInputBuf), ImGuiInputTextFlags_EnterReturnsTrue)) {
        // 解析地址表达式
        uint64_t newAddress = 0;
        if (parseAddressExpression(addressInputBuf, newAddress)) {
            jumpToAddress(newAddress);
            // 更新输入框显示为计算后的地址
            snprintf(addressInputBuf, sizeof(addressInputBuf), "%llX", newAddress);
        } else {
            Gui::log("无效的地址表达式: %s", addressInputBuf);
            // 恢复为上次有效的地址
            snprintf(addressInputBuf, sizeof(addressInputBuf), "%llX", targetAddress);
        }
    }
    
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("支持十六进制运算\n例如: 1000+200, 5000-100\n按回车确认");
    }
    
    ImGui::SameLine();
    if (ImGui::Button("读取")) {
        uint64_t addr = 0;
        if (parseAddressExpression(addressInputBuf, addr)) {
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
        uint64_t newAddress = pageBaseAddress + pageSize;
        jumpToAddress(newAddress);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("向下翻页");
    
    // 页面信息显示
    ImGui::SameLine();
    ImGui::Separator();
    ImGui::SameLine();
    
    uint64_t pageNumber = pageBaseAddress / pageSize;
    ImGui::TextColored(ImVec4(0.7f, 0.9f, 0.7f, 1.0f), "页#%llu", pageNumber);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("当前页号: %llu\n页首: 0x%llX\n页范围: 0x%llX - 0x%llX\n页大小: %d 字节", 
                         pageNumber, pageBaseAddress, 
                         pageBaseAddress, pageBaseAddress + pageSize - 1, pageSize);
    }
    
    if (targetAddress >= pageBaseAddress && targetAddress < pageBaseAddress + pageSize) {
        ImGui::SameLine();
        uint64_t offsetInPage = targetAddress - pageBaseAddress;
        ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.6f, 1.0f), "+0x%llX", offsetInPage);
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
            std::sscanf(newOffsetBuf, "%llx", &newOffset);
            offsetChain.push_back(newOffset);
            memset(newOffsetBuf, 0, sizeof(newOffsetBuf));
            strcpy(newOffsetBuf, "0");
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
                ImGui::TextColored(ImVec4(0.8f, 0.8f, 1.0f, 1.0f), "偏移链路径:");
                ImGui::SameLine();
                std::string chainPath = moduleNameBuf;
                chainPath += " + 0x" + std::string(baseOffsetBuf);
                for (size_t i = 0; i < offsetChain.size(); i++) {
                    char offsetStr[32];
                    sprintf(offsetStr, " -> [+0x%llX]", offsetChain[i]);
                    chainPath += offsetStr;
                }
                ImGui::TextWrapped("%s", chainPath.c_str());
                
                ImGui::Separator();
                
                // 偏移项列表
                for (size_t i = 0; i < offsetChain.size(); i++) {
                    ImGui::PushID((int)i);
                    
                    bool isSelected = (selectedOffsetIndex == (int)i);
                    if (isSelected) {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.5f, 0.8f, 0.5f));
                    }
                    
                    char label[64];
                    sprintf(label, "[%d] +0x%llX", (int)i, offsetChain[i]);
                    
                    if (ImGui::Selectable(label, isSelected, ImGuiSelectableFlags_AllowDoubleClick)) {
                        selectedOffsetIndex = (int)i;
                        
                        // 双击编辑
                        if (ImGui::IsMouseDoubleClicked(0)) {
                            sprintf(newOffsetBuf, "%llX", offsetChain[i]);
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
                            sprintf(newOffsetBuf, "%llX", offsetChain[i]);
                            selectedOffsetIndex = (int)i;
                        }
                        
                        if (ImGui::MenuItem("删除")) {
                            offsetChain.erase(offsetChain.begin() + i);
                            if (selectedOffsetIndex >= (int)offsetChain.size()) {
                                selectedOffsetIndex = -1;
                            }
                        }
                        
                        if (ImGui::MenuItem("上移", nullptr, false, i > 0)) {
                            std::swap(offsetChain[i], offsetChain[i - 1]);
                            selectedOffsetIndex = (int)(i - 1);
                        }
                        
                        if (ImGui::MenuItem("下移", nullptr, false, i < offsetChain.size() - 1)) {
                            std::swap(offsetChain[i], offsetChain[i + 1]);
                            selectedOffsetIndex = (int)(i + 1);
                        }
                        
                        ImGui::EndPopup();
                    }
                    
                    ImGui::PopID();
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
            std::sscanf(newOffsetBuf, "%llx", &newOffset);
            offsetChain.push_back(newOffset);
            memset(newOffsetBuf, 0, sizeof(newOffsetBuf));
            strcpy(newOffsetBuf, "0");
        }
        
        ImGui::SameLine();
        if (selectedOffsetIndex >= 0 && selectedOffsetIndex < (int)offsetChain.size()) {
            if (ImGui::Button("更新选中")) {
                uint64_t updatedOffset = 0;
                std::sscanf(newOffsetBuf, "%llx", &updatedOffset);
                offsetChain[selectedOffsetIndex] = updatedOffset;
                selectedOffsetIndex = -1;
                memset(newOffsetBuf, 0, sizeof(newOffsetBuf));
                strcpy(newOffsetBuf, "0");
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
            std::sscanf(baseOffsetBuf, "%llx", &baseOff);
            uint64_t addr = 0;
            
            if (ResolveModuleOffsetChain(addr, moduleNameBuf, baseOff, offsetChain, derefFinal)) {
                jumpToAddress(addr);
                
                // 构建日志信息
                std::string logMsg = "解析成功: " + std::string(moduleNameBuf) + " + 0x" + std::string(baseOffsetBuf);
                for (size_t i = 0; i < offsetChain.size(); i++) {
                    char offsetStr[32];
                    sprintf(offsetStr, " -> [+0x%llX]", offsetChain[i]);
                    logMsg += offsetStr;
                }
                logMsg += " = 0x" + std::to_string(addr);
                Gui::log("%s", logMsg.c_str());
            } else {
                Gui::log("解析失败 (模块未找到或读取错误)");
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
                ImGui::TextColored(ImVec4(0.6f, 0.9f, 0.6f, 1.0f), "@ 0x%llX", structBaseAddress);
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
                ImGui::TextColored(ImVec4(0.6f, 0.9f, 0.6f, 1.0f), "@ 0x%llX (%d 字节)", structBaseAddress, (int)structBuffer.size());
                
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
                        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.9f, 1.0f), "+0x%X", (int)row);
                        
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
                if (selectedPid && *selectedPid != 0) {
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
                    if (selectedPid && *selectedPid != 0) {
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
            
            // 选择列
            ImGui::TableSetColumnIndex(0);
            bool isSelected = (selectedWatchIndex == (int)i);
            if (ImGui::Selectable("##select", isSelected, ImGuiSelectableFlags_SpanAllColumns)) {
                selectedWatchIndex = (int)i;
            }
            
            // 复选框（启用/禁用）
            ImGui::SameLine();
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
                    }
                }
                ImGui::PopItemWidth();
                
                // 值变化时高亮显示
                static std::vector<std::string> lastValues;
                if (lastValues.size() <= i) lastValues.resize(watchItems.size());
                if (lastValues[i] != value) {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "*");
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
            if (ImGui::Checkbox("冻结", &item.frozen)) {
                if (item.frozen) {
                    item.frozenValue = readWatchItemValue(item);
                }
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
            if (selectedPid && *selectedPid != 0) {
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
    if (!selectedPid || *selectedPid == 0) {
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
        
        // 如果冻结，写入冻结值（保持值不变）
        if (item.frozen && !item.frozenValue.empty()) {
            writeWatchItemValue(item, item.frozenValue);
            item.cachedValue = item.frozenValue;  // 显示冻结值
        } else {
            // 如果未冻结，读取当前值并缓存（只在此处读取，避免每帧读取）
            item.cachedValue = readWatchItemValue(item);
        }
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
        
        size_t frozenValLen = item.frozenValue.size();
        file.write((char*)&frozenValLen, sizeof(frozenValLen));
        if (frozenValLen > 0) {
            file.write(item.frozenValue.c_str(), frozenValLen);
        }
        
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
        item.description.resize(descLen);
        if (!file.read(&item.description[0], descLen)) break;
        
        if (!file.read((char*)&item.address, sizeof(item.address))) break;
        if (!file.read((char*)&item.type, sizeof(item.type))) break;
        if (!file.read((char*)&item.enabled, sizeof(item.enabled))) break;
        if (!file.read((char*)&item.frozen, sizeof(item.frozen))) break;
        
        size_t frozenValLen;
        if (!file.read((char*)&frozenValLen, sizeof(frozenValLen))) break;
        if (frozenValLen > 0) {
            item.frozenValue.resize(frozenValLen);
            if (!file.read(&item.frozenValue[0], frozenValLen)) break;
        }
        
        if (!file.read((char*)&item.isPointer, sizeof(item.isPointer))) break;
        
        size_t offsetCount;
        if (!file.read((char*)&offsetCount, sizeof(offsetCount))) break;
        for (size_t j = 0; j < offsetCount; j++) {
            uint64_t offset;
            if (!file.read((char*)&offset, sizeof(offset))) break;
            item.offsets.push_back(offset);
        }
        
        watchItems.push_back(item);
    }
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
        if (selectedPid && *selectedPid != 0) {
            watchItems.back().cachedValue = readWatchItemValue(watchItems.back());
        }
        
        addedCount++;
    }
    
    Gui::log("已添加 %d 个字段到监控列表", addedCount);
    
    // 触发一次立即更新
    if (selectedPid && *selectedPid != 0 && addedCount > 0) {
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
                uint16_t val = *(uint16_t*)&buffer[bufferIndex];
                snprintf(output, outputSize, "%04X", val);
            } else {
                snprintf(output, outputSize, "    ");
            }
            break;
            
        case DisplayFormat::Hex_4Bytes:
            if (bufferIndex + 3 < buffer.size()) {
                uint32_t val = *(uint32_t*)&buffer[bufferIndex];
                snprintf(output, outputSize, "%08X", val);
            } else {
                snprintf(output, outputSize, "        ");
            }
            break;
            
        case DisplayFormat::Hex_8Bytes:
            if (bufferIndex + 7 < buffer.size()) {
                uint64_t val = *(uint64_t*)&buffer[bufferIndex];
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
                uint16_t val = *(uint16_t*)&buffer[bufferIndex];
                snprintf(output, outputSize, "%5u", val);  // 固定宽度5字符 (0-65535)
            } else {
                snprintf(output, outputSize, "     ");
            }
            break;
            
        case DisplayFormat::Dec_4Bytes:
            if (bufferIndex + 3 < buffer.size()) {
                uint32_t val = *(uint32_t*)&buffer[bufferIndex];
                snprintf(output, outputSize, "%10u", val);  // 固定宽度10字符，显示为无符号
            } else {
                snprintf(output, outputSize, "          ");
            }
            break;
            
        case DisplayFormat::Dec_8Bytes:
            if (bufferIndex + 7 < buffer.size()) {
                uint64_t val = *(uint64_t*)&buffer[bufferIndex];
                snprintf(output, outputSize, "%20llu", val);  // 固定宽度20字符，显示为无符号
            } else {
                snprintf(output, outputSize, "                    ");
            }
            break;
            
        case DisplayFormat::Float_4Bytes:
            if (bufferIndex + 3 < buffer.size()) {
                float val = *(float*)&buffer[bufferIndex];
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
                double val = *(double*)&buffer[bufferIndex];
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
        static bool isAutoScrolling = false;
        
        // 滚动到底部 - 自动加载下一页
        if (scrollY >= scrollMaxY - 10.0f && scrollMaxY > 0 && !isAutoScrolling) {
            isAutoScrolling = true;
            
            // 扩展buffer，读取下一页数据
            size_t currentSize = buffer.size();
            size_t newSize = currentSize + pageSize;
            
            // 限制最大缓冲区大小（例如最多10页）
            if (newSize <= pageSize * 10) {
                buffer.resize(newSize);
                
                // 读取下一页数据
                uint64_t nextPageAddr = viewAddress + currentSize;
                std::vector<unsigned char> nextPageData;
                if (ReadProcessMemoryBytes(nextPageAddr, pageSize, nextPageData, PORT_DEBUG)) {
                    // 复制数据到buffer末尾
                    std::copy(nextPageData.begin(), nextPageData.end(), buffer.begin() + currentSize);
                    viewSize = newSize;
                    Gui::log("已加载下一页: 0x%llX", nextPageAddr);
                } else {
                    // 读取失败，恢复buffer大小
                    buffer.resize(currentSize);
                }
            }
            
            isAutoScrolling = false;
        }
        
        // 滚动到顶部 - 自动加载上一页
        if (scrollY <= 10.0f && viewAddress >= pageSize && !isAutoScrolling) {
            isAutoScrolling = true;
            
            // 在buffer前面插入数据
            uint64_t prevPageAddr = viewAddress - pageSize;
            std::vector<unsigned char> prevPageData;
            
            // 限制最大缓冲区大小
            if (buffer.size() < pageSize * 10) {
                if (ReadProcessMemoryBytes(prevPageAddr, pageSize, prevPageData, PORT_DEBUG)) {
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
            
            isAutoScrolling = false;
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
            uint64_t rowAddress = viewAddress + row;
            
            // 检查当前行是否包含目标地址
            bool isTargetRow = (targetAddress >= rowAddress && targetAddress < rowAddress + bytesPerRow);
            
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
                ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.4f, 1.0f), "%016llX", rowAddress);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("目标地址所在行\n目标: 0x%llX", targetAddress);
                }
            } else {
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.9f, 1.0f), "%016llX", rowAddress);
            }
            
            // 右键菜单
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                ImGui::OpenPopup("AddressContextMenu");
                selectedByteAddress = rowAddress;
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
                uint64_t unitAddress = viewAddress + idx;
                bool isTargetUnit = (targetAddress >= unitAddress && targetAddress < unitAddress + bytesPerUnit);
                
                // 设置按钮样式
                if (isSelected) {
                    // 选中状态 - 蓝色
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.5f, 0.8f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.4f, 0.6f, 0.9f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.2f, 0.4f, 0.7f, 1.0f));
                } else if (isTargetUnit) {
                    // 目标地址 - 黄色高亮
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.5f, 0.2f, 0.6f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.7f, 0.6f, 0.3f, 0.8f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.5f, 0.4f, 0.1f, 0.7f));
                } else {
                    // 默认状态 - 灰色
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.2f, 0.2f, 0.4f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.3f, 0.3f, 0.6f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.25f, 0.25f, 0.25f, 0.5f));
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
                    selectedByteAddress = viewAddress + idx;
                }
                
                ImGui::PopStyleColor(3);
                
                // 右键菜单
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                    ImGui::OpenPopup("ByteContextMenu");
                    selectedByteOffset = (int)idx;
                    selectedByteAddress = viewAddress + idx;
                }
                
                // 双击编辑
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    editMode = true;
                    editingRow = (int)(row / bytesPerRow);
                    editingCol = i;
                    sprintf(editBuffer, "%02X", buffer[idx]);
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
    if (ImGui::BeginPopup("ByteContextMenu")) {
        ImGui::Text("地址: 0x%llX", selectedByteAddress);
        ImGui::Separator();
        
        if (ImGui::MenuItem("编辑")) {
            editMode = true;
        }
        
        if (ImGui::MenuItem("复制地址")) {
            char addrStr[32];
            sprintf(addrStr, "%llX", selectedByteAddress);
            ImGui::SetClipboardText(addrStr);
        }
        
        if (ImGui::MenuItem("复制值")) {
            if (selectedByteOffset >= 0 && selectedByteOffset < (int)buffer.size()) {
                char valueStr[8];
                sprintf(valueStr, "%02X", buffer[selectedByteOffset]);
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
        ImGui::Text("地址: 0x%llX", selectedByteAddress);
        ImGui::Text("当前值: 0x%02X (%d)", buffer[selectedByteOffset], buffer[selectedByteOffset]);
        ImGui::Separator();
        
        ImGui::InputText("新值 (十六进制)", editBuffer, sizeof(editBuffer), ImGuiInputTextFlags_CharsHexadecimal);
        
        if (ImGui::Button("确定", ImVec2(120, 0))) {
            unsigned int newValue = 0;
            if (sscanf(editBuffer, "%x", &newValue) == 1) {
                buffer[selectedByteOffset] = (unsigned char)newValue;
                writeMemoryByte(selectedByteAddress, (unsigned char)newValue);
            }
            ImGui::CloseCurrentPopup();
        }
        
        ImGui::SameLine();
        if (ImGui::Button("取消", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }
        
        ImGui::EndPopup();
    }
}

void MemoryViewerWindow::drawDataInspector()
{
    ImGui::TextColored(ImVec4(0.8f, 0.8f, 1.0f, 1.0f), "数据解析器");
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
                if (selectedPid && *selectedPid != 0) {
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
        if (selectedByteOffset + 1 < (int)buffer.size()) {
            uint16_t val = *(uint16_t*)&buffer[selectedByteOffset];
            ImGui::Text("%u (0x%04X)", val, val);
        } else {
            ImGui::TextDisabled("超出范围");
        }
        
        // Int16
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("Int16");
        ImGui::TableSetColumnIndex(1);
        if (selectedByteOffset + 1 < (int)buffer.size()) {
            int16_t val = *(int16_t*)&buffer[selectedByteOffset];
            ImGui::Text("%d", val);
        } else {
            ImGui::TextDisabled("超出范围");
        }
        
        // DWord (4 bytes)
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("DWord");
        ImGui::TableSetColumnIndex(1);
        if (selectedByteOffset + 3 < (int)buffer.size()) {
            uint32_t val = *(uint32_t*)&buffer[selectedByteOffset];
            ImGui::Text("%u (0x%08X)", val, val);
        } else {
            ImGui::TextDisabled("超出范围");
        }
        
        // Int32
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("Int32");
        ImGui::TableSetColumnIndex(1);
        if (selectedByteOffset + 3 < (int)buffer.size()) {
            int32_t val = *(int32_t*)&buffer[selectedByteOffset];
            ImGui::Text("%d", val);
        } else {
            ImGui::TextDisabled("超出范围");
        }
        
        // QWord (8 bytes)
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("QWord");
        ImGui::TableSetColumnIndex(1);
        if (selectedByteOffset + 7 < (int)buffer.size()) {
            uint64_t val = *(uint64_t*)&buffer[selectedByteOffset];
            ImGui::Text("%llu (0x%016llX)", val, val);
        } else {
            ImGui::TextDisabled("超出范围");
        }
        
        // Int64
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("Int64");
        ImGui::TableSetColumnIndex(1);
        if (selectedByteOffset + 7 < (int)buffer.size()) {
            int64_t val = *(int64_t*)&buffer[selectedByteOffset];
            ImGui::Text("%lld", val);
        } else {
            ImGui::TextDisabled("超出范围");
        }
        
        // Float
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("Float");
        ImGui::TableSetColumnIndex(1);
        if (selectedByteOffset + 3 < (int)buffer.size()) {
            float val = *(float*)&buffer[selectedByteOffset];
            ImGui::Text("%.6f", val);
        } else {
            ImGui::TextDisabled("超出范围");
        }
        
        // Double
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("Double");
        ImGui::TableSetColumnIndex(1);
        if (selectedByteOffset + 7 < (int)buffer.size()) {
            double val = *(double*)&buffer[selectedByteOffset];
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
        if (selectedByteOffset + 7 < (int)buffer.size()) {
            uint64_t ptrVal = *(uint64_t*)&buffer[selectedByteOffset];
            if (ptrVal != 0) {
                jumpToAddress(ptrVal);
            }
        }
    }
}

// 刷新模块列表
void MemoryViewerWindow::refreshModuleList()
{
    if (!selectedPid || *selectedPid != 0) {
        moduleListValid = false;
        return;
    }
    
    std::vector<ModuleInfoItem> newModules;
    if (FetchModuleList(newModules, PORT_MAIN)) {
        moduleList = std::move(newModules);
        moduleListValid = true;
        lastModuleRefreshTime = ImGui::GetTime();
    } else {
        moduleListValid = false;
    }
}

// 根据地址查找对应的模块
const ModuleInfoItem* MemoryViewerWindow::findModuleByAddress(uint64_t address)
{
    // 如果模块列表无效或为空，且已附加进程，则按需刷新（延迟加载）
    if ((!moduleListValid || moduleList.empty()) && selectedPid && *selectedPid != 0) {
        refreshModuleList();
    }
    
    // 如果仍然无效或为空，返回nullptr
    if (!moduleListValid || moduleList.empty()) {
        return nullptr;
    }
    
    // 在模块列表中查找地址所在的范围
    for (const auto& module : moduleList) {
        if (address >= module.base && address < (module.base + module.size)) {
            return &module;
        }
    }
    
    return nullptr;
}

// 格式化地址显示：模块名+偏移量=地址（用于高亮地址）
std::string MemoryViewerWindow::formatAddressWithModule(uint64_t address)
{
    const ModuleInfoItem* module = findModuleByAddress(address);
    
    char buffer[256];
    if (module) {
        uint64_t offset = address - module->base;
        // 提取模块名（如果路径很长，只显示文件名）
        std::string moduleName = module->name;
        size_t lastSlash = moduleName.find_last_of("/\\");
        if (lastSlash != std::string::npos && lastSlash + 1 < moduleName.length()) {
            moduleName = moduleName.substr(lastSlash + 1);
        }
        
        if (moduleName.empty()) {
            moduleName = "未知模块";
        }
        
        snprintf(buffer, sizeof(buffer), "%s+0x%llX=0x%llX", 
                moduleName.c_str(), offset, address);
    } else {
        // 如果找不到模块，只显示地址
        snprintf(buffer, sizeof(buffer), "0x%llX", address);
    }
    
    return std::string(buffer);
}

// 格式化地址显示：地址[偏移量]（用于其他地址）
std::string MemoryViewerWindow::formatAddressWithOffset(uint64_t address)
{
    const ModuleInfoItem* module = findModuleByAddress(address);
    
    char buffer[256];
    if (module) {
        uint64_t offset = address - module->base;
        snprintf(buffer, sizeof(buffer), "0x%llX[+0x%llX]", address, offset);
    } else {
        // 如果找不到模块，只显示地址
        snprintf(buffer, sizeof(buffer), "0x%llX", address);
    }
    
    return std::string(buffer);
}

// 反汇编查看器面板
void MemoryViewerWindow::drawDisassemblyPanel()
{
    // 如果反汇编地址为0但targetAddress不为0，使用targetAddress（仅设置地址，不自动读取）
    if (disassemblyAddress == 0 && targetAddress != 0) {
        disassemblyAddress = targetAddress;
        snprintf(disassemblyAddressBuf, sizeof(disassemblyAddressBuf), "%llX", disassemblyAddress);
        scrollToDisassemblyAddress = true;  // 标记需要滚动到新地址
        // 清空旧的缓存，等待用户点击读取/刷新
        disassemblyFailed = false;
        disassemblyFailedAddress = 0;
        disassemblyBuffer.clear();
        disassemblyBufferValid = false;
        cachedDisassemblyResult = DisassemblyResult();
    }
    
    // 工具栏 - 与内存查看器一致
    ImGui::BeginGroup();
    
    // 导航按钮
    ImGui::BeginDisabled(historyIndex <= 0);
    if (ImGui::ArrowButton("##disasm_back", ImGuiDir_Left)) {
        if (historyIndex > 0) {
            historyIndex--;
            uint64_t historyAddr = addressHistory[historyIndex];
            disassemblyAddress = historyAddr;
            snprintf(disassemblyAddressBuf, sizeof(disassemblyAddressBuf), "%llX", historyAddr);
            scrollToDisassemblyAddress = true;  // 标记需要滚动到新地址
            // 清除缓存，等待用户点击读取/刷新
            disassemblyFailed = false;
            disassemblyFailedAddress = 0;
            disassemblyBuffer.clear();
            disassemblyBufferValid = false;
            cachedDisassemblyResult = DisassemblyResult();
        }
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("后退");
    
    ImGui::SameLine();
    ImGui::BeginDisabled(historyIndex >= (int)addressHistory.size() - 1);
    if (ImGui::ArrowButton("##disasm_forward", ImGuiDir_Right)) {
        if (historyIndex < (int)addressHistory.size() - 1) {
            historyIndex++;
            uint64_t historyAddr = addressHistory[historyIndex];
            disassemblyAddress = historyAddr;
            snprintf(disassemblyAddressBuf, sizeof(disassemblyAddressBuf), "%llX", historyAddr);
            scrollToDisassemblyAddress = true;  // 标记需要滚动到新地址
            // 清除缓存，等待用户点击读取/刷新
            disassemblyFailed = false;
            disassemblyFailedAddress = 0;
            disassemblyBuffer.clear();
            disassemblyBufferValid = false;
            cachedDisassemblyResult = DisassemblyResult();
        }
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("前进");
    
    ImGui::SameLine();
    ImGui::Separator();
    ImGui::SameLine();
    
    // 地址输入
    ImGui::Text("地址:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200);
    
    if (ImGui::InputText("##disasm_addr", disassemblyAddressBuf, sizeof(disassemblyAddressBuf), 
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
        // 解析地址表达式
        uint64_t newAddress = 0;
        if (parseAddressExpression(disassemblyAddressBuf, newAddress)) {
            disassemblyAddress = newAddress;
            addToHistory(newAddress);
            scrollToDisassemblyAddress = true;  // 标记需要滚动到新地址
            // 清除缓存，等待用户点击读取/刷新
            disassemblyFailed = false;
            disassemblyFailedAddress = 0;
            disassemblyBuffer.clear();
            disassemblyBufferValid = false;
            cachedDisassemblyResult = DisassemblyResult();
        } else {
            Gui::log("无效的地址表达式: %s", disassemblyAddressBuf);
            // 恢复为上次有效的地址
            snprintf(disassemblyAddressBuf, sizeof(disassemblyAddressBuf), "%llX", disassemblyAddress);
        }
    }
    
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("支持十六进制运算\n例如: 1000+200, 5000-100\n按回车确认");
    }
    
    // 仅在点击“读取”或“刷新”时读取并反汇编
    auto performReadAndDisassemble = [&](uint64_t addr) {
        // 准备状态
        disassemblyAddress = addr;
        disassemblyFailed = false;
        disassemblyFailedAddress = 0;
        disassemblyBufferValid = false;
        cachedDisassemblyResult = DisassemblyResult();

        // 校验反汇编引擎
        if (!disassemblyInitialized || !disassemblyHelper) {
            cachedDisassemblyResult.success = false;
            cachedDisassemblyResult.errorMessage = "反汇编引擎未初始化";
            return;
        }

        disassemblyBuffer.resize(DISASSEMBLY_BUFFER_SIZE);
        if (!ReadProcessMemoryBytes(disassemblyAddress, DISASSEMBLY_BUFFER_SIZE, disassemblyBuffer)) {
            disassemblyBuffer.clear();
            cachedDisassemblyResult.success = false;
            char msg[128];
            snprintf(msg, sizeof(msg), "读取内存失败: 0x%llX", disassemblyAddress);
            cachedDisassemblyResult.errorMessage = msg;
            Gui::log("%s", msg);
            return;
        }

        if (disassemblyBuffer.empty()) {
            disassemblyBufferValid = false;
            cachedDisassemblyResult.success = false;
            char msg[128];
            snprintf(msg, sizeof(msg), "读取内存为空: 0x%llX", disassemblyAddress);
            cachedDisassemblyResult.errorMessage = msg;
            Gui::log("%s", msg);
            return;
        }

        disassemblyBufferValid = true;
        cachedDisassemblyResult = disassemblyHelper->disassembleMultiple(
            disassemblyAddress,
            disassemblyBuffer.data(),
            disassemblyBuffer.size(),
            0
        );

        if (!cachedDisassemblyResult.success) {
            // 保持 errorMessage
            disassemblyFailed = true;
            disassemblyFailedAddress = disassemblyAddress;
            if (cachedDisassemblyResult.errorMessage.empty()) {
                cachedDisassemblyResult.errorMessage = "反汇编失败";
            }
            return;
        }

        if (cachedDisassemblyResult.instructions.empty()) {
            cachedDisassemblyResult.success = false;
            cachedDisassemblyResult.errorMessage = "反汇编结果为空";
            disassemblyFailed = true;
            disassemblyFailedAddress = disassemblyAddress;
            return;
        }
    };

    ImGui::SameLine();
    if (ImGui::Button("读取")) {
        uint64_t addr = 0;
        if (parseAddressExpression(disassemblyAddressBuf, addr)) {
            disassemblyAddress = addr;
            addToHistory(addr);
            scrollToDisassemblyAddress = true;  // 标记需要滚动到新地址
            performReadAndDisassemble(addr);
        }
    }
    
    ImGui::SameLine();
    if (ImGui::Button("刷新")) {
        if (disassemblyAddress != 0) {
            scrollToDisassemblyAddress = true;
            performReadAndDisassemble(disassemblyAddress);
        }
    }

    // 简化逻辑：去掉自动刷新入口，避免自动读取
    
    ImGui::EndGroup();
    
    ImGui::Separator();
    
    // 检查反汇编引擎是否可用
    if (!disassemblyInitialized || !disassemblyHelper) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "反汇编引擎未初始化");
        ImGui::TextWrapped("提示: 请确保已安装 Capstone 库");
        ImGui::Separator();
        ImGui::Text("安装方法:");
        ImGui::BulletText("使用 vcpkg: vcpkg install capstone:x64-windows");
        ImGui::BulletText("或从 https://www.capstone-engine.org/ 下载预编译版本");
        return;
    }

    // 检查是否有进程附加
    if (!selectedPid || *selectedPid == 0) {
        ImGui::TextDisabled("未附加进程");
        return;
    }
    
    // 如果没有地址，提示用户输入地址
    if (disassemblyAddress == 0) {
        ImGui::TextDisabled("请输入地址并点击读取按钮");
        return;
    }

    // 渲染结果（不在这里触发任何读取或反汇编）
    if (!cachedDisassemblyResult.success) {
        if (!cachedDisassemblyResult.errorMessage.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "错误: %s", cachedDisassemblyResult.errorMessage.c_str());
        } else {
            ImGui::TextDisabled("请点击读取或刷新以获取反汇编数据");
        }
        return;
    }

    if (cachedDisassemblyResult.instructions.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "反汇编结果为空");
        return;
    }
    
    // 显示架构信息
    ImGui::Text("架构: %s", DisassemblyHelper::getArchitectureName(disassemblyHelper->getCurrentArchitecture()).c_str());
    ImGui::SameLine();
    ImGui::Text("指令数量: %d", (int)cachedDisassemblyResult.instructions.size());
    ImGui::SameLine();
    ImGui::TextDisabled("(缓冲区: %d KB)", (int)(DISASSEMBLY_BUFFER_SIZE / 1024));
    ImGui::Separator();
    
    // 使用表格显示反汇编指令，支持滚动
    // 使用 ImGuiTableFlags_ScrollY 和 ImVec2(0, -1) 来启用垂直滚动
    if (ImGui::BeginTable("DisassemblyTable", 3, 
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | 
                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit |
                          ImGuiTableFlags_Resizable,
                          ImVec2(0, -1))) {
        ImGui::TableSetupColumn("地址", ImGuiTableColumnFlags_WidthFixed, 250.0f);
        ImGui::TableSetupColumn("字节码", ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableSetupColumn("指令", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupScrollFreeze(0, 1);  // 冻结表头
        ImGui::TableHeadersRow();
        
        // 查找高亮地址在指令列表中的索引
        int highlightIndex = -1;
        if (disassemblyAddress != 0) {
            for (size_t i = 0; i < cachedDisassemblyResult.instructions.size(); i++) {
                if (cachedDisassemblyResult.instructions[i].address == disassemblyAddress) {
                    highlightIndex = (int)i;
                    break;
                }
            }
        }
        
        // 如果需要滚动到高亮地址，在渲染前计算并设置滚动位置
        if (scrollToDisassemblyAddress && highlightIndex >= 0) {
            int totalRows = (int)cachedDisassemblyResult.instructions.size();
            
            // 如果高亮地址在列表的第一行（index = 0），不自动滚动
            // 让用户手动滚动，这样可以向上滚动查看前面的内容
            if (highlightIndex == 0) {
                // 不设置滚动位置，保持当前滚动状态（通常是顶部）
                // 这样用户可以手动向上滚动
                scrollToDisassemblyAddress = false;
            } else {
                // 估算每行的高度（包括间距）
                float rowHeight = ImGui::GetTextLineHeightWithSpacing();
                
                // 获取表格的可见区域高度
                ImVec2 tableSize = ImGui::GetContentRegionAvail();
                float visibleHeight = tableSize.y - ImGui::GetTextLineHeightWithSpacing(); // 减去表头高度
                
                // 计算可见行数
                int visibleRows = (int)(visibleHeight / rowHeight);
                if (visibleRows < 1) visibleRows = 1;
                
                // 获取当前滚动位置
                float currentScrollY = ImGui::GetScrollY();
                
                // 计算高亮地址所在行的Y位置
                float highlightRowY = highlightIndex * rowHeight;
                
                // 计算高亮地址是否已经在可见区域内
                float visibleStartY = currentScrollY;
                float visibleEndY = currentScrollY + visibleHeight;
                bool isVisible = (highlightRowY >= visibleStartY && highlightRowY < visibleEndY);
                
                // 如果高亮地址已经在可见区域内，不需要滚动
                if (!isVisible) {
                    // 智能滚动策略：
                    // - 如果高亮地址在开头（前10%但不包括第一行），将高亮地址放在视口的30%位置
                    // - 如果高亮地址在末尾（后10%），滚动到底部
                    // - 否则滚动到中心
                    float scrollRatio = 0.5f;
                    if (highlightIndex < totalRows * 0.1f && highlightIndex > 0) {
                        // 在开头（但不是第一行），将高亮地址放在视口的30%位置
                        scrollRatio = 0.3f;
                    } else if (highlightIndex >= totalRows * 0.9f) {
                        // 在末尾，滚动到底部
                        scrollRatio = 1.0f;
                    } else {
                        // 在中间，滚动到中心
                        scrollRatio = 0.5f;
                    }
                    
                    // 计算目标滚动位置
                    // 公式：targetScrollY = highlightRowY - visibleHeight * scrollRatio
                    float targetScrollY = highlightRowY - visibleHeight * scrollRatio;
                    if (targetScrollY < 0) targetScrollY = 0;
                    
                    // 获取最大滚动位置，确保不会超出范围
                    float maxScrollY = (totalRows - visibleRows) * rowHeight;
                    if (maxScrollY < 0) maxScrollY = 0;
                    if (targetScrollY > maxScrollY) targetScrollY = maxScrollY;
                    
                    // 设置滚动位置
                    ImGui::SetScrollY(targetScrollY);
                }
                
                scrollToDisassemblyAddress = false;  // 重置滚动标记
            }
        }
        
        // 使用Clipper来优化大量行的渲染性能
        ImGuiListClipper clipper;
        clipper.Begin((int)cachedDisassemblyResult.instructions.size());
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
                const auto& insn = cachedDisassemblyResult.instructions[i];
                
                ImGui::TableNextRow();
                
                // 地址列 - 高亮地址使用模块名+偏移量=地址，其他地址使用地址[偏移量]
                ImGui::TableSetColumnIndex(0);
                std::string addrStr;
                bool isHighlightAddress = (insn.address == disassemblyAddress);
                if (isHighlightAddress) {
                    // 高亮地址：模块名+偏移量=地址
                    addrStr = formatAddressWithModule(insn.address);
                    // 使用更明显的颜色高亮显示
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.9f, 0.3f, 1.0f));
                    ImGui::Text("%s", addrStr.c_str());
                    ImGui::PopStyleColor();
                } else {
                    // 其他地址：地址[偏移量]
                    addrStr = formatAddressWithOffset(insn.address);
                    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "%s", addrStr.c_str());
                }
                
                // 高亮整行背景（如果是高亮地址）
                if (isHighlightAddress) {
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, 
                                         ImGui::ColorConvertFloat4ToU32(ImVec4(0.3f, 0.2f, 0.1f, 0.3f)));
                }
                
                // 字节码列
                ImGui::TableSetColumnIndex(1);
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s", insn.hexBytes.c_str());
                
                // 指令列
                ImGui::TableSetColumnIndex(2);
                if (isHighlightAddress) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.9f, 0.3f, 1.0f));
                }
                ImGui::Text("%s", insn.mnemonic.c_str());
                ImGui::SameLine();
                //黑色展示
                ImGui::TextColored(ImVec4(0.0f, 0.0f, 0.0f, 1.0f), "%s", insn.operands.c_str());
                if (isHighlightAddress) {
                    ImGui::PopStyleColor();
                }
                
                // 如果点击地址，可以跳转到该地址
                if (ImGui::IsItemClicked(0)) {
                    // 可以在这里添加跳转功能
                }
            }
        }
        
        ImGui::EndTable();
    }
} 