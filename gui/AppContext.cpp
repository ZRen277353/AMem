#include "AppContext.h"
#include "Gui.h"
#include "../imgui/imgui.h"
#include <algorithm>
#include <sstream>
#include <iomanip>

void AppContext::selectProcess(int pid, const std::string& name) {
    selectedPid.store(pid, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(nameMutex_);
        selectedName_ = name;
    }

    SetCurrentPid(pid);
    int handle = 0;
    if (OpenProcessHandle(pid, handle)) {
        processHandle.store(handle, std::memory_order_relaxed);
        Gui::log("进程已打开，句柄=%d", handle);
    } else {
        processHandle.store(0, std::memory_order_relaxed);
        Gui::log("无法打开进程句柄 %d", pid);
    }

    moduleCache.invalidate();
}

void AppContext::ModuleCache::refresh() {
    std::lock_guard<std::mutex> lock(mutex);

    // 节流：如果缓存有效且距上次刷新不足 MIN_REFRESH_INTERVAL 秒，跳过
    double now = ImGui::GetTime();
    if (valid && (now - lastRefreshTime) < MIN_REFRESH_INTERVAL) {
        return;
    }

    std::vector<ModuleInfoItem> newList;
    if (FetchModuleList(newList)) {
        modules = std::move(newList);
        valid = true;
        lastRefreshTime = now;
    }
}

ModuleInfoItem AppContext::ModuleCache::findByAddress(uint64_t addr) {
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& m : modules) {
        if (addr >= m.base && addr < m.base + static_cast<uint64_t>(m.size)) {
            return m;  // 返回拷贝
        }
    }
    return ModuleInfoItem{};  // 空对象，name 为空表示未找到
}

std::string AppContext::ModuleCache::formatWithModule(uint64_t addr) {
    ModuleInfoItem mod = findByAddress(addr);
    if (!mod.name.empty()) {
        char buf[256];
        uint64_t offset = addr - mod.base;
        snprintf(buf, sizeof(buf), "%s+0x%llX", mod.name.c_str(), (unsigned long long)offset);
        return buf;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "0x%llX", (unsigned long long)addr);
    return buf;
}
