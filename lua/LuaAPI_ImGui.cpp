#include "LuaAPI_ImGui.h"
#include "LuaAPI.h"
#include "../gui/Gui.h"
#include "../gui/LuaImGuiWindow.h"
#include "../imgui/imgui.h"
#include <string>
#include <vector>
#include <map>
#include <array>
#include <cstring>
#include <utility>

// 全局窗口管理器：窗口ID -> 窗口指针
static std::map<int, LuaImGuiWindow*> luaWindows;

struct LuaInputTextState {
    std::array<char, 256> buffer{};
    std::string lastExternalValue;
    bool activeLastFrame = false;
};

static std::map<std::pair<lua_State*, ImGuiID>, LuaInputTextState> luaInputTextStates;

// ==================== 辅助函数 ====================
ImVec4 LuaAPI_ImGui::ParseColor(lua_State* L, int index) {
    if (lua_istable(L, index)) {
        lua_pushinteger(L, 1);
        lua_gettable(L, index);
        float r = static_cast<float>(luaL_optnumber(L, -1, 1.0));
        lua_pop(L, 1);
        
        lua_pushinteger(L, 2);
        lua_gettable(L, index);
        float g = static_cast<float>(luaL_optnumber(L, -1, 1.0));
        lua_pop(L, 1);
        
        lua_pushinteger(L, 3);
        lua_gettable(L, index);
        float b = static_cast<float>(luaL_optnumber(L, -1, 1.0));
        lua_pop(L, 1);
        
        lua_pushinteger(L, 4);
        lua_gettable(L, index);
        float a = static_cast<float>(luaL_optnumber(L, -1, 1.0));
        lua_pop(L, 1);
        
        return ImVec4(r, g, b, a);
    } else if (lua_isnumber(L, index)) {
        float r = static_cast<float>(lua_tonumber(L, index));
        float g = static_cast<float>(luaL_optnumber(L, index + 1, 1.0));
        float b = static_cast<float>(luaL_optnumber(L, index + 2, 1.0));
        float a = static_cast<float>(luaL_optnumber(L, index + 3, 1.0));
        return ImVec4(r, g, b, a);
    }
    return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
}

void LuaAPI_ImGui::PushVec2(lua_State* L, const ImVec2& vec) {
    lua_newtable(L);
    lua_pushnumber(L, vec.x);
    lua_setfield(L, -2, "x");
    lua_pushnumber(L, vec.y);
    lua_setfield(L, -2, "y");
}

ImVec2 LuaAPI_ImGui::GetVec2(lua_State* L, int index) {
    if (lua_istable(L, index)) {
        lua_getfield(L, index, "x");
        float x = static_cast<float>(luaL_optnumber(L, -1, 0.0));
        lua_pop(L, 1);
        
        lua_getfield(L, index, "y");
        float y = static_cast<float>(luaL_optnumber(L, -1, 0.0));
        lua_pop(L, 1);
        
        return ImVec2(x, y);
    } else if (lua_isnumber(L, index)) {
        float x = static_cast<float>(lua_tonumber(L, index));
        float y = static_cast<float>(luaL_optnumber(L, index + 1, 0.0));
        return ImVec2(x, y);
    }
    return ImVec2(0, 0);
}

