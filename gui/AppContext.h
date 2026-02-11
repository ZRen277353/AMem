#pragma once

#include "../socket/client_singleton.h"
#include <atomic>
#include <string>
#include <vector>
#include <mutex>
#include <cstdint>

class AppContext {
public:
    static AppContext& Get() {
        static AppContext instance;
        return instance;
    }

    // 进程状态
    std::atomic<int> selectedPid{0};
    std::atomic<int> processHandle{0};
    std::string selectedName;

    void selectProcess(int pid, const std::string& name);
    bool hasProcess() const { return selectedPid.load(std::memory_order_relaxed) != 0; }

    // 模块缓存
    struct ModuleCache {
        std::vector<ModuleInfoItem> modules;
        bool valid = false;
        double lastRefreshTime = 0.0;
        std::mutex mutex;

        void refresh();
        const ModuleInfoItem* findByAddress(uint64_t addr);
        std::string formatWithModule(uint64_t addr);
        void invalidate() { valid = false; }
    } moduleCache;

private:
    AppContext() = default;
    AppContext(const AppContext&) = delete;
    AppContext& operator=(const AppContext&) = delete;
};
