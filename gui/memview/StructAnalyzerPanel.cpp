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

// ============================================================
// resolveNodeByPath — 通过路径定位树中节点
// ============================================================
DissectNode* MemoryViewerWindow::resolveNodeByPath(const std::vector<int>& path)
{
    if (path.empty()) return nullptr;
    std::vector<DissectNode>* vec = &dissectNodes;
    DissectNode* node = nullptr;
    for (size_t i = 0; i < path.size(); i++) {
        int idx = path[i];
        if (idx < 0 || idx >= (int)vec->size()) return nullptr;
        node = &(*vec)[idx];
        if (i + 1 < path.size())
            vec = &node->children;
    }
    return node;
}

// ============================================================
// drawStructAnalyzerPanel — 主入口
// ============================================================
void MemoryViewerWindow::drawStructAnalyzerPanel()
{
    drawDissectorToolbar();
    ImGui::Separator();

    // --- 分割布局：左侧模板列表，右侧 Dissector 表格 ---
    ImGui::BeginChild("TemplateList", ImVec2(180, 0), true);
    {
        ImGui::Text("模板列表:");
        ImGui::Separator();

        if (ImGui::Button("保存当前..")) {
            showSaveTemplateDialog = true;
            memset(saveTemplateName, 0, sizeof(saveTemplateName));
        }
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

        int pendingDeleteIndex = -1;
        for (size_t i = 0; i < structDefinitions.size(); i++) {
            bool isSelected = (selectedStructIndex == (int)i);
            if (ImGui::Selectable(structDefinitions[i].name.c_str(), isSelected)) {
                selectedStructIndex = (int)i;
                loadTemplateIntoDissector((int)i);
            }
            char popup_id[64];
            snprintf(popup_id, sizeof(popup_id), "TplPopup_%zu", i);
            if (ImGui::BeginPopupContextItem(popup_id)) {
                if (ImGui::MenuItem("加载到 Dissector")) {
                    loadTemplateIntoDissector((int)i);
                }
                if (ImGui::MenuItem("删除")) {
                    pendingDeleteIndex = (int)i;
                }
                ImGui::EndPopup();
            }
        }
        if (pendingDeleteIndex >= 0) {
            structDefinitions.erase(structDefinitions.begin() + pendingDeleteIndex);
            if (selectedStructIndex >= (int)structDefinitions.size())
                selectedStructIndex = -1;
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // 右侧：Dissector 表格
    ImGui::BeginChild("DissectorView", ImVec2(0, 0), true);
    {
        if (!dissectNodes.empty() && structBaseAddress != 0) {
            drawDissectorTable();
        } else {
            ImGui::TextDisabled("输入地址并点击 [解析] 开始分析内存");
        }
    }
    ImGui::EndChild();

    // 保存模板对话框
    if (showSaveTemplateDialog) {
        ImGui::OpenPopup("保存为模板");
        showSaveTemplateDialog = false;
    }
    if (ImGui::BeginPopupModal("保存为模板", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText("模板名称", saveTemplateName, sizeof(saveTemplateName));
        if (ImGui::Button("确定") && strlen(saveTemplateName) > 0) {
            saveDissectAsTemplate(saveTemplateName);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("取消")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // 节点编辑弹窗
    drawNodeEditPopup();
}

// ============================================================
// drawDissectorToolbar — 工具栏
// ============================================================
void MemoryViewerWindow::drawDissectorToolbar()
{
    // --- 第一行：地址 + 导航 ---
    ImGui::Text("地址:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160);
    ImGui::InputScalar("##struct_addr", ImGuiDataType_U64, &structBaseAddress,
                        nullptr, nullptr, "%llX", ImGuiInputTextFlags_CharsHexadecimal);

    ImGui::SameLine();
    if (ImGui::Button("解析")) {
        if (structBaseAddress != 0) {
            ReadProcessMemoryBytes(structBaseAddress, dissectTotalSize, structBuffer);
            autoAnalyzeNodes(structBuffer);
            dissectAddrHistory.clear();
            dissectAddrHistory.push_back(structBaseAddress);
            dissectHistoryIdx = 0;
        }
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(dissectHistoryIdx <= 0);
    if (ImGui::Button("<< 后退")) {
        dissectHistoryIdx--;
        structBaseAddress = dissectAddrHistory[dissectHistoryIdx];
        ReadProcessMemoryBytes(structBaseAddress, dissectTotalSize, structBuffer);
        autoAnalyzeNodes(structBuffer);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(dissectHistoryIdx < 0 || dissectHistoryIdx >= (int)dissectAddrHistory.size() - 1);
    if (ImGui::Button("前进 >>")) {
        dissectHistoryIdx++;
        structBaseAddress = dissectAddrHistory[dissectHistoryIdx];
        ReadProcessMemoryBytes(structBaseAddress, dissectTotalSize, structBuffer);
        autoAnalyzeNodes(structBuffer);
    }
    ImGui::EndDisabled();

    // --- 第二行：显示大小 ---
    ImGui::Text("显示大小:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    ImGui::InputInt("##dissect_total", &dissectTotalSize, 0, 0);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        if (dissectTotalSize < 16) dissectTotalSize = 16;
        if (dissectTotalSize > 65536) dissectTotalSize = 65536;
        if (structBaseAddress != 0) {
            ReadProcessMemoryBytes(structBaseAddress, dissectTotalSize, structBuffer);
            autoAnalyzeNodes(structBuffer);
        }
    }

    ImGui::SameLine();
    if (ImGui::Button("自动分析")) {
        if (structBaseAddress != 0 && !structBuffer.empty()) {
            autoAnalyzeNodes(structBuffer);
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("自动检测指针/浮点/字符串/整数类型");

    // --- 第三行：自动刷新 ---
    ImGui::Checkbox("自动刷新", &dissectAutoRefresh);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60);
    ImGui::InputFloat("##dissect_interval", &dissectRefreshInterval, 0, 0, "%.1f");
    ImGui::SameLine();
    ImGui::Text("秒");
    if (dissectAutoRefresh && structBaseAddress != 0 && !dissectNodes.empty()) {
        timeSinceDissectRefresh += ImGui::GetIO().DeltaTime;
        if (timeSinceDissectRefresh >= dissectRefreshInterval) {
            timeSinceDissectRefresh = 0.0f;
            ReadProcessMemoryBytes(structBaseAddress, dissectTotalSize, structBuffer);
            refreshDissectValues();
        }
    }
}

// PLACEHOLDER_REMAINING_FUNCTIONS

// ============================================================
// drawDissectorTable — 树形表格
// ============================================================
void MemoryViewerWindow::drawDissectorTable()
{
    DissectDeferredOps ops;

    const int colCount = 6;
    ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                            ImGuiTableFlags_SizingFixedFit;
    if (ImGui::BeginTable("DissectorTable", colCount, flags)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("偏移",  ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableSetupColumn("地址",  ImGuiTableColumnFlags_WidthFixed, 130);
        ImGui::TableSetupColumn("名称",  ImGuiTableColumnFlags_WidthFixed, 160);
        ImGui::TableSetupColumn("类型",  ImGuiTableColumnFlags_WidthFixed, 100);
        ImGui::TableSetupColumn("值",    ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("操作",  ImGuiTableColumnFlags_WidthFixed, 50);
        ImGui::TableHeadersRow();

        int flatIndex = 0;
        std::vector<int> path;
        for (int i = 0; i < (int)dissectNodes.size(); i++) {
            path.clear();
            path.push_back(i);
            drawNodeRow(dissectNodes[i], structBaseAddress, flatIndex, path, ops);
        }
        ImGui::EndTable();
    }

    // 延迟执行类型变更
    if (ops.pendingTypeChangeFlat >= 0) {
        auto* parentVec = &dissectNodes;
        for (int p = 0; p < (int)ops.pendingTypeChangePath.size() - 1; p++)
            parentVec = &(*parentVec)[ops.pendingTypeChangePath[p]].children;
        int idx = ops.pendingTypeChangePath.back();
        onDissectNodeTypeChanged(*parentVec, idx, ops.pendingNewType);
    }
    // 延迟执行写入
    if (ops.pendingWriteFlat >= 0) {
        DissectNode* node = resolveNodeByPath(ops.pendingWritePath);
        if (node) {
            uint64_t baseAddr = structBaseAddress;
            auto* parentVec = &dissectNodes;
            for (int p = 0; p < (int)ops.pendingWritePath.size() - 1; p++) {
                baseAddr = (*parentVec)[ops.pendingWritePath[p]].pointerTarget;
                parentVec = &(*parentVec)[ops.pendingWritePath[p]].children;
            }
            writeDissectNodeValue(*node, baseAddr, ops.pendingWriteValue);
        }
    }
    // 延迟执行展开/折叠
    if (ops.pendingExpandFlat >= 0) {
        DissectNode* node = resolveNodeByPath(ops.pendingExpandPath);
        if (node) {
            uint64_t baseAddr = structBaseAddress;
            auto* parentVec = &dissectNodes;
            for (int p = 0; p < (int)ops.pendingExpandPath.size() - 1; p++) {
                baseAddr = (*parentVec)[ops.pendingExpandPath[p]].pointerTarget;
                parentVec = &(*parentVec)[ops.pendingExpandPath[p]].children;
            }
            if (node->expanded)
                collapsePointerNode(*node);
            else
                expandPointerNode(*node, baseAddr);
        }
    }
}

// PLACEHOLDER_DRAW_NODE_ROW

// ============================================================
// drawNodeRow — 递归渲染单个节点行
// ============================================================
void MemoryViewerWindow::drawNodeRow(DissectNode& node, uint64_t baseAddr,
                                     int& flatIndex, std::vector<int>& path,
                                     DissectDeferredOps& ops)
{
    int myFlat = flatIndex++;
    ImGui::TableNextRow();
    ImGui::PushID(myFlat);

    // --- 偏移列：TreeNodeEx 实现缩进 ---
    ImGui::TableSetColumnIndex(0);
    bool isPointer = (node.type == FieldType::POINTER);
    ImGuiTreeNodeFlags treeFlags = ImGuiTreeNodeFlags_SpanAvailWidth
                                 | ImGuiTreeNodeFlags_OpenOnArrow;
    if (!isPointer)
        treeFlags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (node.expanded)
        ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    else if (isPointer)
        ImGui::SetNextItemOpen(false, ImGuiCond_Always);

    char offsetLabel[64];
    snprintf(offsetLabel, sizeof(offsetLabel), "+0x%03X", node.offset);
    bool treeOpen = ImGui::TreeNodeEx(offsetLabel, treeFlags);

    // 右键菜单
    char ctxId[64];
    snprintf(ctxId, sizeof(ctxId), "NodeCtx_%d", myFlat);
    if (ImGui::BeginPopupContextItem(ctxId)) {
        if (ImGui::MenuItem("编辑属性...")) {
            dissectEditPath = path;
            showNodeEditPopup = true;
            strncpy(editNodeName, node.name.c_str(), sizeof(editNodeName) - 1);
            editNodeName[sizeof(editNodeName) - 1] = '\0';
            strncpy(editNodeDesc, node.description.c_str(), sizeof(editNodeDesc) - 1);
            editNodeDesc[sizeof(editNodeDesc) - 1] = '\0';
            editNodeTypeIdx = (int)node.type;
            editNodeStringSize = node.storedSize > 0 ? node.storedSize : 32;
        }
        if (ImGui::MenuItem("浏览内存")) {
            jumpToAddress(baseAddr + node.offset);
        }
        if (ImGui::MenuItem("添加到监控")) {
            MemoryWatchItem item;
            item.description = node.name;
            item.address = baseAddr + node.offset;
            item.type = node.type;
            item.enabled = true;
            watchItems.push_back(item);
            if (AppContext::Get().hasProcess())
                watchItems.back().cachedValue = readWatchItemValue(watchItems.back());
        }
        if (isPointer && node.pointerTarget != 0) {
            if (ImGui::MenuItem("在新地址解析指针目标")) {
                uint64_t target = node.pointerTarget;
                if (dissectHistoryIdx >= 0 &&
                    dissectHistoryIdx < (int)dissectAddrHistory.size() - 1)
                    dissectAddrHistory.resize(dissectHistoryIdx + 1);
                dissectAddrHistory.push_back(target);
                dissectHistoryIdx = (int)dissectAddrHistory.size() - 1;
                structBaseAddress = target;
                ReadProcessMemoryBytes(structBaseAddress, dissectTotalSize, structBuffer);
                regenerateDissectNodes();
                refreshDissectValues();
            }
        }
        ImGui::EndPopup();
    }

    // 指针展开/折叠状态同步
    if (isPointer && treeOpen && !node.expanded) {
        ops.pendingExpandFlat = myFlat;
        ops.pendingExpandPath = path;
    } else if (isPointer && !treeOpen && node.expanded) {
        ops.pendingExpandFlat = myFlat;
        ops.pendingExpandPath = path;
    }

    // --- 地址列 ---
    ImGui::TableSetColumnIndex(1);
    ImGui::TextColored(ColorScheme::Address, "%llX", (unsigned long long)(baseAddr + node.offset));

    // --- 名称列（可编辑）---
    ImGui::TableSetColumnIndex(2);
    char nameBuf[128];
    strncpy(nameBuf, node.name.c_str(), sizeof(nameBuf) - 1);
    nameBuf[sizeof(nameBuf) - 1] = '\0';
    ImGui::PushItemWidth(-1);
    if (ImGui::InputText("##name", nameBuf, sizeof(nameBuf)))
        node.name = nameBuf;
    ImGui::PopItemWidth();

    // --- 类型列（下拉）---
    ImGui::TableSetColumnIndex(3);
    const char* typeNames[] = {
        "BYTE", "WORD", "DWORD", "QWORD",
        "FLOAT", "DOUBLE", "POINTER",
        "STRING", "UTF-8", "UTF-16"
    };
    int currentType = (int)node.type;
    ImGui::PushItemWidth(-1);
    if (ImGui::Combo("##type", &currentType, typeNames, IM_ARRAYSIZE(typeNames))) {
        ops.pendingTypeChangeFlat = myFlat;
        ops.pendingNewType = (FieldType)currentType;
        ops.pendingTypeChangePath = path;
    }
    ImGui::PopItemWidth();

    // --- 值列（可编辑，回车写入）---
    ImGui::TableSetColumnIndex(4);
    char valBuf[256];
    strncpy(valBuf, node.cachedValue.c_str(), sizeof(valBuf) - 1);
    valBuf[sizeof(valBuf) - 1] = '\0';
    ImGui::PushItemWidth(-1);
    if (ImGui::InputText("##val", valBuf, sizeof(valBuf), ImGuiInputTextFlags_EnterReturnsTrue)) {
        ops.pendingWriteFlat = myFlat;
        ops.pendingWriteValue = valBuf;
        ops.pendingWritePath = path;
    }
    ImGui::PopItemWidth();

    // --- 操作列 ---
    ImGui::TableSetColumnIndex(5);
    if (isPointer && node.pointerTarget != 0) {
        if (ImGui::SmallButton("->")) {
            ops.pendingExpandFlat = myFlat;
            ops.pendingExpandPath = path;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("展开/折叠指针 (0x%llX)", (unsigned long long)node.pointerTarget);
    }

    ImGui::PopID();

    // 递归渲染子节点
    // 注意：只要 treeOpen 为 true 且是指针节点，就必须调用 TreePop()
    if (isPointer && treeOpen) {
        if (node.expanded) {
            for (int c = 0; c < (int)node.children.size(); c++) {
                path.push_back(c);
                drawNodeRow(node.children[c], node.pointerTarget, flatIndex, path, ops);
                path.pop_back();
            }
        }
        ImGui::TreePop();
    }
}

// PLACEHOLDER_NODE_EDIT_POPUP

// ============================================================
// drawNodeEditPopup — 节点属性编辑弹窗
// ============================================================
void MemoryViewerWindow::drawNodeEditPopup()
{
    if (showNodeEditPopup) {
        ImGui::OpenPopup("编辑字段属性");
        showNodeEditPopup = false;
    }

    if (ImGui::BeginPopupModal("编辑字段属性", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        DissectNode* node = resolveNodeByPath(dissectEditPath);
        if (!node) {
            ImGui::Text("节点不存在");
            if (ImGui::Button("关闭")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }

        ImGui::Text("偏移: +0x%03X", node->offset);
        ImGui::Separator();

        ImGui::InputText("名称", editNodeName, sizeof(editNodeName));

        const char* typeNames[] = {
            "BYTE", "WORD", "DWORD", "QWORD",
            "FLOAT", "DOUBLE", "POINTER",
            "STRING", "UTF-8", "UTF-16"
        };
        ImGui::Combo("类型", &editNodeTypeIdx, typeNames, IM_ARRAYSIZE(typeNames));

        FieldType editType = (FieldType)editNodeTypeIdx;
        if (editType == FieldType::STRING || editType == FieldType::STRING_UTF8 ||
            editType == FieldType::STRING_UTF16) {
            ImGui::InputInt("字符串长度", &editNodeStringSize);
            if (editNodeStringSize < 1) editNodeStringSize = 1;
            if (editNodeStringSize > 4096) editNodeStringSize = 4096;
        }

        ImGui::InputTextMultiline("备注", editNodeDesc, sizeof(editNodeDesc),
                                  ImVec2(300, 60));

        ImGui::Separator();
        if (ImGui::Button("确定", ImVec2(120, 0))) {
            node->name = editNodeName;
            node->description = editNodeDesc;
            FieldType newType = (FieldType)editNodeTypeIdx;
            if (newType != node->type) {
                // 延迟处理类型变更（需要 merge/split）
                auto* parentVec = &dissectNodes;
                for (int p = 0; p < (int)dissectEditPath.size() - 1; p++)
                    parentVec = &(*parentVec)[dissectEditPath[p]].children;
                int idx = dissectEditPath.back();
                onDissectNodeTypeChanged(*parentVec, idx, newType);
            }
            if (newType == FieldType::STRING || newType == FieldType::STRING_UTF8 ||
                newType == FieldType::STRING_UTF16) {
                node->storedSize = editNodeStringSize;
            }
            refreshDissectValues();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("取消", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// PLACEHOLDER_CORE_LOGIC

// ============================================================
// regenerateDissectNodes — 按默认大小切分
// ============================================================
void MemoryViewerWindow::regenerateDissectNodes()
{
    dissectNodes.clear();
    int totalBytes = (int)structBuffer.size();
    if (totalBytes <= 0) return;

    FieldType defaultType;
    switch (dissectDefaultSize) {
        case 1: defaultType = FieldType::BYTE; break;
        case 2: defaultType = FieldType::WORD; break;
        case 8: defaultType = FieldType::QWORD; break;
        default: defaultType = FieldType::DWORD; break;
    }
    int offset = 0;
    while (offset + dissectDefaultSize <= totalBytes) {
        DissectNode node;
        node.offset = offset;
        node.type = defaultType;
        node.depth = 0;
        char nb[32];
        snprintf(nb, sizeof(nb), "field_%04X", offset);
        node.name = nb;
        node.storedSize = dissectDefaultSize;
        dissectNodes.push_back(std::move(node));
        offset += dissectDefaultSize;
    }
}

// ============================================================
// refreshDissectValues / refreshNodeValues — 递归刷新值
// ============================================================
void MemoryViewerWindow::refreshDissectValues()
{
    refreshNodeValues(dissectNodes, structBuffer, structBaseAddress);
}

void MemoryViewerWindow::refreshNodeValues(std::vector<DissectNode>& nodes,
                                           const std::vector<unsigned char>& buffer,
                                           uint64_t baseAddr)
{
    for (auto& node : nodes) {
        int sz = node.getSize();
        if (node.offset + sz > (int)buffer.size()) {
            node.cachedValue = "??";
            continue;
        }
        node.cachedValue = readSingleFieldValue(buffer, node.type, node.offset, sz);

        // POINTER 类型提取目标地址
        if (node.type == FieldType::POINTER && node.offset + 8 <= (int)buffer.size()) {
            node.pointerTarget = *(uint64_t*)&buffer[node.offset];
        }

        // 递归刷新已展开的子节点
        if (node.expanded && !node.children.empty() && node.pointerTarget != 0) {
            ReadProcessMemoryBytes(node.pointerTarget, dissectTotalSize, node.childBuffer);
            refreshNodeValues(node.children, node.childBuffer, node.pointerTarget);
        }
    }
}

// PLACEHOLDER_EXPAND_AND_TYPE_CHANGE

// ============================================================
// expandPointerNode / collapsePointerNode — 指针展开/折叠
// ============================================================
void MemoryViewerWindow::expandPointerNode(DissectNode& node, uint64_t baseAddr)
{
    if (node.type != FieldType::POINTER) return;
    if (node.pointerTarget == 0) {
        Gui::log("指针值为 NULL，无法展开");
        return;
    }

    node.childBuffer.clear();
    if (!ReadProcessMemoryBytes(node.pointerTarget, dissectTotalSize, node.childBuffer)) {
        Gui::log("读取指针目标内存失败: 0x%llX", (unsigned long long)node.pointerTarget);
        return;
    }

    // 对子节点内存执行自动分析
    node.children.clear();
    int offset = 0;
    int fieldIndex = 1;
    int childDepth = node.depth + 1;
    const auto& data = node.childBuffer;

    while (offset < (int)data.size() - 8) {
        DissectNode child;
        child.offset = offset;
        child.depth = childDepth;

        // 检查指针
        if (offset + 7 < (int)data.size()) {
            uint64_t val64 = *(uint64_t*)&data[offset];
            uint64_t tmp = val64;
            if ((val64 & 0xffff00000000) == 0xb40000000000)
                tmp = val64 & 0xffffffffffff;
            if (tmp > 0x4FFFFFFFFF && tmp < 0x7FFFFFFFFFFF) {
                child.type = FieldType::POINTER;
                child.storedSize = 8;
                child.pointerTarget = val64;
                char nb[32]; snprintf(nb, sizeof(nb), "ptr_%d", fieldIndex++);
                child.name = nb;
                node.children.push_back(std::move(child));
                offset += 8;
                continue;
            }
        }

        // 检查浮点
        if (offset + 3 < (int)data.size()) {
            float fval = *(float*)&data[offset];
            if (!std::isnan(fval) && !std::isinf(fval) && fval > -1000000 && fval < 1000000) {
                uint32_t ival = *(uint32_t*)&data[offset];
                if (ival > 0x1000 && (ival & 0xFF) != 0) {
                    child.type = FieldType::FLOAT;
                    child.storedSize = 4;
                    char nb[32]; snprintf(nb, sizeof(nb), "float_%d", fieldIndex++);
                    child.name = nb;
                    node.children.push_back(std::move(child));
                    offset += 4;
                    continue;
                }
            }
        }

        // 检查字符串
        bool isString = true;
        int strLen = 0;
        for (int i = offset; i < (int)data.size() && i < offset + 32; i++) {
            unsigned char c = data[i];
            if (c == 0) { strLen = i - offset; break; }
            if (c < 32 || c > 126) { isString = false; break; }
            strLen++;
        }
        if (isString && strLen >= 4) {
            child.type = FieldType::STRING;
            child.storedSize = strLen + 1;
            char nb[32]; snprintf(nb, sizeof(nb), "str_%d", fieldIndex++);
            child.name = nb;
            node.children.push_back(std::move(child));
            offset += strLen + 1;
            continue;
        }

        // 默认 DWORD
        if (offset + 3 < (int)data.size()) {
            child.type = FieldType::DWORD;
            child.storedSize = 4;
            char nb[32]; snprintf(nb, sizeof(nb), "dword_%d", fieldIndex++);
            child.name = nb;
            node.children.push_back(std::move(child));
            offset += 4;
        } else {
            break;
        }
    }

    refreshNodeValues(node.children, node.childBuffer, node.pointerTarget);
    node.expanded = true;
    Gui::log("展开指针 -> 0x%llX (%d 子字段)", (unsigned long long)node.pointerTarget, (int)node.children.size());
}

void MemoryViewerWindow::collapsePointerNode(DissectNode& node)
{
    node.expanded = false;
    node.children.clear();
    node.childBuffer.clear();
}

// ============================================================
// onDissectNodeTypeChanged — 类型变更，处理合并/拆分
// ============================================================
void MemoryViewerWindow::onDissectNodeTypeChanged(std::vector<DissectNode>& nodes,
                                                  int nodeIndex, FieldType newType)
{
    if (nodeIndex < 0 || nodeIndex >= (int)nodes.size()) return;
    auto& node = nodes[nodeIndex];

    // 从 POINTER 切换走时折叠子节点
    if (node.type == FieldType::POINTER && newType != FieldType::POINTER)
        collapsePointerNode(node);

    int oldSize = node.getSize();
    node.type = newType;

    if (newType == FieldType::STRING || newType == FieldType::STRING_UTF8 ||
        newType == FieldType::STRING_UTF16) {
        node.storedSize = 32;
    } else {
        node.storedSize = 0;
    }

    int newSize = node.getSize();

    if (newSize > oldSize) {
        // 吸收后续行
        int need = newSize - oldSize;
        while (need > 0 && nodeIndex + 1 < (int)nodes.size()) {
            int nextSize = nodes[nodeIndex + 1].getSize();
            nodes.erase(nodes.begin() + nodeIndex + 1);
            need -= nextSize;
        }
    } else if (newSize < oldSize) {
        // 释放空间，用默认大小填充
        int freed = oldSize - newSize;
        int fillOffset = node.offset + newSize;
        FieldType fillType;
        switch (dissectDefaultSize) {
            case 1: fillType = FieldType::BYTE; break;
            case 2: fillType = FieldType::WORD; break;
            case 8: fillType = FieldType::QWORD; break;
            default: fillType = FieldType::DWORD; break;
        }
        int insertPos = nodeIndex + 1;
        while (freed >= dissectDefaultSize) {
            DissectNode nr;
            nr.offset = fillOffset;
            nr.type = fillType;
            nr.depth = node.depth;
            char nb[32];
            snprintf(nb, sizeof(nb), "field_%04X", fillOffset);
            nr.name = nb;
            nr.storedSize = dissectDefaultSize;
            nodes.insert(nodes.begin() + insertPos, std::move(nr));
            fillOffset += dissectDefaultSize;
            freed -= dissectDefaultSize;
            insertPos++;
        }
    }

    refreshDissectValues();
}

// PLACEHOLDER_WRITE_AND_TEMPLATE

// ============================================================
// writeDissectNodeValue — 写入值到内存
// ============================================================
bool MemoryViewerWindow::writeDissectNodeValue(DissectNode& node, uint64_t baseAddr,
                                               const std::string& valueStr)
{
    uint64_t addr = baseAddr + node.offset;
    std::vector<unsigned char> data;
    try {
        switch (node.type) {
            case FieldType::BYTE: {
                int val = std::stoi(valueStr);
                data.push_back((unsigned char)val);
                break;
            }
            case FieldType::WORD: {
                uint16_t val = (uint16_t)std::stoul(valueStr);
                data.resize(2);
                *(uint16_t*)&data[0] = val;
                break;
            }
            case FieldType::DWORD: {
                uint32_t val = (uint32_t)std::stoul(valueStr);
                data.resize(4);
                *(uint32_t*)&data[0] = val;
                break;
            }
            case FieldType::QWORD:
            case FieldType::POINTER: {
                uint64_t val = std::stoull(valueStr, nullptr, 16);
                data.resize(8);
                *(uint64_t*)&data[0] = val;
                break;
            }
            case FieldType::FLOAT: {
                float val = std::stof(valueStr);
                data.resize(4);
                *(float*)&data[0] = val;
                break;
            }
            case FieldType::DOUBLE: {
                double val = std::stod(valueStr);
                data.resize(8);
                *(double*)&data[0] = val;
                break;
            }
            default:
                return false;
        }

        if (WriteProcessMemoryBytes(addr, (uint32_t)data.size(), data)) {
            Gui::log("写入 %s @ 0x%llX: %s", node.name.c_str(), (unsigned long long)addr, valueStr.c_str());
            // 刷新整棵树的值
            ReadProcessMemoryBytes(structBaseAddress, dissectTotalSize, structBuffer);
            refreshDissectValues();
            return true;
        }
    } catch (const std::exception& e) {
        Gui::log("写入失败: %s", e.what());
    }
    return false;
}

// ============================================================
// saveDissectAsTemplate / loadTemplateIntoDissector
// ============================================================
void MemoryViewerWindow::saveDissectAsTemplate(const std::string& name)
{
    StructDefinition def;
    def.name = name;
    for (auto& node : dissectNodes) {
        StructField f;
        f.name = node.name;
        f.type = node.type;
        f.offset = node.offset;
        f.size = node.getSize();
        f.description = node.description;
        def.fields.push_back(f);
    }
    def.calculateSize();
    structDefinitions.push_back(def);
    selectedStructIndex = (int)structDefinitions.size() - 1;
    Gui::log("已保存模板: %s (%d 字段)", name.c_str(), (int)def.fields.size());
}

void MemoryViewerWindow::loadTemplateIntoDissector(int index)
{
    if (index < 0 || index >= (int)structDefinitions.size()) return;
    auto& def = structDefinitions[index];

    if (structBaseAddress != 0) {
        int needed = def.totalSize > dissectTotalSize ? def.totalSize : dissectTotalSize;
        ReadProcessMemoryBytes(structBaseAddress, needed, structBuffer);
    }

    dissectNodes.clear();
    for (auto& f : def.fields) {
        DissectNode node;
        node.offset = f.offset;
        node.type = f.type;
        node.name = f.name;
        node.storedSize = f.size;
        node.description = f.description;
        node.depth = 0;
        dissectNodes.push_back(std::move(node));
    }
    refreshDissectValues();
    Gui::log("已加载模板: %s", def.name.c_str());
}

// PLACEHOLDER_AUTO_ANALYZE

// ============================================================
// autoAnalyzeNodes — 自动推导数据类型
// ============================================================
void MemoryViewerWindow::autoAnalyzeNodes(const std::vector<unsigned char>& data)
{
    if (data.empty()) return;

    dissectNodes.clear();
    int offset = 0;
    int fieldIndex = 1;

    while (offset < (int)data.size() - 8) {
        DissectNode node;
        node.offset = offset;
        node.depth = 0;

        // 检查指针（ARM64 用户空间地址范围）
        if (offset + 7 < (int)data.size()) {
            uint64_t val64 = *(uint64_t*)&data[offset];
            uint64_t tmp = val64;
            if ((val64 & 0xffff00000000) == 0xb40000000000)
                tmp = val64 & 0xffffffffffff;
            if (tmp > 0x4FFFFFFFFF && tmp < 0x7FFFFFFFFFFF) {
                node.type = FieldType::POINTER;
                node.storedSize = 8;
                node.pointerTarget = val64;
                char nb[32]; snprintf(nb, sizeof(nb), "ptr_%d", fieldIndex++);
                node.name = nb;
                dissectNodes.push_back(std::move(node));
                offset += 8;
                continue;
            }
        }

        // 检查浮点
        if (offset + 3 < (int)data.size()) {
            float fval = *(float*)&data[offset];
            if (!std::isnan(fval) && !std::isinf(fval) && fval > -1000000 && fval < 1000000) {
                uint32_t ival = *(uint32_t*)&data[offset];
                if (ival > 0x1000 && (ival & 0xFF) != 0) {
                    node.type = FieldType::FLOAT;
                    node.storedSize = 4;
                    char nb[32]; snprintf(nb, sizeof(nb), "float_%d", fieldIndex++);
                    node.name = nb;
                    dissectNodes.push_back(std::move(node));
                    offset += 4;
                    continue;
                }
            }
        }

        // 检查字符串
        bool isString = true;
        int strLen = 0;
        for (int i = offset; i < (int)data.size() && i < offset + 32; i++) {
            unsigned char c = data[i];
            if (c == 0) { strLen = i - offset; break; }
            if (c < 32 || c > 126) { isString = false; break; }
            strLen++;
        }
        if (isString && strLen >= 4) {
            node.type = FieldType::STRING;
            node.storedSize = strLen + 1;
            char nb[32]; snprintf(nb, sizeof(nb), "str_%d", fieldIndex++);
            node.name = nb;
            dissectNodes.push_back(std::move(node));
            offset += strLen + 1;
            continue;
        }

        // 默认 DWORD
        if (offset + 3 < (int)data.size()) {
            node.type = FieldType::DWORD;
            node.storedSize = 4;
            char nb[32]; snprintf(nb, sizeof(nb), "dword_%d", fieldIndex++);
            node.name = nb;
            dissectNodes.push_back(std::move(node));
            offset += 4;
        } else {
            break;
        }
    }

    refreshDissectValues();
    Gui::log("自动分析发现 %d 个字段", (int)dissectNodes.size());
}

// PLACEHOLDER_UTILITY_FUNCTIONS

// ============================================================
// 保留的辅助函数
// ============================================================

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
        case FieldType::POINTER: return 8;
        case FieldType::STRING: return 1;
        case FieldType::ARRAY: return 1;
        case FieldType::STRUCT: return 0;
        default: return 1;
    }
}

std::string MemoryViewerWindow::readFieldValue(const std::vector<unsigned char>& data,
                                               const StructField& field, uint64_t baseAddr)
{
    if (field.offset + field.size * field.arrayCount > (int)data.size())
        return "超出范围";
    std::stringstream ss;
    if (field.arrayCount > 1) {
        ss << "{ ";
        for (int i = 0; i < field.arrayCount && i < 10; i++) {
            if (i > 0) ss << ", ";
            int elemOffset = field.offset + i * field.size;
            if (elemOffset + field.size <= (int)data.size())
                ss << readSingleFieldValue(data, field.type, elemOffset, field.size);
        }
        if (field.arrayCount > 10) ss << ", ...";
        ss << " }";
        return ss.str();
    } else {
        return readSingleFieldValue(data, field.type, field.offset, field.size);
    }
}

// PLACEHOLDER_READ_SINGLE_FIELD

std::string MemoryViewerWindow::readSingleFieldValue(const std::vector<unsigned char>& data,
                                                     FieldType type, int offset, int size)
{
    std::stringstream ss;
    switch (type) {
        case FieldType::BYTE: {
            if (offset < (int)data.size()) {
                ss << "0x" << std::hex << std::uppercase << (int)data[offset]
                   << " (" << std::dec << (int)data[offset] << ")";
            }
            break;
        }
        case FieldType::WORD: {
            if (offset + 1 < (int)data.size()) {
                uint16_t val = *(uint16_t*)&data[offset];
                ss << "0x" << std::hex << std::uppercase << val
                   << " (" << std::dec << val << ")";
            }
            break;
        }
        case FieldType::DWORD: {
            if (offset + 3 < (int)data.size()) {
                uint32_t val = *(uint32_t*)&data[offset];
                ss << "0x" << std::hex << std::uppercase << val
                   << " (" << std::dec << val << ")";
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
            std::string str;
            for (int i = offset; i < (int)data.size() && i < offset + 256; i++) {
                if (data[i] == 0) break;
                str += (data[i] >= 32 && data[i] < 127) ? (char)data[i] : '.';
            }
            ss << "\"" << str << "\"";
            break;
        }
        case FieldType::STRING_UTF8: {
            ss << "\"" << readUTF8String(data, offset, 256) << "\"";
            break;
        }
        case FieldType::STRING_UTF16: {
            ss << "\"" << readUTF16String(data, offset, 256) << "\"";
            break;
        }
        default:
            ss << "未知类型";
            break;
    }
    return ss.str();
}

// PLACEHOLDER_SAVE_LOAD_DEFS

void MemoryViewerWindow::saveStructDefinitions()
{
    std::ofstream file("struct_definitions.dat", std::ios::binary);
    if (!file.is_open()) return;
    size_t count = structDefinitions.size();
    file.write((char*)&count, sizeof(count));
    for (const auto& structDef : structDefinitions) {
        size_t nameLen = structDef.name.size();
        file.write((char*)&nameLen, sizeof(nameLen));
        file.write(structDef.name.c_str(), nameLen);
        size_t fieldCount = structDef.fields.size();
        file.write((char*)&fieldCount, sizeof(fieldCount));
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
        size_t nameLen;
        if (!file.read((char*)&nameLen, sizeof(nameLen))) break;
        structDef.name.resize(nameLen);
        if (!file.read(&structDef.name[0], nameLen)) break;
        size_t fieldCount;
        if (!file.read((char*)&fieldCount, sizeof(fieldCount))) break;
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

// PLACEHOLDER_STRING_UTILS

std::string MemoryViewerWindow::readUTF8String(const std::vector<unsigned char>& data,
                                               size_t offset, size_t maxLength)
{
    std::string result;
    size_t i = offset;
    size_t end = (offset + maxLength < data.size()) ? (offset + maxLength) : data.size();
    while (i < end) {
        unsigned char c = data[i];
        if (c == 0) break;
        if (c <= 0x7F) {
            result += (c >= 32 && c < 127) ? (char)c : '.';
            i++;
        } else if ((c & 0xE0) == 0xC0) {
            if (i + 1 < end && (data[i + 1] & 0xC0) == 0x80) {
                result += (char)c; result += (char)data[i + 1]; i += 2;
            } else { result += '.'; i++; }
        } else if ((c & 0xF0) == 0xE0) {
            if (i + 2 < end && (data[i + 1] & 0xC0) == 0x80 && (data[i + 2] & 0xC0) == 0x80) {
                result += (char)c; result += (char)data[i + 1]; result += (char)data[i + 2]; i += 3;
            } else { result += '.'; i++; }
        } else if ((c & 0xF8) == 0xF0) {
            if (i + 3 < end && (data[i + 1] & 0xC0) == 0x80 && (data[i + 2] & 0xC0) == 0x80 && (data[i + 3] & 0xC0) == 0x80) {
                result += (char)c; result += (char)data[i + 1]; result += (char)data[i + 2]; result += (char)data[i + 3]; i += 4;
            } else { result += '.'; i++; }
        } else { result += '.'; i++; }
    }
    return result;
}

std::string MemoryViewerWindow::utf16ToUtf8(const uint16_t* utf16Str, size_t length)
{
    std::string result;
    for (size_t i = 0; i < length; i++) {
        uint16_t c = utf16Str[i];
        if (c == 0) break;
        if (c < 0xD800 || c > 0xDFFF) {
            if (c <= 0x7F) result += (char)c;
            else if (c <= 0x7FF) { result += (char)(0xC0 | (c >> 6)); result += (char)(0x80 | (c & 0x3F)); }
            else { result += (char)(0xE0 | (c >> 12)); result += (char)(0x80 | ((c >> 6) & 0x3F)); result += (char)(0x80 | (c & 0x3F)); }
        } else if (c >= 0xD800 && c <= 0xDBFF && i + 1 < length) {
            uint16_t c2 = utf16Str[i + 1];
            if (c2 >= 0xDC00 && c2 <= 0xDFFF) {
                uint32_t cp = 0x10000 + ((c & 0x3FF) << 10) + (c2 & 0x3FF);
                result += (char)(0xF0 | (cp >> 18)); result += (char)(0x80 | ((cp >> 12) & 0x3F));
                result += (char)(0x80 | ((cp >> 6) & 0x3F)); result += (char)(0x80 | (cp & 0x3F));
                i++;
            } else result += '.';
        } else result += '.';
    }
    return result;
}

std::string MemoryViewerWindow::readUTF16String(const std::vector<unsigned char>& data,
                                                size_t offset, size_t maxLength)
{
    size_t maxChars = maxLength / 2;
    if (offset + 1 >= data.size()) return "";
    std::vector<uint16_t> utf16Chars;
    for (size_t i = 0; i < maxChars; i++) {
        size_t byteIdx = offset + i * 2;
        if (byteIdx + 1 >= data.size()) break;
        uint16_t c = data[byteIdx] | (data[byteIdx + 1] << 8);
        if (c == 0) break;
        utf16Chars.push_back(c);
    }
    return utf16Chars.empty() ? "" : utf16ToUtf8(utf16Chars.data(), utf16Chars.size());
}

// PLACEHOLDER_WATCH_STRUCT

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
        if (AppContext::Get().hasProcess())
            watchItems.back().cachedValue = readWatchItemValue(watchItems.back());
        addedCount++;
    }
    Gui::log("已添加 %d 个字段到监控列表", addedCount);
    if (AppContext::Get().hasProcess() && addedCount > 0)
        timeSinceWatchUpdate = 0.0f;
}

bool MemoryViewerWindow::writeStructFieldValue(int fieldIndex, const std::string& value)
{
    if (selectedStructIndex < 0 || selectedStructIndex >= (int)structDefinitions.size()) return false;
    const auto& structDef = structDefinitions[selectedStructIndex];
    if (fieldIndex < 0 || fieldIndex >= (int)structDef.fields.size()) return false;
    const auto& field = structDef.fields[fieldIndex];
    uint64_t addr = structBaseAddress + field.offset;
    std::vector<unsigned char> data;
    try {
        switch (field.type) {
            case FieldType::BYTE: { data.push_back((unsigned char)std::stoi(value)); break; }
            case FieldType::WORD: { data.resize(2); *(uint16_t*)&data[0] = (uint16_t)std::stoul(value); break; }
            case FieldType::DWORD: { data.resize(4); *(uint32_t*)&data[0] = (uint32_t)std::stoul(value); break; }
            case FieldType::QWORD: case FieldType::POINTER: { data.resize(8); *(uint64_t*)&data[0] = std::stoull(value, nullptr, 16); break; }
            case FieldType::FLOAT: { data.resize(4); *(float*)&data[0] = std::stof(value); break; }
            case FieldType::DOUBLE: { data.resize(8); *(double*)&data[0] = std::stod(value); break; }
            default: return false;
        }
        if (WriteProcessMemoryBytes(addr, (uint32_t)data.size(), data)) {
            if (field.offset + (int)data.size() <= (int)structBuffer.size())
                memcpy(&structBuffer[field.offset], data.data(), data.size());
            return true;
        }
    } catch (const std::exception& e) {
        Gui::log("写入失败: %s", e.what());
    }
    return false;
}