// ==================== 注册 ImGui API ====================
void LuaAPI_ImGui::Register(lua_State* L) {
    // 创建imgui表
    lua_newtable(L);
    
    // 窗口管理
    lua_pushcfunction(L, CreateWindow);
    lua_setfield(L, -2, "createWindow");
    lua_pushcfunction(L, DestroyWindow);
    lua_setfield(L, -2, "destroyWindow");
    lua_pushcfunction(L, IsWindowOpen);
    lua_setfield(L, -2, "isWindowOpen");
    lua_pushcfunction(L, SetWindowOpen);
    lua_setfield(L, -2, "setWindowOpen");
    
    // 窗口控制
    lua_pushcfunction(L, Begin);
    lua_setfield(L, -2, "begin");
    lua_pushcfunction(L, End);
    lua_setfield(L, -2, "end");
    lua_pushcfunction(L, BeginChild);
    lua_setfield(L, -2, "beginChild");
    lua_pushcfunction(L, EndChild);
    lua_setfield(L, -2, "endChild");
    
    // 文本和显示
    lua_pushcfunction(L, Text);
    lua_setfield(L, -2, "text");
    lua_pushcfunction(L, TextColored);
    lua_setfield(L, -2, "textColored");
    lua_pushcfunction(L, TextWrapped);
    lua_setfield(L, -2, "textWrapped");
    lua_pushcfunction(L, Separator);
    lua_setfield(L, -2, "separator");
    lua_pushcfunction(L, Spacing);
    lua_setfield(L, -2, "spacing");
    lua_pushcfunction(L, NewLine);
    lua_setfield(L, -2, "newLine");
    
    // 按钮和输入
    lua_pushcfunction(L, Button);
    lua_setfield(L, -2, "button");
    lua_pushcfunction(L, SmallButton);
    lua_setfield(L, -2, "smallButton");
    lua_pushcfunction(L, Checkbox);
    lua_setfield(L, -2, "checkbox");
    lua_pushcfunction(L, InputText);
    lua_setfield(L, -2, "inputText");
    lua_pushcfunction(L, InputInt);
    lua_setfield(L, -2, "inputInt");
    lua_pushcfunction(L, InputFloat);
    lua_setfield(L, -2, "inputFloat");
    lua_pushcfunction(L, SliderInt);
    lua_setfield(L, -2, "sliderInt");
    lua_pushcfunction(L, SliderFloat);
    lua_setfield(L, -2, "sliderFloat");
    
    // 布局
    lua_pushcfunction(L, SameLine);
    lua_setfield(L, -2, "sameLine");
    lua_pushcfunction(L, Columns);
    lua_setfield(L, -2, "columns");
    lua_pushcfunction(L, NextColumn);
    lua_setfield(L, -2, "nextColumn");
    lua_pushcfunction(L, SetColumnWidth);
    lua_setfield(L, -2, "setColumnWidth");
    
    // 树形和折叠
    lua_pushcfunction(L, TreeNode);
    lua_setfield(L, -2, "treeNode");
    lua_pushcfunction(L, TreePop);
    lua_setfield(L, -2, "treePop");
    lua_pushcfunction(L, CollapsingHeader);
    lua_setfield(L, -2, "collapsingHeader");
    
    // 列表和选择
    lua_pushcfunction(L, Selectable);
    lua_setfield(L, -2, "selectable");
    lua_pushcfunction(L, ListBox);
    lua_setfield(L, -2, "listBox");
    
    // 表格
    lua_pushcfunction(L, BeginTable);
    lua_setfield(L, -2, "beginTable");
    lua_pushcfunction(L, EndTable);
    lua_setfield(L, -2, "endTable");
    lua_pushcfunction(L, TableNextRow);
    lua_setfield(L, -2, "tableNextRow");
    lua_pushcfunction(L, TableNextColumn);
    lua_setfield(L, -2, "tableNextColumn");
    lua_pushcfunction(L, TableSetColumnIndex);
    lua_setfield(L, -2, "tableSetColumnIndex");
    
    // 其他
    lua_pushcfunction(L, IsItemClicked);
    lua_setfield(L, -2, "isItemClicked");
    lua_pushcfunction(L, IsItemHovered);
    lua_setfield(L, -2, "isItemHovered");
    lua_pushcfunction(L, GetWindowSize);
    lua_setfield(L, -2, "getWindowSize");
    lua_pushcfunction(L, SetWindowSize);
    lua_setfield(L, -2, "setWindowSize");
    lua_pushcfunction(L, GetWindowPos);
    lua_setfield(L, -2, "getWindowPos");
    lua_pushcfunction(L, SetWindowPos);
    lua_setfield(L, -2, "setWindowPos");
    
    lua_setglobal(L, "imgui");
}

