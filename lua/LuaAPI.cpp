#include "LuaAPI.h"
#include "LuaAPI_Memory.h"
#include "LuaAPI_ImGui.h"
#include "LuaAPI_Assembly.h"
#include "../socket/socket_io_timeout.h"
#include "../mem/IMemService.h"
#include "../gui/Gui.h"
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <algorithm>
#include <cstring>
#include <sstream>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <limits>

// 辅助函数：将 Lua 值转换为字符串（Lua 5.1 兼容版本）
static const char* luaL_tolstring_compat(lua_State* L, int idx, size_t* len) {
    // Lua 5.1 兼容：手动计算绝对索引
    int absidx = (idx < 0) ? lua_gettop(L) + idx + 1 : idx;
    int type = lua_type(L, absidx);
    
    switch (type) {
        case LUA_TSTRING:
            return lua_tolstring(L, absidx, len);
        case LUA_TNUMBER: {
            lua_Number num = lua_tonumber(L, absidx);
            std::ostringstream oss;
            oss << num;
            std::string str = oss.str();
            lua_pushlstring(L, str.c_str(), str.length());
            if (len) *len = str.length();
            return lua_tostring(L, -1);
        }
        case LUA_TBOOLEAN: {
            int b = lua_toboolean(L, absidx);
            const char* str = b ? "true" : "false";
            lua_pushstring(L, str);
            if (len) *len = strlen(str);
            return str;
        }
        case LUA_TNIL: {
            const char* str = "nil";
            lua_pushstring(L, str);
            if (len) *len = strlen(str);
            return str;
        }
        case LUA_TTABLE: {
            const char* str = "table";
            lua_pushstring(L, str);
            if (len) *len = strlen(str);
            return str;
        }
        case LUA_TFUNCTION: {
            const char* str = "function";
            lua_pushstring(L, str);
            if (len) *len = strlen(str);
            return str;
        }
        case LUA_TUSERDATA: {
            const char* str = "userdata";
            lua_pushstring(L, str);
            if (len) *len = strlen(str);
            return str;
        }
        case LUA_TTHREAD: {
            const char* str = "thread";
            lua_pushstring(L, str);
            if (len) *len = strlen(str);
            return str;
        }
        case LUA_TLIGHTUSERDATA: {
            const char* str = "lightuserdata";
            lua_pushstring(L, str);
            if (len) *len = strlen(str);
            return str;
        }
        default: {
            const char* str = lua_typename(L, type);
            lua_pushstring(L, str);
            if (len) *len = strlen(str);
            return str;
        }
    }
}

// 辅助函数：检查地址参数
namespace {
constexpr size_t kMaxLuaOffsetCount = 1024;
constexpr int kLuaSleepPollMs = 50;
char g_memServiceRegistryKey;
char g_operationContextRegistryKey;

bool isValidBreakpointSize(uint32_t size) {
    return size == 1 || size == 2 || size == 4 || size == 8;
}

uint32_t checkBreakpointSize(lua_State* L, int index, uint32_t defaultValue) {
    lua_Integer raw = luaL_optinteger(L, index, defaultValue);
    if (raw < 0 || raw > (std::numeric_limits<uint32_t>::max)()) {
        luaL_error(L, "Breakpoint size out of range");
        return 0;
    }

    const uint32_t size = static_cast<uint32_t>(raw);
    if (!isValidBreakpointSize(size)) {
        luaL_error(L, "Breakpoint size must be 1, 2, 4, or 8");
        return 0;
    }
    return size;
}
} // namespace

uint64_t LuaAPI::CheckAddress(lua_State* L, int index) {
    if (lua_isnumber(L, index)) {
        lua_Number number = lua_tonumber(L, index);
        if (!std::isfinite(static_cast<double>(number)) || number < 0) {
            luaL_error(L, "Address must be a non-negative finite number");
            return 0;
        }
        return static_cast<uint64_t>(number);
    } else if (lua_isstring(L, index)) {
        // 支持十六进制字符串 "0x12345678"
        const char* str = lua_tostring(L, index);
        const char* begin = str;
        while (*begin == ' ' || *begin == '\t' || *begin == '\r' || *begin == '\n') {
            ++begin;
        }
        if (*begin == '-' || *begin == '\0') {
            luaL_error(L, "Invalid address string");
            return 0;
        }

        char* end = nullptr;
        errno = 0;
        uint64_t address = std::strtoull(begin, &end, 0);
        if (end == begin || errno == ERANGE) {
            luaL_error(L, "Invalid address string");
            return 0;
        }
        while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') {
            ++end;
        }
        if (*end != '\0') {
            luaL_error(L, "Invalid address string");
            return 0;
        }
        return address;
    }
    luaL_error(L, "Expected number or hex string for address");
    return 0;
}

