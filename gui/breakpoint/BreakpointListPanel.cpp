#include "../BreakpointWindow.h"
#include "../MemoryViewerWindow.h"
#include "../Gui.h"
#include "../AppContext.h"
#include "../ColorScheme.h"
#include "../../imgui/imgui.h"
#include "../../socket/client_singleton.h"
#include <algorithm>
#include <cstring>

void BreakpointWindow::drawBreakpointControls()
{
    if (ImGui::Button("添加断点")) {
        showAddBreakpointDialog = true;
    }

    ImGui::SameLine();
    if (ImGui::Button("清除所有断点")) {
        for (int i = breakpoints.size() - 1; i >= 0; i--) {
            removeBreakpoint(i);
        }
        refreshAllDetailWindows();
    }

    ImGui::SameLine();
    if (ImGui::Button("刷新所有断点")) {
        for (int i = 0; i < (int)breakpoints.size(); i++) {
            if (breakpoints[i].enabled) {
                refreshBreakpointHitInfo(i);
            }
        }
    }

    ImGui::SameLine();
    ImGui::Checkbox("自动刷新", &autoRefreshHitInfo);

    if (autoRefreshHitInfo) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        ImGui::SliderFloat("刷新间隔", &refreshInterval, 0.5f, 5.0f, "%.1fs");
    }
}