// ==================== 窗口管理API ====================
int LuaAPI_ImGui::CreateWindow(lua_State* L) {
    const char* windowName = luaL_checkstring(L, 1);
    const char* callbackName = luaL_checkstring(L, 2);
    
    auto* window = new LuaImGuiWindow(windowName, callbackName);
    int windowId = window->GetWindowId();
    luaWindows[windowId] = window;
    
    Gui::addWindow(window);
    
    lua_pushinteger(L, windowId);
    return 1;
}

int LuaAPI_ImGui::DestroyWindow(lua_State* L) {
    int windowId = static_cast<int>(luaL_checkinteger(L, 1));
    
    auto it = luaWindows.find(windowId);
    if (it != luaWindows.end()) {
        it->second->pOpen = false;
        luaWindows.erase(it);
        lua_pushboolean(L, 1);
    } else {
        lua_pushboolean(L, 0);
    }
    return 1;
}

int LuaAPI_ImGui::IsWindowOpen(lua_State* L) {
    int windowId = static_cast<int>(luaL_checkinteger(L, 1));
    
    auto it = luaWindows.find(windowId);
    if (it != luaWindows.end()) {
        lua_pushboolean(L, it->second->pOpen ? 1 : 0);
    } else {
        lua_pushboolean(L, 0);
    }
    return 1;
}

int LuaAPI_ImGui::SetWindowOpen(lua_State* L) {
    int windowId = static_cast<int>(luaL_checkinteger(L, 1));
    bool open = lua_toboolean(L, 2) != 0;
    
    auto it = luaWindows.find(windowId);
    if (it != luaWindows.end()) {
        it->second->pOpen = open;
        lua_pushboolean(L, 1);
    } else {
        lua_pushboolean(L, 0);
    }
    return 1;
}

// ==================== 窗口控制API ====================
int LuaAPI_ImGui::Begin(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    bool* pOpen = nullptr;
    if (lua_isboolean(L, 2)) {
        bool open = lua_toboolean(L, 2) != 0;
        pOpen = &open;
    }
    
    int flags = static_cast<int>(luaL_optinteger(L, 3, 0));
    bool result = ImGui::Begin(name, pOpen, flags);
    lua_pushboolean(L, result ? 1 : 0);
    return 1;
}

int LuaAPI_ImGui::End(lua_State* L) {
    ImGui::End();
    return 0;
}

int LuaAPI_ImGui::BeginChild(lua_State* L) {
    const char* strId = luaL_checkstring(L, 1);
    ImVec2 size = GetVec2(L, 2);
    bool border = lua_toboolean(L, 3) != 0;
    int flags = static_cast<int>(luaL_optinteger(L, 4, 0));
    
    bool result = ImGui::BeginChild(strId, size, border, flags);
    lua_pushboolean(L, result ? 1 : 0);
    return 1;
}

int LuaAPI_ImGui::EndChild(lua_State* L) {
    ImGui::EndChild();
    return 0;
}

// ==================== 文本和显示API ====================
int LuaAPI_ImGui::Text(lua_State* L) {
    const char* text = luaL_checkstring(L, 1);
    ImGui::TextUnformatted(text);
    return 0;
}

int LuaAPI_ImGui::TextColored(lua_State* L) {
    ImVec4 color = ParseColor(L, 1);
    const char* text = luaL_checkstring(L, 2);
    ImGui::TextColored(color, "%s", text);
    return 0;
}

int LuaAPI_ImGui::TextWrapped(lua_State* L) {
    const char* text = luaL_checkstring(L, 1);
    ImGui::TextWrapped("%s", text);
    return 0;
}

int LuaAPI_ImGui::Separator(lua_State* L) {
    ImGui::Separator();
    return 0;
}

int LuaAPI_ImGui::Spacing(lua_State* L) {
    ImGui::Spacing();
    return 0;
}

int LuaAPI_ImGui::NewLine(lua_State* L) {
    ImGui::NewLine();
    return 0;
}

// ==================== 按钮和输入API ====================
int LuaAPI_ImGui::Button(lua_State* L) {
    const char* label = luaL_checkstring(L, 1);
    ImVec2 size = GetVec2(L, 2);
    
    bool result = ImGui::Button(label, size);
    lua_pushboolean(L, result ? 1 : 0);
    return 1;
}