void LuaAPI::PushError(lua_State* L, const std::string& msg) {
    lua_pushnil(L);
    lua_pushstring(L, msg.c_str());
}

// ==================== 注册所有API ====================
Mem::IMemService& LuaAPI::GetMemService(lua_State* L) {
    lua_pushlightuserdata(L, &g_memServiceRegistryKey);
    lua_gettable(L, LUA_REGISTRYINDEX);
    auto* service =
        static_cast<Mem::IMemService*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (!service) {
        luaL_error(L, "AMem memory service is not registered");
        std::abort();
    }
    return *service;
}

Mem::OperationContext LuaAPI::GetOperationContext(
    lua_State* L, bool includeTarget) {
    lua_pushlightuserdata(L, &g_operationContextRegistryKey);
    lua_gettable(L, LUA_REGISTRYINDEX);
    const auto* bound = static_cast<const Mem::OperationContext*>(
        lua_touserdata(L, -1));
    lua_pop(L, 1);

    if (!bound) {
        return GetMemService(L).captureContext(includeTarget);
    }

    Mem::OperationContext context = *bound;
    if (!includeTarget) {
        context.target.reset();
    }
    return context;
}

const Mem::OperationContext* LuaAPI::BindOperationContext(
    lua_State* L, const Mem::OperationContext* context) {
    lua_pushlightuserdata(L, &g_operationContextRegistryKey);
    lua_gettable(L, LUA_REGISTRYINDEX);
    const auto* previous = static_cast<const Mem::OperationContext*>(
        lua_touserdata(L, -1));
    lua_pop(L, 1);

    lua_pushlightuserdata(L, &g_operationContextRegistryKey);
    if (context) {
        lua_pushlightuserdata(
            L, const_cast<Mem::OperationContext*>(context));
    } else {
        lua_pushnil(L);
    }
    lua_settable(L, LUA_REGISTRYINDEX);
    return previous;
}

void LuaAPI::RegisterAll(lua_State* L, Mem::IMemService& memService) {
    lua_pushlightuserdata(L, &g_memServiceRegistryKey);
    lua_pushlightuserdata(L, &memService);
    lua_settable(L, LUA_REGISTRYINDEX);
    // 注册内存操作 API
    LuaAPI_Memory::Register(L);

    // 创建process表
    lua_newtable(L);
    lua_pushcfunction(L, GetProcessList);
    lua_setfield(L, -2, "list");
    lua_pushcfunction(L, AttachProcess);
    lua_setfield(L, -2, "attach");
    lua_pushcfunction(L, GetCurrentPid);
    lua_setfield(L, -2, "getCurrent");
    lua_setglobal(L, "process");

    // 创建module表
    lua_newtable(L);
    lua_pushcfunction(L, GetModuleList);
    lua_setfield(L, -2, "list");
    lua_pushcfunction(L, GetModuleBase);
    lua_setfield(L, -2, "getBase");
    lua_pushcfunction(L, ResolveOffsetChain);
    lua_setfield(L, -2, "resolveOffsetChain");
    lua_setglobal(L, "module");

    // 创建scan表
    lua_newtable(L);
    lua_pushcfunction(L, ScanFirst);
    lua_setfield(L, -2, "first");
    lua_pushcfunction(L, ScanNext);
    lua_setfield(L, -2, "next");
    lua_pushcfunction(L, GetScanResults);
    lua_setfield(L, -2, "getResults");
    lua_pushcfunction(L, ClearScanResults);
    lua_setfield(L, -2, "clear");
    lua_pushcfunction(L, ScanFuzzy);
    lua_setfield(L, -2, "fuzzy");
    lua_setglobal(L, "scan");

    // 创建bp表
    lua_newtable(L);
    lua_pushcfunction(L, SetBreakpoint);
    lua_setfield(L, -2, "set");
    lua_pushcfunction(L, RemoveBreakpoint);
    lua_setfield(L, -2, "remove");
    lua_pushcfunction(L, SuspendBreakpoint);
    lua_setfield(L, -2, "suspend");
    lua_pushcfunction(L, ResumeBreakpoint);
    lua_setfield(L, -2, "resume");
    lua_pushcfunction(L, GetBreakpointInfo);
    lua_setfield(L, -2, "getInfo");
    lua_setglobal(L, "bp");

    // 全局函数
    lua_pushcfunction(L, Log);
    lua_setglobal(L, "log");
    lua_pushcfunction(L, Sleep);
    lua_setglobal(L, "sleep");
    lua_pushcfunction(L, GetTime);
    lua_setglobal(L, "time");

    // 注册 ImGui API
    LuaAPI_ImGui::Register(L);

    // 注册汇编 API
    LuaAPI_Assembly::Register(L);
}

