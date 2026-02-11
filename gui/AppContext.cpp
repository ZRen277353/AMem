#include "AppContext.h"
#include "Gui.h"
#include "../imgui/imgui.h"
#include <algorithm>
#include <sstream>
#include <iomanip>

void AppContext::selectProcess(int pid, const std::string& name) {
    selectedPid.store(pid, std::memory_order_relaxed);
    selectedName = name;

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
    std::vector<ModuleInfoItem> newList;
    if (FetchModuleList(newList)) {
        modules = std::move(newList);
        valid = true;
        lastRefreshTime = ImGui::GetTime();
    }
}

const ModuleInfoItem* AppContext::ModuleCache::findByAddress(uint64_t addr) {
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& m : modules) {
        if (addr >= m.base && addr < m.base + static_cast<uint64_t>(m.size)) {
            return &m;
        }
    }
    return nullptr;
}

std::string AppContext::ModuleCache::formatWithModule(uint64_t addr) {
    const ModuleInfoItem* mod = findByAddress(addr);
    std::ostringstream oss;
    if (mod && !mod->name.empty()) {
        uint64_t offset = addr - mod->base;
        oss << mod->name << "+0x" << std::hex << std::uppercase << offset;
    } else {
        oss << "0x" << std::hex << std::uppercase << addr;
    }
    return oss.str();
}