int LuaAPI_ImGui::SmallButton(lua_State* L) {
    const char* label = luaL_checkstring(L, 1);
    bool result = ImGui::SmallButton(label);
    lua_pushboolean(L, result ? 1 : 0);
    return 1;
}

int LuaAPI_ImGui::Checkbox(lua_State* L) {
    const char* label = luaL_checkstring(L, 1);
    
    bool value = false;
    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "value");
        if (lua_isboolean(L, -1)) {
            value = lua_toboolean(L, -1) != 0;
        }
        lua_pop(L, 1);
    } else if (lua_isboolean(L, 2)) {
        value = lua_toboolean(L, 2) != 0;
    }
    
    bool result = ImGui::Checkbox(label, &value);
    
    if (lua_istable(L, 2)) {
        lua_pushboolean(L, value ? 1 : 0);
        lua_setfield(L, 2, "value");
    } else {
        lua_pushboolean(L, value ? 1 : 0);
    }
    
    lua_pushboolean(L, result ? 1 : 0);
    return 2;
}

int LuaAPI_ImGui::InputText(lua_State* L) {
    const char* label = luaL_checkstring(L, 1);

    size_t len;
    const char* str = luaL_checklstring(L, 2, &len);
    std::string externalValue(str, len);

    ImGuiID id = ImGui::GetID(label);
    LuaInputTextState& state = luaInputTextStates[{L, id}];
    if (state.lastExternalValue != externalValue && !state.activeLastFrame) {
        size_t copyLen = (externalValue.size() < state.buffer.size() - 1) ? externalValue.size() : state.buffer.size() - 1;
        std::memcpy(state.buffer.data(), externalValue.data(), copyLen);
        state.buffer[copyLen] = '\0';
        state.lastExternalValue = externalValue.substr(0, copyLen);
    }

    int flags = static_cast<int>(luaL_optinteger(L, 3, 0));
    bool result = ImGui::InputText(label, state.buffer.data(), state.buffer.size(), flags);
    state.activeLastFrame = ImGui::IsItemActive();

    if (result) {
        state.lastExternalValue = state.buffer.data();
        lua_pushstring(L, state.buffer.data());
    } else {
        lua_pushstring(L, state.buffer.data());
    }
    lua_pushboolean(L, result ? 1 : 0);
    return 2;
}

int LuaAPI_ImGui::InputInt(lua_State* L) {
    const char* label = luaL_checkstring(L, 1);
    int value = static_cast<int>(luaL_checkinteger(L, 2));
    int step = static_cast<int>(luaL_optinteger(L, 3, 1));
    int stepFast = static_cast<int>(luaL_optinteger(L, 4, 100));
    int flags = static_cast<int>(luaL_optinteger(L, 5, 0));
    
    bool result = ImGui::InputInt(label, &value, step, stepFast, flags);
    lua_pushinteger(L, value);
    lua_pushboolean(L, result ? 1 : 0);
    return 2;
}

int LuaAPI_ImGui::InputFloat(lua_State* L) {
    const char* label = luaL_checkstring(L, 1);
    float value = static_cast<float>(luaL_checknumber(L, 2));
    float step = static_cast<float>(luaL_optnumber(L, 3, 0.0));
    float stepFast = static_cast<float>(luaL_optnumber(L, 4, 0.0));
    const char* format = luaL_optstring(L, 5, "%.3f");
    int flags = static_cast<int>(luaL_optinteger(L, 6, 0));
    
    bool result = ImGui::InputFloat(label, &value, step, stepFast, format, flags);
    lua_pushnumber(L, value);
    lua_pushboolean(L, result ? 1 : 0);
    return 2;
}

int LuaAPI_ImGui::SliderInt(lua_State* L) {
    const char* label = luaL_checkstring(L, 1);
    int value = static_cast<int>(luaL_checkinteger(L, 2));
    int minVal = static_cast<int>(luaL_checkinteger(L, 3));
    int maxVal = static_cast<int>(luaL_checkinteger(L, 4));
    const char* format = luaL_optstring(L, 5, "%d");
    
    bool result = ImGui::SliderInt(label, &value, minVal, maxVal, format);
    lua_pushinteger(L, value);
    lua_pushboolean(L, result ? 1 : 0);
    return 2;
}