// ==================== 内存操作API实现 ====================
// 已移至 LuaAPI_Memory.cpp

// ==================== 进程和模块API实现 ====================
int LuaAPI::GetProcessList(lua_State* L) {
    auto& service = GetMemService(L);
    const Mem::OperationContext context = GetOperationContext(L, false);
    std::vector<Mem::ProcessInfo> processes;
    size_t offset = 0;
    while (true) {
        Mem::ProcessListRequest request;
        request.offset = offset;
        request.limit = Mem::kMaxProcessPageSize;
        auto response = service.listProcesses(context, request);
        if (!response.ok()) {
            PushError(L, response.error().message);
            return 2;
        }
        const auto& page = response.value();
        processes.insert(processes.end(), page.items.begin(), page.items.end());
        if (!page.nextOffset) break;
        offset = *page.nextOffset;
    }
    lua_newtable(L);
    for (size_t i = 0; i < processes.size(); ++i) {
        lua_pushinteger(L, i + 1);
        lua_newtable(L);
        lua_pushinteger(L, processes[i].pid);
        lua_setfield(L, -2, "pid");
        lua_pushstring(L, processes[i].name.c_str());
        lua_setfield(L, -2, "name");
        lua_settable(L, -3);
    }
    return 1;
}

int LuaAPI::AttachProcess(lua_State* L) {
    lua_Integer rawPid = luaL_checkinteger(L, 1);
    if (rawPid <= 0 || rawPid > (std::numeric_limits<int>::max)()) {
        luaL_error(L, "PID out of range");
        return 0;
    }

    auto& service = GetMemService(L);
    Mem::OpenProcessRequest request;
    request.pid = static_cast<int>(rawPid);
    auto response = service.openProcess(
        GetOperationContext(L, true), request);
    if (response.ok()) {
        lua_pushboolean(L, 1);
        return 1;
    }
    PushError(L, response.error().message);
    return 2;
}

int LuaAPI::GetCurrentPid(lua_State* L) {
    const auto context = GetOperationContext(L, true);
    const int pid = context.target ? context.target->pid : 0;
    lua_pushinteger(L, pid);
    return 1;
}

int LuaAPI::GetModuleList(lua_State* L) {
    auto& service = GetMemService(L);
    const Mem::OperationContext context = GetOperationContext(L, true);
    std::vector<Mem::ModuleInfo> modules;
    size_t offset = 0;
    while (true) {
        Mem::ModuleListRequest request;
        request.offset = offset;
        request.limit = Mem::kMaxModulePageSize;
        auto response = service.listModules(context, request);
        if (!response.ok()) {
            PushError(L, response.error().message);
            return 2;
        }
        const auto& page = response.value();
        modules.insert(modules.end(), page.items.begin(), page.items.end());
        if (!page.nextOffset) break;
        offset = *page.nextOffset;
    }
    lua_newtable(L);
    for (size_t i = 0; i < modules.size(); ++i) {
        lua_pushinteger(L, i + 1);
        lua_newtable(L);
        lua_pushstring(L, modules[i].name.c_str());
        lua_setfield(L, -2, "name");
        lua_pushnumber(L, static_cast<lua_Number>(modules[i].base));
        lua_setfield(L, -2, "base");
        lua_pushnumber(L, static_cast<lua_Number>(modules[i].size));
        lua_setfield(L, -2, "size");
        lua_settable(L, -3);
    }
    return 1;
}

