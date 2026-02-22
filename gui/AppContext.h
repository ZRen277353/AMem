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

    void selectProcess(int pid, const std::string& name);
    bool hasProcess() const { return selectedPid.load(std::memory_order_relaxed) != 0; }

    // 线程安全的 selectedName 访问
    std::string getSelectedName() const {
        std::lock_guard<std::mutex> lock(nameMutex_);
        return selectedName_;
    }

    // 兼容旧代码的只读引用（仅限主线程使用）
    const std::string& selectedName = selectedName_;

    // 模块缓存
    struct ModuleCache {
        std::vector<ModuleInfoItem> modules;
        bool valid = false;
        double lastRefreshTime = 0.0;
        std::mutex mutex;
        static constexpr double MIN_REFRESH_INTERVAL = 1.0;  // 最小刷新间隔（秒）

        void refresh();
        ModuleInfoItem findByAddress(uint64_t addr);  // 返回值拷贝，避免悬空指针
        std::string formatWithModule(uint64_t addr);
        void invalidate() {
            std::lock_guard<std::mutex> lock(mutex);
            valid = false;
        }
    } moduleCache;

private:
    AppContext() = default;
    AppContext(const AppContext&) = delete;
    AppContext& operator=(const AppContext&) = delete;

    mutable std::mutex nameMutex_;
    std::string selectedName_;
};