int LuaAPI_ImGui::SliderFloat(lua_State* L) {
    const char* label = luaL_checkstring(L, 1);
    float value = static_cast<float>(luaL_checknumber(L, 2));
    float minVal = static_cast<float>(luaL_checknumber(L, 3));
    float maxVal = static_cast<float>(luaL_checknumber(L, 4));
    const char* format = luaL_optstring(L, 5, "%.3f");
    
    bool result = ImGui::SliderFloat(label, &value, minVal, maxVal, format);
    lua_pushnumber(L, value);
    lua_pushboolean(L, result ? 1 : 0);
    return 2;
}

// ==================== 布局API ====================
int LuaAPI_ImGui::SameLine(lua_State* L) {
    float offsetX = static_cast<float>(luaL_optnumber(L, 1, 0.0));
    float spacing = static_cast<float>(luaL_optnumber(L, 2, -1.0));
    ImGui::SameLine(offsetX, spacing);
    return 0;
}

int LuaAPI_ImGui::Columns(lua_State* L) {
    int count = static_cast<int>(luaL_optinteger(L, 1, 1));
    const char* id = luaL_optstring(L, 2, nullptr);
    bool border = lua_toboolean(L, 3) != 0;
    
    ImGui::Columns(count, id, border);
    return 0;
}

int LuaAPI_ImGui::NextColumn(lua_State* L) {
    ImGui::NextColumn();
    return 0;
}

int LuaAPI_ImGui::SetColumnWidth(lua_State* L) {
    int columnIndex = static_cast<int>(luaL_checkinteger(L, 1));
    float width = static_cast<float>(luaL_checknumber(L, 2));
    ImGui::SetColumnWidth(columnIndex, width);
    return 0;
}

// ==================== 树形和折叠API ====================
int LuaAPI_ImGui::TreeNode(lua_State* L) {
    const char* label = luaL_checkstring(L, 1);
    bool result = ImGui::TreeNode(label);
    lua_pushboolean(L, result ? 1 : 0);
    return 1;
}

int LuaAPI_ImGui::TreePop(lua_State* L) {
    ImGui::TreePop();
    return 0;
}

int LuaAPI_ImGui::CollapsingHeader(lua_State* L) {
    const char* label = luaL_checkstring(L, 1);
    int flags = static_cast<int>(luaL_optinteger(L, 2, 0));
    bool result = ImGui::CollapsingHeader(label, flags);
    lua_pushboolean(L, result ? 1 : 0);
    return 1;
}

// ==================== 列表和选择API ====================
int LuaAPI_ImGui::Selectable(lua_State* L) {
    const char* label = luaL_checkstring(L, 1);
    bool selected = lua_toboolean(L, 2) != 0;
    int flags = static_cast<int>(luaL_optinteger(L, 3, 0));
    ImVec2 size = GetVec2(L, 4);
    
    bool result = ImGui::Selectable(label, selected, flags, size);
    lua_pushboolean(L, result ? 1 : 0);
    return 1;
}