void BreakpointWindow::drawBreakpointList()
{
    ImGui::Text("断点列表 (%d个)", (int)breakpoints.size());

    if (ImGui::BeginTable("BreakpointTable", 9, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY))
    {
        ImGui::TableSetupColumn("启用", ImGuiTableColumnFlags_WidthFixed, 50);
        ImGui::TableSetupColumn("地址", ImGuiTableColumnFlags_WidthFixed, 120);
        ImGui::TableSetupColumn("类型", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("大小", ImGuiTableColumnFlags_WidthFixed, 50);
        ImGui::TableSetupColumn("状态", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("命中", ImGuiTableColumnFlags_WidthFixed, 50);
        ImGui::TableSetupColumn("PC数", ImGuiTableColumnFlags_WidthFixed, 50);
        ImGui::TableSetupColumn("描述", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("操作", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < breakpoints.size(); i++)
        {
            auto& bp = breakpoints[i];
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::PushID((int)i);

            bool enabled = bp.enabled;
            if (ImGui::Checkbox("##enabled", &enabled)) {
                toggleBreakpoint((int)i);
            }

            ImGui::TableSetColumnIndex(1);
            char addrStr[32];
            snprintf(addrStr, sizeof(addrStr), "0x%llX", bp.address);
            if (ImGui::Selectable(addrStr, false, ImGuiSelectableFlags_SpanAllColumns)) {
                // 双击打开详情窗口
                if (ImGui::IsMouseDoubleClicked(0)) {
                    openBreakpointDetailWindow((int)i);
                }
            }

            // 右键菜单 - 使用唯一ID避免断言失败
            char bp_popup_id[64];
            snprintf(bp_popup_id, sizeof(bp_popup_id), "BPPopup_%zu", i);
            if (ImGui::BeginPopupContextItem(bp_popup_id)) {
                if (ImGui::MenuItem("删除断点")) {
                    removeBreakpoint((int)i);
                    ImGui::EndPopup();
                    ImGui::PopID();
                    break;
                }
                if (bp.enabled) {
                    if (bp.suspended) {
                        if (ImGui::MenuItem("恢复断点")) {
                            resumeBreakpoint((int)i);
                        }
                    } else {
                        if (ImGui::MenuItem("暂停断点")) {
                            suspendBreakpoint((int)i);
                        }
                    }
                }
                if (ImGui::MenuItem("刷新命中信息")) {
                    refreshBreakpointHitInfo((int)i);
                }
                if (ImGui::MenuItem("查看详细信息")) {
                    Gui::log("用户点击查看详细信息，断点索引: %d", (int)i);
                    openBreakpointDetailWindow((int)i);
                }
                ImGui::EndPopup();
            }

            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%s", getBreakpointTypeName(bp.type));

            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%s", getBreakpointSizeName(bp.size));

            ImGui::TableSetColumnIndex(4);
            if (!bp.enabled) {
                ImGui::TextDisabled("禁用");
            } else if (bp.suspended) {
                ImGui::TextColored(ColorScheme::Warning, "暂停");
            } else {
                ImGui::TextColored(ColorScheme::Success, "活动");
            }

            ImGui::TableSetColumnIndex(5);
            ImGui::Text("%d", bp.hitCount);

            ImGui::TableSetColumnIndex(6);
            int pcCount = (int)bp.pcHitStats.size();
            if (pcCount > 0) {
                ImGui::TextColored(ColorScheme::SuccessLight, "%d", pcCount);
                if (ImGui::IsItemHovered()) {
                    // 显示热点PC信息
                    if (!bp.pcHitStats.empty()) {
                        auto maxHit = std::max_element(bp.pcHitStats.begin(), bp.pcHitStats.end(),
                            [](const auto& a, const auto& b) { return a.second.hit_count < b.second.hit_count; });
                        float maxHitRate = bp.hitCount > 0 ? (float)maxHit->second.hit_count / bp.hitCount * 100.0f : 0.0f;
                        std::string maxHitAddrStr = AppContext::Get().moduleCache.formatWithModule(maxHit->first);
                        ImGui::SetTooltip("不同PC地址: %d个\n热点PC: %s (0x%llX)\n热点命中: %d次 (%.1f%%)",
                                         pcCount, maxHitAddrStr.c_str(), maxHit->first, maxHit->second.hit_count, maxHitRate);
                    }
                }
            } else {
                ImGui::TextDisabled("0");
            }

            ImGui::TableSetColumnIndex(7);
            ImGui::Text("%s", bp.description.c_str());

            ImGui::TableSetColumnIndex(8);
            char buttonId[32];
            snprintf(buttonId, sizeof(buttonId), "详情##%d", (int)i);
            if (ImGui::SmallButton(buttonId)) {
                Gui::log("用户点击详情按钮，断点索引: %d", (int)i);
                openBreakpointDetailWindow((int)i);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("打开断点详情窗口");
            }

            ImGui::PopID();
        }

        ImGui::EndTable();
    }
}



void BreakpointWindow::drawAddBreakpointDialog()
{
    if (ImGui::Begin("添加断点", &showAddBreakpointDialog, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("断点地址:");
        ImGui::InputText("##address", newBreakpointAddress, sizeof(newBreakpointAddress), ImGuiInputTextFlags_CharsHexadecimal);

        ImGui::Text("断点类型:");
        const char* breakpointTypes[] = { "只读", "写入", "读写", "执行" };
        ImGui::Combo("##type", &newBreakpointType, breakpointTypes, 4);

        ImGui::Text("断点大小:");
        const char* breakpointSizes[] = { "1字节", "2字节", "4字节", "8字节" };
        ImGui::Combo("##size", &newBreakpointSize, breakpointSizes, 4);

        ImGui::Text("描述:");
        ImGui::InputText("##description", newBreakpointDescription, sizeof(newBreakpointDescription));

        ImGui::Separator();

        if (ImGui::Button("添加")) {
            uint64_t address = 0;
            if (sscanf(newBreakpointAddress, "%llx", &address) == 1) {
                BreakpointType type = (BreakpointType)(newBreakpointType+1);
                BreakpointSize size = (BreakpointSize)(1 << newBreakpointSize);

                addBreakpoint(address, type, size, newBreakpointDescription);

                // 清空输入
                memset(newBreakpointAddress, 0, sizeof(newBreakpointAddress));
                memset(newBreakpointDescription, 0, sizeof(newBreakpointDescription));
                newBreakpointType = 0;
                newBreakpointSize = 0;

                showAddBreakpointDialog = false;
            } else {
                Gui::log("无效的地址格式");
            }
        }

        ImGui::SameLine();
        if (ImGui::Button("取消")) {
            showAddBreakpointDialog = false;
        }
    }
    ImGui::End();
}

const char* BreakpointWindow::getBreakpointTypeName(BreakpointType type)
{
    switch (type) {
        case BreakpointType::HW_BREAKPOINT_R: return "只读";
        case BreakpointType::HW_BREAKPOINT_W: return "写入";
        case BreakpointType::HW_BREAKPOINT_RW: return "读写";
        case BreakpointType::HW_BREAKPOINT_X: return "执行";
        default: return "未知";
    }
}

const char* BreakpointWindow::getBreakpointSizeName(BreakpointSize size)
{
    switch (size) {
        case BreakpointSize::SIZE_1: return "1B";
        case BreakpointSize::SIZE_2: return "2B";
        case BreakpointSize::SIZE_4: return "4B";
        case BreakpointSize::SIZE_8: return "8B";
        default: return "?";
    }
}

void BreakpointWindow::addBreakpoint(uint64_t address, BreakpointType type, BreakpointSize size, const std::string& description)
{
    if (!AppContext::Get().hasProcess()) {
        Gui::log("未附加进程，无法设置断点");
        return;
    }

    auto existing = std::find_if(breakpoints.begin(), breakpoints.end(),
        [address](const BreakpointInfo& bp) { return bp.address == address; });
    if (existing != breakpoints.end()) {
        Gui::log("断点已存在: 0x%llX", address);
        return;
    }

    if (SetKernelBreakpoint(address, (uint32_t)type, (uint32_t)size)) {
        BreakpointInfo bp;
        bp.address = address;
        bp.type = type;
        bp.size = size;
        bp.description = description;
        bp.enabled = true;
        bp.suspended = false;
        bp.hitCount = 0;

        breakpoints.push_back(bp);
        Gui::log("断点已设置: 0x%llX", address);
    } else {
        Gui::log("设置断点失败: 0x%llX", address);
    }
}

void BreakpointWindow::removeBreakpoint(int index)
{
    if (index < 0 || index >= (int)breakpoints.size()) return;

    auto& bp = breakpoints[index];
    if (bp.enabled) {
        if (RemoveKernelBreakpoint(bp.address)) {
            Gui::log("断点已移除: 0x%llX", bp.address);
        } else {
            Gui::log("移除断点失败: 0x%llX", bp.address);
            return;
        }
    }

    breakpoints.erase(breakpoints.begin() + index);

    // 更新所有打开的详情窗口的断点索引
    for (auto& window : detailWindows) {
        if (window.breakpointIndex > index) {
            window.breakpointIndex--;
        } else if (window.breakpointIndex == index) {
            window.isOpen = false; // 关闭被删除断点的详情窗口
        }
    }
}

void BreakpointWindow::toggleBreakpoint(int index)
{
    if (index < 0 || index >= (int)breakpoints.size()) return;

    auto& bp = breakpoints[index];
    if (bp.enabled) {
        // 禁用断点
        if (RemoveKernelBreakpoint(bp.address)) {
            bp.enabled = false;
            bp.suspended = false;
            Gui::log("断点已禁用: 0x%llX", bp.address);
        } else {
            Gui::log("禁用断点失败: 0x%llX", bp.address);
        }
    } else {
        // 启用断点
        if (SetKernelBreakpoint(bp.address, (uint32_t)bp.type, (uint32_t)bp.size)) {
            bp.enabled = true;
            bp.suspended = false;
            Gui::log("断点已启用: 0x%llX", bp.address);
        } else {
            Gui::log("启用断点失败: 0x%llX", bp.address);
        }
    }
}

void BreakpointWindow::suspendBreakpoint(int index)
{
    if (index < 0 || index >= (int)breakpoints.size()) return;

    auto& bp = breakpoints[index];
    if (!bp.enabled || bp.suspended) return;

    if (SuspendKernelBreakpoint(bp.address)) {
        bp.suspended = true;
        Gui::log("断点已暂停: 0x%llX", bp.address);
    } else {
        Gui::log("暂停断点失败: 0x%llX", bp.address);
    }
}

void BreakpointWindow::resumeBreakpoint(int index)
{
    if (index < 0 || index >= (int)breakpoints.size()) return;

    auto& bp = breakpoints[index];
    if (!bp.enabled || !bp.suspended) return;

    if (ResumeKernelBreakpoint(bp.address)) {
        bp.suspended = false;
        Gui::log("断点已恢复: 0x%llX", bp.address);
    } else {
        Gui::log("恢复断点失败: 0x%llX", bp.address);
    }
}
