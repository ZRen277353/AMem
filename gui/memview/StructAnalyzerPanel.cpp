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
#include <cstdio>
#include <fstream>
#include <cmath>
#include <cstdlib>
#include <cerrno>
#include <limits>

namespace {
constexpr uint64_t kMinLikelyPointer = 0x4FFFFFFFFFULL;
constexpr uint64_t kMaxLikelyPointer = 0x7FFFFFFFFFFFULL;
constexpr size_t kMaxStructDefinitionCount = 4096;
constexpr size_t kMaxStructFieldCount = 65536;
constexpr size_t kMaxStructStringLength = 4096;
constexpr int kMaxStructFieldByteSpan = 1024 * 1024;

bool isLikelyPointerAddress(uint64_t address)
{
    return address > kMinLikelyPointer && address < kMaxLikelyPointer;
}

uint64_t normalizePointerAddress(uint64_t rawAddress)
{
    if (isLikelyPointerAddress(rawAddress)) {
        return rawAddress;
    }

    const uint64_t topByteStripped = rawAddress & 0x00FFFFFFFFFFFFFFULL;
    if (isLikelyPointerAddress(topByteStripped)) {
        return topByteStripped;
    }

    const uint64_t low48 = rawAddress & 0x0000FFFFFFFFFFFFULL;
    if (isLikelyPointerAddress(low48)) {
        return low48;
    }

    return rawAddress;
}

bool readDissectMemory(uint64_t address, int size, std::vector<unsigned char>& out)
{
    out.clear();
    if (address == 0 || size <= 0) {
        return false;
    }
    return ReadProcessMemoryBytes(address, static_cast<uint32_t>(size), out);
}

bool getStructFieldSpan(const StructField& field, size_t bufferSize,
                        size_t& offset, size_t& bytes)
{
    offset = 0;
    bytes = 0;
    if (field.offset < 0 || field.size <= 0 || field.arrayCount <= 0) {
        return false;
    }

    const int64_t span =
        static_cast<int64_t>(field.size) * static_cast<int64_t>(field.arrayCount);
    if (span <= 0 || span > kMaxStructFieldByteSpan) {
        return false;
    }

    offset = static_cast<size_t>(field.offset);
    bytes = static_cast<size_t>(span);
    return offset <= bufferSize && bytes <= bufferSize - offset;
}

bool addStructOffsetToAddress(uint64_t baseAddress, int offset, uint64_t& address)
{
    address = 0;
    if (baseAddress == 0 || offset < 0) {
        return false;
    }

    const uint64_t unsignedOffset = static_cast<uint64_t>(offset);
    if (baseAddress > (std::numeric_limits<uint64_t>::max)() - unsignedOffset) {
        return false;
    }

    address = baseAddress + unsignedOffset;
    return true;
}

bool isValidStoredStructField(const StructField& field)
{
    size_t offset = 0;
    size_t bytes = 0;
    return static_cast<int>(field.type) >= static_cast<int>(FieldType::BYTE) &&
           static_cast<int>(field.type) <= static_cast<int>(FieldType::STRUCT) &&
           getStructFieldSpan(field, static_cast<size_t>(kMaxStructFieldByteSpan), offset, bytes);
}

template <typename T>
bool readDissectScalar(const std::vector<unsigned char>& data, int offset, T& value)
{
    if (offset < 0 || static_cast<size_t>(offset) + sizeof(T) > data.size()) {
        return false;
    }

    std::memcpy(&value, data.data() + offset, sizeof(T));
    return true;
}

template <typename T>
void appendDissectScalar(std::vector<unsigned char>& data, T value)
{
    const size_t oldSize = data.size();
    data.resize(oldSize + sizeof(T));
    std::memcpy(data.data() + oldSize, &value, sizeof(T));
}

std::string stripDisplayQuotes(const std::string& value)
{
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        return value.substr(1, value.size() - 2);
    }

    return value;
}

void appendDissectString(std::vector<unsigned char>& data, const std::string& value, size_t fixedSize)
{
    std::string text = stripDisplayQuotes(value);
    size_t maxTextBytes = text.size();
    if (fixedSize > 0) {
        maxTextBytes = text.size() < fixedSize ? text.size() : fixedSize;
    }

    data.insert(data.end(), text.begin(), text.begin() + maxTextBytes);
    if (fixedSize == 0 || data.size() < fixedSize) {
        data.push_back(0);
    }
    if (fixedSize > 0 && data.size() < fixedSize) {
        data.resize(fixedSize, 0);
    }
}