int LuaAPI_ImGui::ListBox(lua_State* L) {
    const char* label = luaL_checkstring(L, 1);
    int currentItem = static_cast<int>(luaL_checkinteger(L, 2)) - 1;
    if (currentItem < 0) currentItem = 0;
    
    if (!lua_istable(L, 3)) {
        luaL_error(L, "Expected table for items");
    }
    
    int len = static_cast<int>(lua_objlen(L, 3));
    std::vector<std::string> itemStrings;
    itemStrings.reserve(len);
    
    for (int i = 1; i <= len; ++i) {
        lua_pushinteger(L, i);
        lua_gettable(L, 3);
        if (lua_isstring(L, -1)) {
            const char* str = lua_tostring(L, -1);
            if (str) {
                itemStrings.push_back(std::string(str));
            } else {
                itemStrings.push_back(std::string(""));
            }
        } else {
            itemStrings.push_back(std::string(""));
        }
        lua_pop(L, 1);
    }
    
    if (currentItem >= static_cast<int>(itemStrings.size())) {
        currentItem = static_cast<int>(itemStrings.size()) - 1;
    }
    if (currentItem < 0 && !itemStrings.empty()) {
        currentItem = 0;
    }
    
    std::vector<const char*> items;
    items.reserve(itemStrings.size());
    for (const auto& str : itemStrings) {
        items.push_back(str.c_str());
    }
    
    bool result = false;
    if (!items.empty() && currentItem >= 0) {
        result = ImGui::ListBox(label, &currentItem, items.data(), static_cast<int>(items.size()));
        currentItem = currentItem + 1;
    }
    
    lua_pushinteger(L, currentItem);
    lua_pushboolean(L, result ? 1 : 0);
    return 2;
}

// ==================== 表格API ====================
int LuaAPI_ImGui::BeginTable(lua_State* L) {
    const char* strId = luaL_checkstring(L, 1);
    int column = static_cast<int>(luaL_checkinteger(L, 2));
    int flags = static_cast<int>(luaL_optinteger(L, 3, 0));
    ImVec2 outerSize = GetVec2(L, 4);
    float innerWidth = static_cast<float>(luaL_optnumber(L, 5, 0.0));
    
    bool result = ImGui::BeginTable(strId, column, flags, outerSize, innerWidth);
    lua_pushboolean(L, result ? 1 : 0);
    return 1;
}

int LuaAPI_ImGui::EndTable(lua_State* L) {
    ImGui::EndTable();
    return 0;
}

int LuaAPI_ImGui::TableNextRow(lua_State* L) {
    int rowFlags = static_cast<int>(luaL_optinteger(L, 1, 0));
    float minRowHeight = static_cast<float>(luaL_optnumber(L, 2, 0.0));
    ImGui::TableNextRow(rowFlags, minRowHeight);
    return 0;
}

int LuaAPI_ImGui::TableNextColumn(lua_State* L) {
    bool result = ImGui::TableNextColumn();
    lua_pushboolean(L, result ? 1 : 0);
    return 1;
}

int LuaAPI_ImGui::TableSetColumnIndex(lua_State* L) {
    int columnN = static_cast<int>(luaL_checkinteger(L, 1));
    bool result = ImGui::TableSetColumnIndex(columnN);
    lua_pushboolean(L, result ? 1 : 0);
    return 1;
}

// ==================== 其他API ====================
int LuaAPI_ImGui::IsItemClicked(lua_State* L) {
    int button = static_cast<int>(luaL_optinteger(L, 1, 0));
    bool result = ImGui::IsItemClicked(button);
    lua_pushboolean(L, result ? 1 : 0);
    return 1;
}

int LuaAPI_ImGui::IsItemHovered(lua_State* L) {
    int flags = static_cast<int>(luaL_optinteger(L, 1, 0));
    bool result = ImGui::IsItemHovered(flags);
    lua_pushboolean(L, result ? 1 : 0);
    return 1;
}

int LuaAPI_ImGui::GetWindowSize(lua_State* L) {
    ImVec2 size = ImGui::GetWindowSize();
    PushVec2(L, size);
    return 1;
}

int LuaAPI_ImGui::SetWindowSize(lua_State* L) {
    ImVec2 size = GetVec2(L, 1);
    int cond = static_cast<int>(luaL_optinteger(L, 2, 0));
    ImGui::SetWindowSize(size, cond);
    return 0;
}

int LuaAPI_ImGui::GetWindowPos(lua_State* L) {
    ImVec2 pos = ImGui::GetWindowPos();
    PushVec2(L, pos);
    return 1;
}

int LuaAPI_ImGui::SetWindowPos(lua_State* L) {
    ImVec2 pos = GetVec2(L, 1);
    int cond = static_cast<int>(luaL_optinteger(L, 2, 0));
    ImGui::SetWindowPos(pos, cond);
    return 0;
}