int LuaAPI::GetModuleBase(lua_State* L) {
    const char* moduleName = luaL_checkstring(L, 1);
    auto& service = GetMemService(L);
    Mem::ModuleResolveRequest request;
    request.name = moduleName;
    auto response = service.resolveModule(
        GetOperationContext(L, true), request);
    if (response.ok()) {
        lua_pushnumber(L, static_cast<lua_Number>(
            response.value().module.base));
        return 1;
    }
    PushError(L, response.error().message);
    return 2;
}

int LuaAPI::ResolveOffsetChain(lua_State* L) {
    const char* moduleName = luaL_checkstring(L, 1);
    if (!lua_istable(L, 2)) {
        luaL_error(L, "Expected table for offsets");
    }

    const size_t len = lua_objlen(L, 2);
    if (len > kMaxLuaOffsetCount) {
        luaL_error(L, "Offset chain is too long");
        return 0;
    }

    std::vector<uint64_t> offsets;
    offsets.reserve(len);
    for (size_t i = 0; i < len; ++i) {
        lua_rawgeti(L, 2, static_cast<int>(i + 1));
        offsets.push_back(LuaAPI::CheckAddress(L, -1));
        lua_pop(L, 1);
    }

    auto& service = GetMemService(L);
    Mem::PointerResolveRequest request;
    request.moduleName = moduleName;
    request.offsets = std::move(offsets);
    request.dereferenceFinal = true;
    auto response = service.resolvePointer(
        GetOperationContext(L, true), request);
    if (response.ok()) {
        lua_pushnumber(L, static_cast<lua_Number>(response.value().address));
        return 1;
    }
    PushError(L, response.error().message);
    return 2;
}

// ==================== 扫描API实现 ====================
int LuaAPI::ScanFirst(lua_State* L) {
    // 简化实现，实际需要更复杂的参数处理
    luaL_error(L, "Not fully implemented yet");
    return 0;
}

int LuaAPI::ScanNext(lua_State* L) {
    luaL_error(L, "Not fully implemented yet");
    return 0;
}

int LuaAPI::GetScanResults(lua_State* L) {
    int offset = static_cast<int>(luaL_optinteger(L, 1, 0));
    int count = static_cast<int>(luaL_optinteger(L, 2, 1000));
    
    std::vector<uint64_t> results;
    // 这里需要调用实际的扫描结果获取函数
    // 暂时返回空表
    lua_newtable(L);
    return 1;
}

