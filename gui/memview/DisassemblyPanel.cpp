#include "../MemoryViewerWindow.h"
#include "../AppContext.h"
#include "../DisassemblyHelper.h"
#include "../Gui.h"
#include "../ColorScheme.h"
#include "../../imgui/imgui.h"
#include <cstdint>
#include <cstring>
#include <memory>

std::string MemoryViewerWindow::formatAddressWithOffset(uint64_t address)
{
    std::string baseText = AppContext::Get().moduleCache
        .formatAddressWithModuleAndSymbol(address, memService_);

    char buffer[768];
    snprintf(buffer, sizeof(buffer), "0x%llX [%s]", address, baseText.c_str());
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
        if (!readTargetMemory(
                disassemblyAddress,
                static_cast<uint32_t>(DISASSEMBLY_BUFFER_SIZE),
                disassemblyBuffer)) {
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
        ImGui::TextColored(ColorScheme::Warning, "反汇编引擎未初始化");
        ImGui::TextWrapped("提示: 请确保已安装 Capstone 库");
        ImGui::Separator();
        ImGui::Text("安装方法:");
        ImGui::BulletText("使用 vcpkg: vcpkg install capstone:x64-windows");
        ImGui::BulletText("或从 https://www.capstone-engine.org/ 下载预编译版本");
        return;
    }

    // 检查是否有进程附加
    if (!AppContext::Get().hasProcess()) {
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
            ImGui::TextColored(ColorScheme::ErrorBright, "错误: %s", cachedDisassemblyResult.errorMessage.c_str());
        } else {
            ImGui::TextDisabled("请点击读取或刷新以获取反汇编数据");
        }
        return;
    }

    if (cachedDisassemblyResult.instructions.empty()) {
        ImGui::TextColored(ColorScheme::ErrorBright, "反汇编结果为空");
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
                    addrStr = AppContext::Get().moduleCache
                        .formatAddressWithModuleAndSymbol(
                            insn.address, memService_);
                    // 使用更明显的颜色高亮显示
                    ImGui::PushStyleColor(ImGuiCol_Text, ColorScheme::WarningBright);
                    ImGui::Text("%s", addrStr.c_str());
                    ImGui::PopStyleColor();
                } else {
                    // 其他地址：地址[偏移量]
                    addrStr = formatAddressWithOffset(insn.address);
                    ImGui::TextColored(ColorScheme::InfoBright, "%s", addrStr.c_str());
                }
                
                // 高亮整行背景（如果是高亮地址）
                if (isHighlightAddress) {
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, 
                                         ImGui::ColorConvertFloat4ToU32(ColorScheme::Warning));
                }
                
                // 字节码列
                ImGui::TableSetColumnIndex(1);
                ImGui::TextColored(ColorScheme::DisassemblyHex, "%s", insn.hexBytes.c_str());
                
                // 指令列
                ImGui::TableSetColumnIndex(2);
                if (isHighlightAddress) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ColorScheme::WarningBright);
                }
                ImGui::Text("%s", insn.mnemonic.c_str());
                ImGui::SameLine();
                //黑色展示
                ImGui::TextColored(ColorScheme::TextPrimary, "%s", insn.operands.c_str());
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