std::vector<uint16_t> utf8ToUtf16Units(const std::string& text)
{
    std::vector<uint16_t> units;
    for (size_t i = 0; i < text.size();) {
        uint32_t cp = 0xFFFD;
        unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < 0x80) {
            cp = c;
            i += 1;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < text.size()) {
            unsigned char c1 = static_cast<unsigned char>(text[i + 1]);
            if ((c1 & 0xC0) == 0x80) {
                cp = ((c & 0x1F) << 6) | (c1 & 0x3F);
                i += 2;
            } else {
                i += 1;
            }
        } else if ((c & 0xF0) == 0xE0 && i + 2 < text.size()) {
            unsigned char c1 = static_cast<unsigned char>(text[i + 1]);
            unsigned char c2 = static_cast<unsigned char>(text[i + 2]);
            if ((c1 & 0xC0) == 0x80 && (c2 & 0xC0) == 0x80) {
                cp = ((c & 0x0F) << 12) | ((c1 & 0x3F) << 6) | (c2 & 0x3F);
                i += 3;
            } else {
                i += 1;
            }
        } else if ((c & 0xF8) == 0xF0 && i + 3 < text.size()) {
            unsigned char c1 = static_cast<unsigned char>(text[i + 1]);
            unsigned char c2 = static_cast<unsigned char>(text[i + 2]);
            unsigned char c3 = static_cast<unsigned char>(text[i + 3]);
            if ((c1 & 0xC0) == 0x80 && (c2 & 0xC0) == 0x80 && (c3 & 0xC0) == 0x80) {
                cp = ((c & 0x07) << 18) | ((c1 & 0x3F) << 12) |
                     ((c2 & 0x3F) << 6) | (c3 & 0x3F);
                i += 4;
            } else {
                i += 1;
            }
        } else {
            i += 1;
        }

        if (cp <= 0xFFFF) {
            units.push_back(static_cast<uint16_t>(cp));
        } else if (cp <= 0x10FFFF) {
            cp -= 0x10000;
            units.push_back(static_cast<uint16_t>(0xD800 | (cp >> 10)));
            units.push_back(static_cast<uint16_t>(0xDC00 | (cp & 0x3FF)));
        }
    }
    return units;
}

void appendDissectUtf16String(std::vector<unsigned char>& data, const std::string& value, size_t fixedSize)
{
    std::vector<uint16_t> units = utf8ToUtf16Units(stripDisplayQuotes(value));
    for (uint16_t unit : units) {
        if (fixedSize > 0 && data.size() + sizeof(unit) > fixedSize) {
            break;
        }
        appendDissectScalar(data, unit);
    }

    if (fixedSize == 0 || data.size() + 2 <= fixedSize) {
        appendDissectScalar<uint16_t>(data, 0);
    }
    if (fixedSize > 0 && data.size() < fixedSize) {
        data.resize(fixedSize, 0);
    }
}

bool parseUnsignedValue(const std::string& text, uint64_t& value, int defaultBase = 0)
{
    size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos || text[begin] == '-') {
        return false;
    }

    size_t endPos = begin;
    while (endPos < text.size() &&
           text[endPos] != ' ' &&
           text[endPos] != '\t' &&
           text[endPos] != '\r' &&
           text[endPos] != '\n' &&
           text[endPos] != '(' &&
           text[endPos] != ',') {
        endPos++;
    }

    std::string token = text.substr(begin, endPos - begin);
    if (token.empty()) {
        return false;
    }

    bool hasHexLetter = false;
    for (char c : token) {
        if ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) {
            hasHexLetter = true;
            break;
        }
    }

    int base = defaultBase;
    if (token.size() > 2 && token[0] == '0' && (token[1] == 'x' || token[1] == 'X')) {
        base = 0;
    } else if (base == 0) {
        base = hasHexLetter ? 16 : 10;
    }

    const char* str = token.c_str();
    char* end = nullptr;
    errno = 0;
    value = std::strtoull(str, &end, base);
    if (end == str || *end != '\0' || errno == ERANGE) {
        return false;
    }

    return true;
}