int LuaAPI::ClearScanResults(lua_State* L) {
    auto& service = GetMemService(L);
    auto response = service.clearScan(
        GetOperationContext(L, true), Mem::ScanClearRequest{});
    if (!response.ok()) {
        PushError(L, response.error().message);
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

int LuaAPI::ScanFuzzy(lua_State* L) {
    luaL_error(L, "Not fully implemented yet");
    return 0;
}

// ==================== 断点API实现 ====================
int LuaAPI::SetBreakpoint(lua_State* L) {
    uint64_t address = LuaAPI::CheckAddress(L, 1);
    const char* bpType = luaL_checkstring(L, 2);
    uint32_t bpSize = checkBreakpointSize(L, 3, 4);

    uint32_t typeFlag = 0;
    if (strcmp(bpType, "execute") == 0) typeFlag = 4;
    else if (strcmp(bpType, "write") == 0) typeFlag = 2;
    else if (strcmp(bpType, "read") == 0) typeFlag = 1;
    else if (strcmp(bpType, "access") == 0 || strcmp(bpType, "readwrite") == 0) typeFlag = 3;
    else {
        LuaAPI::PushError(L, "Invalid breakpoint type");
        return 2;
    }

    if (typeFlag == 4) bpSize = 4;

    Mem::BreakpointSetRequest request;
    request.address = address;
    request.access = static_cast<Mem::BreakpointAccess>(typeFlag);
    request.size = bpSize;
    auto& service = GetMemService(L);
    auto response = service.setBreakpoint(
        GetOperationContext(L, true), request);
    if (!response.ok()) {
        PushError(L, response.error().message);
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

int LuaAPI::RemoveBreakpoint(lua_State* L) {
    uint64_t address = LuaAPI::CheckAddress(L, 1);
    Mem::BreakpointAddressRequest request{address};
    auto& service = GetMemService(L);
    auto response = service.removeBreakpoint(
        GetOperationContext(L, true), request);
    if (!response.ok()) {
        PushError(L, response.error().message);
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

int LuaAPI::SuspendBreakpoint(lua_State* L) {
    uint64_t address = LuaAPI::CheckAddress(L, 1);
    Mem::BreakpointAddressRequest request{address};
    auto& service = GetMemService(L);
    auto response = service.suspendBreakpoint(
        GetOperationContext(L, true), request);
    if (!response.ok()) {
        PushError(L, response.error().message);
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

int LuaAPI::ResumeBreakpoint(lua_State* L) {
    uint64_t address = LuaAPI::CheckAddress(L, 1);
    Mem::BreakpointAddressRequest request{address};
    auto& service = GetMemService(L);
    auto response = service.resumeBreakpoint(
        GetOperationContext(L, true), request);
    if (!response.ok()) {
        PushError(L, response.error().message);
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

int LuaAPI::GetBreakpointInfo(lua_State* L) {
    uint64_t address = LuaAPI::CheckAddress(L, 1);
    auto& service = GetMemService(L);
    Mem::BreakpointHitBatchRequest request;
    request.address = address;
    request.limit = Mem::kMaxBreakpointHitBatchSize;
    auto response = service.breakpointHitBatch(
        GetOperationContext(L, true), request);
    if (!response.ok()) {
        PushError(L, response.error().message);
        return 2;
    }
    lua_newtable(L);
    const auto& hits = response.value().items;
    for (size_t i = 0; i < hits.size(); ++i) {
        lua_pushinteger(L, i + 1);
        lua_newtable(L);
        lua_pushnumber(L, static_cast<lua_Number>(hits[i].hitAddress));
        lua_setfield(L, -2, "addr");
        lua_pushnumber(L, static_cast<lua_Number>(hits[i].hitTime));
        lua_setfield(L, -2, "time");
        lua_settable(L, -3);
    }
    return 1;
}

// ==================== 工具API实现 ====================
int LuaAPI::Log(lua_State* L) {
    int n = lua_gettop(L);
    std::string msg;
    for (int i = 1; i <= n; ++i) {
        if (i > 1) msg += " ";
        if (lua_isstring(L, i)) {
            msg += lua_tostring(L, i);
        } else {
            size_t len;
            const char* str = luaL_tolstring_compat(L, i, &len);
            msg += str;
            lua_pop(L, 1);  // 弹出转换后的字符串
        }
    }
    Gui::log("%s", msg.c_str());
    return 0;
}

int LuaAPI::Sleep(lua_State* L) {
    lua_Integer rawMilliseconds = luaL_checkinteger(L, 1);
    if (rawMilliseconds < 0 || rawMilliseconds > (std::numeric_limits<int>::max)()) {
        luaL_error(L, "sleep duration must be non-negative");
        return 0;
    }
    int milliseconds = static_cast<int>(rawMilliseconds);
    if (!SocketIoTimeout::HasThreadTimeout()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
        return 0;
    }

    int remainingSleepMs = milliseconds;
    while (remainingSleepMs > 0) {
        if (SocketIoTimeout::IsThreadTimeoutExpired()) {
            luaL_error(L, "Lua execution timed out");
            return 0;
        }

        const DWORD remainingTimeoutMs = SocketIoTimeout::GetRemainingTimeoutMs();
        const int sliceMs = (std::min)(
            remainingSleepMs,
            (std::min)(kLuaSleepPollMs, static_cast<int>(remainingTimeoutMs)));
        std::this_thread::sleep_for(std::chrono::milliseconds(sliceMs));
        remainingSleepMs -= sliceMs;
    }

    if (SocketIoTimeout::IsThreadTimeoutExpired()) {
        luaL_error(L, "Lua execution timed out");
    }
    return 0;
}

int LuaAPI::GetTime(lua_State* L) {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    lua_pushnumber(L, static_cast<lua_Number>(time));
    return 1;
}

// ==================== ImGui API实现 ====================
// 已移至 LuaAPI_ImGui.cpp