bool parseFloatValue(const std::string& text, float& value)
{
    const char* str = text.c_str();
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

bool parseDoubleValue(const std::string& text, double& value)
{
    const char* str = text.c_str();
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

void markNodesUnavailable(std::vector<DissectNode>& nodes)
{
    for (auto& node : nodes) {
        node.cachedValue = "??";
        node.pointerTarget = 0;
        if (!node.children.empty()) {
            markNodesUnavailable(node.children);
        }
    }
}
}

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
            if (readDissectMemory(structBaseAddress, dissectTotalSize, structBuffer)) {
                autoAnalyzeNodes(structBuffer);
                dissectAddrHistory.clear();
                dissectAddrHistory.push_back(structBaseAddress);
                dissectHistoryIdx = 0;
            } else {
                dissectNodes.clear();
                Gui::log("读取结构分析地址失败: 0x%llX", (unsigned long long)structBaseAddress);
            }
        }
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(dissectHistoryIdx <= 0);
    if (ImGui::Button("<< 后退")) {
        dissectHistoryIdx--;
        structBaseAddress = dissectAddrHistory[dissectHistoryIdx];
        if (readDissectMemory(structBaseAddress, dissectTotalSize, structBuffer)) {
            autoAnalyzeNodes(structBuffer);
        } else {
            dissectNodes.clear();
            Gui::log("读取结构分析地址失败: 0x%llX", (unsigned long long)structBaseAddress);
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(dissectHistoryIdx < 0 || dissectHistoryIdx >= (int)dissectAddrHistory.size() - 1);
    if (ImGui::Button("前进 >>")) {
        dissectHistoryIdx++;
        structBaseAddress = dissectAddrHistory[dissectHistoryIdx];
        if (readDissectMemory(structBaseAddress, dissectTotalSize, structBuffer)) {
            autoAnalyzeNodes(structBuffer);
        } else {
            dissectNodes.clear();
            Gui::log("读取结构分析地址失败: 0x%llX", (unsigned long long)structBaseAddress);
        }
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
            if (readDissectMemory(structBaseAddress, dissectTotalSize, structBuffer)) {
                autoAnalyzeNodes(structBuffer);
            } else {
                dissectNodes.clear();
                Gui::log("读取结构分析地址失败: 0x%llX", (unsigned long long)structBaseAddress);
            }
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
    if (dissectRefreshInterval < 0.1f) dissectRefreshInterval = 0.1f;
    ImGui::SameLine();
    ImGui::Text("秒");
    if (dissectAutoRefresh && structBaseAddress != 0 && !dissectNodes.empty()) {
        timeSinceDissectRefresh += ImGui::GetIO().DeltaTime;
        if (timeSinceDissectRefresh >= dissectRefreshInterval) {
            timeSinceDissectRefresh = 0.0f;
            if (readDissectMemory(structBaseAddress, dissectTotalSize, structBuffer)) {
                refreshDissectValues();
            } else {
                markNodesUnavailable(dissectNodes);
            }
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
    // 延迟执行偏移调整
    if (ops.pendingOffsetChangeFlat >= 0) {
        auto* parentVec = &dissectNodes;
        for (int p = 0; p < (int)ops.pendingOffsetChangePath.size() - 1; p++)
            parentVec = &(*parentVec)[ops.pendingOffsetChangePath[p]].children;
        int idx = ops.pendingOffsetChangePath.back();
        onDissectNodeOffsetChanged(*parentVec, idx, ops.pendingNewOffset);
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
    uint64_t nodeAddress = 0;
    const bool hasNodeAddress = addStructOffsetToAddress(baseAddr, node.offset, nodeAddress);

    // --- 偏移列：TreeNodeEx + 可编辑偏移 ---
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

    // 使用树节点实现缩进，但偏移值可通过 InputScalar 编辑
    char offsetLabel[64];
    snprintf(offsetLabel, sizeof(offsetLabel), "##tree_%d", myFlat);
    bool treeOpen = ImGui::TreeNodeEx(offsetLabel, treeFlags);

    // 在同一行追加可编辑的偏移输入框
    ImGui::SameLine();
    int editableOffset = node.offset;
    ImGui::PushItemWidth(60);
    char offsetFmt[16];
    snprintf(offsetFmt, sizeof(offsetFmt), "+0x%%03X");
    if (ImGui::InputScalar("##off", ImGuiDataType_S32, &editableOffset,
                           nullptr, nullptr, "+0x%03X",
                           ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue)) {
        if (editableOffset != node.offset && editableOffset >= 0) {
            ops.pendingOffsetChangeFlat = myFlat;
            ops.pendingNewOffset = editableOffset;
            ops.pendingOffsetChangePath = path;
        }
    }
    ImGui::PopItemWidth();

    // 右键菜单
    char ctxId[64];
    snprintf(ctxId, sizeof(ctxId), "NodeCtx_%d", myFlat);
    if (ImGui::BeginPopupContextItem(ctxId)) {
        if (ImGui::MenuItem("编辑属性...")) {
            dissectEditPath = path;
            showNodeEditPopup = true;
            std::snprintf(editNodeName, sizeof(editNodeName), "%s", node.name.c_str());
            std::snprintf(editNodeDesc, sizeof(editNodeDesc), "%s", node.description.c_str());
            editNodeTypeIdx = (int)node.type;
            editNodeStringSize = node.storedSize > 0 ? node.storedSize : 32;
            editNodeOffset = node.offset;
        }
        if (ImGui::MenuItem("浏览内存")) {
            if (hasNodeAddress) {
                jumpToAddress(nodeAddress);
            }
        }
        if (ImGui::MenuItem("添加到监控")) {
            if (hasNodeAddress) {
                MemoryWatchItem item;
                item.description = node.name;
                item.address = nodeAddress;
                item.type = node.type;
                item.enabled = true;
                watchItems.push_back(item);
                if (AppContext::Get().hasProcess())
                    watchItems.back().cachedValue = readWatchItemValue(watchItems.back());
            }
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
                if (readDissectMemory(structBaseAddress, dissectTotalSize, structBuffer)) {
                    regenerateDissectNodes();
                    refreshDissectValues();
                } else {
                    dissectNodes.clear();
                    Gui::log("读取指针目标内存失败: 0x%llX", (unsigned long long)structBaseAddress);
                }
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
    if (hasNodeAddress) {
        ImGui::TextColored(ColorScheme::Address, "%llX", (unsigned long long)nodeAddress);
    } else {
        ImGui::TextDisabled("??");
    }

    // --- 名称列（可编辑）---
    ImGui::TableSetColumnIndex(2);
    char nameBuf[128];
    std::snprintf(nameBuf, sizeof(nameBuf), "%s", node.name.c_str());
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
    if (!node.valueEditActive) {
        std::snprintf(node.editValueBuffer, sizeof(node.editValueBuffer), "%s", node.cachedValue.c_str());
    }
    ImGui::PushItemWidth(-1);
    bool valueSubmitted = ImGui::InputText(
        "##val",
        node.editValueBuffer,
        sizeof(node.editValueBuffer),
        ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::IsItemActivated()) {
        node.valueEditActive = true;
    }
    bool valueDeactivated = ImGui::IsItemDeactivated();
    if (valueSubmitted) {
        ops.pendingWriteFlat = myFlat;
        ops.pendingWriteValue = node.editValueBuffer;
        ops.pendingWritePath = path;
        node.valueEditActive = false;
    } else if (valueDeactivated) {
        node.valueEditActive = false;
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
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        ImGui::InputScalar("##editOff", ImGuiDataType_S32, &editNodeOffset,
                           nullptr, nullptr, "0x%X", ImGuiInputTextFlags_CharsHexadecimal);
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
            bool changedLayout = false;
            bool needsTypeOrSizeChange = newType != node->type;
            if ((newType == FieldType::STRING || newType == FieldType::STRING_UTF8 ||
                 newType == FieldType::STRING_UTF16) &&
                editNodeStringSize != node->storedSize) {
                needsTypeOrSizeChange = true;
            }

            if (needsTypeOrSizeChange) {
                auto* parentVec = &dissectNodes;
                for (int p = 0; p < (int)dissectEditPath.size() - 1; p++)
                    parentVec = &(*parentVec)[dissectEditPath[p]].children;
                int idx = dissectEditPath.back();
                onDissectNodeTypeChanged(*parentVec, idx, newType, editNodeStringSize);
                changedLayout = true;
            }
            node = resolveNodeByPath(dissectEditPath);
            if (!node) {
                ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
                return;
            }
            // 处理偏移变更
            if (editNodeOffset != node->offset && editNodeOffset >= 0) {
                auto* parentVec = &dissectNodes;
                for (int p = 0; p < (int)dissectEditPath.size() - 1; p++)
                    parentVec = &(*parentVec)[dissectEditPath[p]].children;
                int idx = dissectEditPath.back();
                onDissectNodeOffsetChanged(*parentVec, idx, editNodeOffset);
            } else {
                if (!changedLayout) {
                    refreshDissectValues();
                }
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
            uint64_t rawPointer = 0;
            memcpy(&rawPointer, &buffer[node.offset], sizeof(rawPointer));
            node.pointerTarget = normalizePointerAddress(rawPointer);
        }

        // 递归刷新已展开的子节点
        if (node.expanded && !node.children.empty() && node.pointerTarget != 0) {
            if (readDissectMemory(node.pointerTarget, dissectTotalSize, node.childBuffer)) {
                refreshNodeValues(node.children, node.childBuffer, node.pointerTarget);
            } else {
                markNodesUnavailable(node.children);
            }
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
    if (!readDissectMemory(node.pointerTarget, dissectTotalSize, node.childBuffer)) {
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
        uint64_t val64 = 0;
        if (readDissectScalar(data, offset, val64)) {
            uint64_t target = normalizePointerAddress(val64);
            if (isLikelyPointerAddress(target)) {
                child.type = FieldType::POINTER;
                child.storedSize = 8;
                child.pointerTarget = target;
                char nb[32]; snprintf(nb, sizeof(nb), "ptr_%d", fieldIndex++);
                child.name = nb;
                node.children.push_back(std::move(child));
                offset += 8;
                continue;
            }
        }

        // 检查浮点
        float fval = 0.0f;
        uint32_t ival = 0;
        if (readDissectScalar(data, offset, fval) && readDissectScalar(data, offset, ival)) {
            if (!std::isnan(fval) && !std::isinf(fval) && fval > -1000000 && fval < 1000000) {
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
                                                  int nodeIndex, FieldType newType,
                                                  int stringSize)
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
        if (stringSize < 1) stringSize = 1;
        if (stringSize > 4096) stringSize = 4096;
        node.storedSize = stringSize;
    } else {
        node.storedSize = 0;
    }

    int newSize = node.getSize();
    int nodeOffset = node.offset;
    int nodeDepth = node.depth;

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
        int fillOffset = nodeOffset + newSize;
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
            nr.depth = nodeDepth;
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
// onDissectNodeOffsetChanged — 偏移调整，重新排列节点
// ============================================================
void MemoryViewerWindow::onDissectNodeOffsetChanged(std::vector<DissectNode>& nodes,
                                                    int nodeIndex, int newOffset)
{
    if (nodeIndex < 0 || nodeIndex >= (int)nodes.size()) return;
    auto& node = nodes[nodeIndex];
    int oldOffset = node.offset;
    if (newOffset == oldOffset) return;

    int totalBytes = (int)structBuffer.size();
    if (newOffset < 0) newOffset = 0;
    if (newOffset + node.getSize() > totalBytes)
        newOffset = totalBytes - node.getSize();
    if (newOffset < 0) return;

    node.offset = newOffset;

    // 按偏移重新排序节点列表
    std::sort(nodes.begin(), nodes.end(), [](const DissectNode& a, const DissectNode& b) {
        return a.offset < b.offset;
    });

    // 检测并修复重叠：如果当前节点与前后节点有重叠，调整相邻节点
    for (int i = 0; i < (int)nodes.size() - 1; i++) {
        int endOfCurrent = nodes[i].offset + nodes[i].getSize();
        if (endOfCurrent > nodes[i + 1].offset) {
            // 当前节点末尾超过了下一个节点的起始，缩短或移除下一个节点
            // 策略：将下一个节点的偏移推到当前节点末尾
            nodes[i + 1].offset = endOfCurrent;
            if (nodes[i + 1].offset + nodes[i + 1].getSize() > totalBytes) {
                // 超出范围，移除该节点
                nodes.erase(nodes.begin() + i + 1);
                i--; // 重新检查当前位置
            }
        }
    }

    // 检查是否有间隙需要填充（可选：不自动填充，让用户自由控制）
    // 如果两个节点之间有空隙且空隙 >= dissectDefaultSize，插入填充节点
    FieldType fillType;
    switch (dissectDefaultSize) {
        case 1: fillType = FieldType::BYTE; break;
        case 2: fillType = FieldType::WORD; break;
        case 8: fillType = FieldType::QWORD; break;
        default: fillType = FieldType::DWORD; break;
    }

    // 填充首节点之前的空隙
    if (!nodes.empty() && nodes[0].offset >= dissectDefaultSize) {
        int gapStart = 0;
        int gapEnd = nodes[0].offset;
        int pos = 0;
        while (gapStart + dissectDefaultSize <= gapEnd) {
            DissectNode filler;
            filler.offset = gapStart;
            filler.type = fillType;
            filler.depth = nodes[0].depth;
            filler.storedSize = dissectDefaultSize;
            char nb[32];
            snprintf(nb, sizeof(nb), "field_%04X", gapStart);
            filler.name = nb;
            nodes.insert(nodes.begin() + pos, std::move(filler));
            gapStart += dissectDefaultSize;
            pos++;
        }
    }

    // 填充节点之间的空隙
    for (int i = 0; i < (int)nodes.size() - 1; i++) {
        int endOfCurrent = nodes[i].offset + nodes[i].getSize();
        int startOfNext = nodes[i + 1].offset;
        if (startOfNext - endOfCurrent >= dissectDefaultSize) {
            int gapStart = endOfCurrent;
            int insertPos = i + 1;
            while (gapStart + dissectDefaultSize <= startOfNext) {
                DissectNode filler;
                filler.offset = gapStart;
                filler.type = fillType;
                filler.depth = nodes[i].depth;
                filler.storedSize = dissectDefaultSize;
                char nb[32];
                snprintf(nb, sizeof(nb), "field_%04X", gapStart);
                filler.name = nb;
                nodes.insert(nodes.begin() + insertPos, std::move(filler));
                gapStart += dissectDefaultSize;
                insertPos++;
                i++;
            }
        }
    }

    refreshDissectValues();
}

// ============================================================
// writeDissectNodeValue — 写入值到内存
// ============================================================
bool MemoryViewerWindow::writeDissectNodeValue(DissectNode& node, uint64_t baseAddr,
                                               const std::string& valueStr)
{
    uint64_t addr = 0;
    if (!addStructOffsetToAddress(baseAddr, node.offset, addr)) {
        return false;
    }
    std::vector<unsigned char> data;
    try {
        switch (node.type) {
            case FieldType::BYTE: {
                uint64_t val = 0;
                if (!parseUnsignedValue(valueStr, val) || val > 0xFF) {
                    return false;
                }
                data.push_back(static_cast<unsigned char>(val));
                break;
            }
            case FieldType::WORD: {
                uint64_t val = 0;
                if (!parseUnsignedValue(valueStr, val) || val > 0xFFFF) {
                    return false;
                }
                appendDissectScalar(data, static_cast<uint16_t>(val));
                break;
            }
            case FieldType::DWORD: {
                uint64_t val = 0;
                if (!parseUnsignedValue(valueStr, val) || val > 0xFFFFFFFFULL) {
                    return false;
                }
                appendDissectScalar(data, static_cast<uint32_t>(val));
                break;
            }
            case FieldType::QWORD:
            case FieldType::POINTER: {
                uint64_t val = 0;
                if (!parseUnsignedValue(valueStr, val, 16)) {
                    return false;
                }
                appendDissectScalar(data, val);
                break;
            }
            case FieldType::FLOAT: {
                float val = 0.0f;
                if (!parseFloatValue(valueStr, val)) {
                    return false;
                }
                appendDissectScalar(data, val);
                break;
            }
            case FieldType::DOUBLE: {
                double val = 0.0;
                if (!parseDoubleValue(valueStr, val)) {
                    return false;
                }
                appendDissectScalar(data, val);
                break;
            }
            case FieldType::STRING:
            case FieldType::STRING_UTF8: {
                appendDissectString(data, valueStr, static_cast<size_t>(node.getSize()));
                break;
            }
            case FieldType::STRING_UTF16: {
                appendDissectUtf16String(data, valueStr, static_cast<size_t>(node.getSize()));
                break;
            }
            default:
                return false;
        }

        if (WriteProcessMemoryBytes(addr, (uint32_t)data.size(), data)) {
            Gui::log("写入 %s @ 0x%llX: %s", node.name.c_str(), (unsigned long long)addr, valueStr.c_str());
            // 刷新整棵树的值
            if (readDissectMemory(structBaseAddress, dissectTotalSize, structBuffer)) {
                refreshDissectValues();
            } else {
                markNodesUnavailable(dissectNodes);
            }
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
        if (!readDissectMemory(structBaseAddress, needed, structBuffer)) {
            structBuffer.clear();
            Gui::log("读取结构分析地址失败: 0x%llX", (unsigned long long)structBaseAddress);
        }
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
        uint64_t val64 = 0;
        if (readDissectScalar(data, offset, val64)) {
            uint64_t target = normalizePointerAddress(val64);
            if (isLikelyPointerAddress(target)) {
                node.type = FieldType::POINTER;
                node.storedSize = 8;
                node.pointerTarget = target;
                char nb[32]; snprintf(nb, sizeof(nb), "ptr_%d", fieldIndex++);
                node.name = nb;
                dissectNodes.push_back(std::move(node));
                offset += 8;
                continue;
            }
        }

        // 检查浮点
        float fval = 0.0f;
        uint32_t ival = 0;
        if (readDissectScalar(data, offset, fval) && readDissectScalar(data, offset, ival)) {
            if (!std::isnan(fval) && !std::isinf(fval) && fval > -1000000 && fval < 1000000) {
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
    size_t fieldOffset = 0;
    size_t fieldBytes = 0;
    if (!getStructFieldSpan(field, data.size(), fieldOffset, fieldBytes))
        return "超出范围";
    std::stringstream ss;
    if (field.arrayCount > 1) {
        ss << "{ ";
        for (int i = 0; i < field.arrayCount && i < 10; i++) {
            if (i > 0) ss << ", ";
            const size_t elemOffset = fieldOffset + static_cast<size_t>(i) * static_cast<size_t>(field.size);
            if (elemOffset <= data.size() &&
                static_cast<size_t>(field.size) <= data.size() - elemOffset &&
                elemOffset <= static_cast<size_t>((std::numeric_limits<int>::max)())) {
                ss << readSingleFieldValue(data, field.type, static_cast<int>(elemOffset), field.size);
            }
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
            uint16_t val = 0;
            if (readDissectScalar(data, offset, val)) {
                ss << "0x" << std::hex << std::uppercase << val
                   << " (" << std::dec << val << ")";
            }
            break;
        }
        case FieldType::DWORD: {
            uint32_t val = 0;
            if (readDissectScalar(data, offset, val)) {
                ss << "0x" << std::hex << std::uppercase << val
                   << " (" << std::dec << val << ")";
            }
            break;
        }
        case FieldType::QWORD:
        case FieldType::POINTER: {
            uint64_t val = 0;
            if (readDissectScalar(data, offset, val)) {
                ss << "0x" << std::hex << std::uppercase << val;
            }
            break;
        }
        case FieldType::FLOAT: {
            float val = 0.0f;
            if (readDissectScalar(data, offset, val)) {
                ss << std::fixed << std::setprecision(6) << val;
            }
            break;
        }
        case FieldType::DOUBLE: {
            double val = 0.0;
            if (readDissectScalar(data, offset, val)) {
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
    if (count > kMaxStructDefinitionCount) return;

    std::vector<StructDefinition> loadedDefinitions;
    loadedDefinitions.reserve(count);
    for (size_t i = 0; i < count; i++) {
        StructDefinition structDef;
        bool structValid = true;

        size_t nameLen;
        if (!file.read((char*)&nameLen, sizeof(nameLen)) || nameLen > kMaxStructStringLength) return;
        structDef.name.resize(nameLen);
        if (nameLen > 0 && !file.read(&structDef.name[0], nameLen)) return;

        size_t fieldCount;
        if (!file.read((char*)&fieldCount, sizeof(fieldCount)) || fieldCount > kMaxStructFieldCount) return;
        for (size_t j = 0; j < fieldCount; j++) {
            StructField field;
            size_t fieldNameLen;
            if (!file.read((char*)&fieldNameLen, sizeof(fieldNameLen)) ||
                fieldNameLen > kMaxStructStringLength) {
                structValid = false;
                break;
            }
            field.name.resize(fieldNameLen);
            if (fieldNameLen > 0 && !file.read(&field.name[0], fieldNameLen)) {
                structValid = false;
                break;
            }
            if (!file.read((char*)&field.type, sizeof(field.type)) ||
                !file.read((char*)&field.offset, sizeof(field.offset)) ||
                !file.read((char*)&field.size, sizeof(field.size)) ||
                !file.read((char*)&field.arrayCount, sizeof(field.arrayCount)) ||
                !file.read((char*)&field.isPointer, sizeof(field.isPointer))) {
                structValid = false;
                break;
            }
            if (!isValidStoredStructField(field)) {
                structValid = false;
                break;
            }

            size_t descLen;
            if (!file.read((char*)&descLen, sizeof(descLen)) ||
                descLen > kMaxStructStringLength) {
                structValid = false;
                break;
            }
            field.description.resize(descLen);
            if (descLen > 0 && !file.read(&field.description[0], descLen)) {
                structValid = false;
                break;
            }

            size_t structTypeLen;
            if (!file.read((char*)&structTypeLen, sizeof(structTypeLen)) ||
                structTypeLen > kMaxStructStringLength) {
                structValid = false;
                break;
            }
            field.structTypeName.resize(structTypeLen);
            if (structTypeLen > 0 && !file.read(&field.structTypeName[0], structTypeLen)) {
                structValid = false;
                break;
            }
            structDef.fields.push_back(field);
        }
        if (!structValid) return;
        int storedTotalSize = 0;
        if (!file.read((char*)&storedTotalSize, sizeof(storedTotalSize))) return;
        structDef.calculateSize();
        loadedDefinitions.push_back(structDef);
    }

    structDefinitions = std::move(loadedDefinitions);
}

// PLACEHOLDER_STRING_UTILS

std::string MemoryViewerWindow::readUTF8String(const std::vector<unsigned char>& data,
                                               size_t offset, size_t maxLength)
{
    std::string result;
    size_t i = offset;
    if (offset >= data.size()) {
        return result;
    }
    size_t end = data.size();
    if (maxLength < data.size() - offset) {
        end = offset + maxLength;
    }
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
        uint64_t fieldAddress = 0;
        if (!addStructOffsetToAddress(structBaseAddress, field.offset, fieldAddress)) {
            continue;
        }
        MemoryWatchItem item;
        item.description = structDef.name + "." + field.name;
        item.address = fieldAddress;
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
    uint64_t addr = 0;
    if (!addStructOffsetToAddress(structBaseAddress, field.offset, addr)) return false;
    if (field.size <= 0) return false;
    std::vector<unsigned char> data;
    try {
        switch (field.type) {
            case FieldType::BYTE: {
                uint64_t parsed = 0;
                if (!parseUnsignedValue(value, parsed) || parsed > 0xFF) return false;
                data.push_back(static_cast<unsigned char>(parsed));
                break;
            }
            case FieldType::WORD: {
                uint64_t parsed = 0;
                if (!parseUnsignedValue(value, parsed) || parsed > 0xFFFF) return false;
                appendDissectScalar(data, static_cast<uint16_t>(parsed));
                break;
            }
            case FieldType::DWORD: {
                uint64_t parsed = 0;
                if (!parseUnsignedValue(value, parsed) || parsed > 0xFFFFFFFFULL) return false;
                appendDissectScalar(data, static_cast<uint32_t>(parsed));
                break;
            }
            case FieldType::QWORD:
            case FieldType::POINTER: {
                uint64_t parsed = 0;
                if (!parseUnsignedValue(value, parsed, 16)) return false;
                appendDissectScalar(data, parsed);
                break;
            }
            case FieldType::FLOAT: {
                float parsed = 0.0f;
                if (!parseFloatValue(value, parsed)) return false;
                appendDissectScalar(data, parsed);
                break;
            }
            case FieldType::DOUBLE: {
                double parsed = 0.0;
                if (!parseDoubleValue(value, parsed)) return false;
                appendDissectScalar(data, parsed);
                break;
            }
            case FieldType::STRING:
            case FieldType::STRING_UTF8: {
                appendDissectString(data, value, static_cast<size_t>(field.size));
                break;
            }
            case FieldType::STRING_UTF16: {
                appendDissectUtf16String(data, value, static_cast<size_t>(field.size));
                break;
            }
            default: return false;
        }
        if (WriteProcessMemoryBytes(addr, (uint32_t)data.size(), data)) {
            const size_t fieldOffset = static_cast<size_t>(field.offset);
            if (fieldOffset <= structBuffer.size() &&
                data.size() <= structBuffer.size() - fieldOffset) {
                memcpy(structBuffer.data() + fieldOffset, data.data(), data.size());
            }
            return true;
        }
    } catch (const std::exception& e) {
        Gui::log("写入失败: %s", e.what());
    }
    return false;
}
