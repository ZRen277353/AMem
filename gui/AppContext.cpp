#include "AppContext.h"
#include "../mem/IMemService.h"
#include "../imgui/imgui.h"
#include <algorithm>

AppContext::TargetMutation::TargetMutation(
    AppContext& owner,
    std::unique_lock<std::mutex> stateLock,
    Mem::TargetSnapshot previousTarget)
    : owner_(&owner),
      stateLock_(std::move(stateLock)),
      previousTarget_(previousTarget) {}

AppContext::TargetMutation::TargetMutation(TargetMutation&& other) noexcept
    : owner_(other.owner_),
      stateLock_(std::move(other.stateLock_)),
      previousTarget_(other.previousTarget_) {
    other.owner_ = nullptr;
}

AppContext::TargetMutation::~TargetMutation() {
    finish();
}

void AppContext::TargetMutation::publish(
    int pid, int handle, const std::string& name) {
    if (!owner_) {
        return;
    }
    owner_->selectedPid.store(pid, std::memory_order_relaxed);
    owner_->processHandle.store(handle, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(owner_->nameMutex_);
        owner_->selectedName_ = name;
    }
    owner_->moduleCache.invalidate();
}

void AppContext::TargetMutation::clear() {
    publish(0, 0, {});
}

void AppContext::TargetMutation::finish() {
    if (!owner_) {
        return;
    }
    owner_->processRevision.fetch_add(1, std::memory_order_release);
    owner_ = nullptr;
}

std::optional<AppContext::TargetMutation> AppContext::beginTargetMutation(
    const std::optional<Mem::TargetSnapshot>& expected,
    uint64_t connectionGeneration) {
    std::unique_lock<std::mutex> stateLock(processStateMutex_);
    const uint64_t revision =
        processRevision.load(std::memory_order_acquire);
    if ((revision & 1u) != 0u) {
        return std::nullopt;
    }

    Mem::TargetSnapshot current;
    current.pid = selectedPid.load(std::memory_order_relaxed);
    current.processHandle = processHandle.load(std::memory_order_relaxed);
    current.processRevision = revision;
    current.connectionGeneration = connectionGeneration;
    if (expected && current != *expected) {
        return std::nullopt;
    }

    processRevision.fetch_add(1, std::memory_order_acq_rel);
    return TargetMutation(*this, std::move(stateLock), current);
}

void AppContext::clearProcessForDisconnect() {
    auto mutation = beginTargetMutation(std::nullopt, 0);
    if (mutation) {
        mutation->clear();
    }
}

Mem::TargetSnapshot AppContext::snapshotTarget(
    uint64_t connectionGeneration) const {
    std::lock_guard<std::mutex> stateLock(processStateMutex_);
    Mem::TargetSnapshot snapshot;
    snapshot.pid = selectedPid.load(std::memory_order_relaxed);
    snapshot.processHandle = processHandle.load(std::memory_order_relaxed);
    snapshot.processRevision = processRevision.load(std::memory_order_acquire);
    snapshot.connectionGeneration = connectionGeneration;
    return snapshot;
}

bool AppContext::matchesStableTarget(
    const Mem::TargetSnapshot& expected,
    uint64_t connectionGeneration) const {
    if (expected.connectionGeneration != connectionGeneration) {
        return false;
    }

    const uint64_t revisionBefore =
        processRevision.load(std::memory_order_acquire);
    if ((revisionBefore & 1u) != 0u ||
        revisionBefore != expected.processRevision) {
        return false;
    }
    const int pid = selectedPid.load(std::memory_order_relaxed);
    const int handle = processHandle.load(std::memory_order_relaxed);
    const uint64_t revisionAfter =
        processRevision.load(std::memory_order_acquire);
    return revisionBefore == revisionAfter &&
           (revisionAfter & 1u) == 0u &&
           pid == expected.pid &&
           handle == expected.processHandle;
}

void AppContext::ModuleCache::refresh(Mem::IMemService& service) {
    const double now = ImGui::GetTime();
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (valid && (now - lastRefreshTime) < MIN_REFRESH_INTERVAL) {
            return;
        }
    }

    const Mem::OperationContext context = service.captureContext(true);
    if (!context.target || !context.target->isAttached()) {
        return;
    }

    std::vector<Mem::ModuleInfo> newList;
    size_t offset = 0;
    while (true) {
        Mem::ModuleListRequest request;
        request.offset = offset;
        request.limit = Mem::kMaxModulePageSize;
        auto response = service.listModules(context, request);
        if (!response.ok() || response.value().target != *context.target) {
            return;
        }
        const auto& page = response.value();
        newList.insert(newList.end(), page.items.begin(), page.items.end());
        if (!page.nextOffset) {
            break;
        }
        offset = *page.nextOffset;
    }

    const Mem::OperationContext current = service.captureContext(true);
    if (!current.target || *current.target != *context.target) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex);
    modules = std::move(newList);
    symbolCacheByModuleBase.clear();
    valid = true;
    lastRefreshTime = now;
}

Mem::ModuleInfo AppContext::ModuleCache::findByAddress(uint64_t addr) {
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& m : modules) {
        if (m.size <= 0) {
            continue;
        }
        const uint64_t moduleSize = static_cast<uint64_t>(m.size);
        if (m.base > UINT64_MAX - moduleSize) {
            continue;
        }
        if (addr >= m.base && addr < m.base + moduleSize) {
            return m;  // 返回拷贝
        }
    }
    return Mem::ModuleInfo{};  // 空对象，name 为空表示未找到
}

std::string AppContext::ModuleCache::formatWithModule(uint64_t addr) {
    Mem::ModuleInfo mod = findByAddress(addr);
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

bool AppContext::ModuleCache::ensureSymbolListCached(
    const Mem::ModuleInfo& module,
    Mem::IMemService& service,
    std::vector<SymbolInfoItem>& outSymbols) {
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = symbolCacheByModuleBase.find(module.base);
        if (it != symbolCacheByModuleBase.end() && it->second.valid) {
            outSymbols = it->second.symbols;
            return true;
        }
    }

    Mem::SymbolTableRequest request;
    request.moduleName = module.name;
    auto response = service.loadSymbolTable(
        service.captureContext(true), request);
    if (!response.ok() || response.value().items.empty() ||
        response.value().session.module.base != module.base) {
        return false;
    }

    const Mem::TargetSnapshot expectedTarget = response.value().session.target;
    std::vector<SymbolInfoItem> loadedSymbols;
    loadedSymbols.reserve(response.value().items.size());
    for (auto& symbol : response.value().items) {
        if (!symbol.name.empty()) {
            loadedSymbols.push_back(
                SymbolInfoItem{symbol.address, std::move(symbol.name)});
        }
    }

    std::sort(loadedSymbols.begin(), loadedSymbols.end(), [](const SymbolInfoItem& lhs, const SymbolInfoItem& rhs) {
        return lhs.address < rhs.address;
    });

    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!AppContext::Get().matchesStableTarget(
                expectedTarget, expectedTarget.connectionGeneration)) {
            return false;
        }
        const auto currentModule = std::find_if(
            modules.begin(), modules.end(),
            [&](const Mem::ModuleInfo& item) {
                return item.base == module.base && item.name == module.name;
            });
        if (currentModule == modules.end()) {
            return false;
        }
        auto& cacheEntry = symbolCacheByModuleBase[module.base];
        cacheEntry.symbols = std::move(loadedSymbols);
        cacheEntry.lastMatchedIndex = 0;
        cacheEntry.valid = true;
        outSymbols = cacheEntry.symbols;
    }
    return true;
}

bool AppContext::ModuleCache::tryFindContainingSymbol(
    uint64_t addr,
    Mem::IMemService& service,
    SymbolInfoItem& outSymbol,
    uint64_t& outOffset) {
    outSymbol = SymbolInfoItem{};
    outOffset = 0;

    Mem::ModuleInfo mod = findByAddress(addr);
    if (mod.name.empty() || mod.base == 0) {
        return false;
    }

    std::vector<SymbolInfoItem> symbols;
    if (!ensureSymbolListCached(mod, service, symbols) || symbols.empty()) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex);
        auto cacheIt = symbolCacheByModuleBase.find(mod.base);
        if (cacheIt != symbolCacheByModuleBase.end() && cacheIt->second.valid && !cacheIt->second.symbols.empty()) {
            const auto& cachedSymbols = cacheIt->second.symbols;
            if (cacheIt->second.lastMatchedIndex < cachedSymbols.size()) {
                const size_t idx = cacheIt->second.lastMatchedIndex;
                const uint64_t start = cachedSymbols[idx].address;
                const uint64_t end = (idx + 1 < cachedSymbols.size()) ? cachedSymbols[idx + 1].address : UINT64_MAX;
                if (addr >= start && addr < end) {
                    outSymbol.address = cachedSymbols[idx].address;
                    outSymbol.name = cachedSymbols[idx].name;
                    outOffset = addr - cachedSymbols[idx].address;
                    return true;
                }
            }
        }
    }

    auto it = std::upper_bound(symbols.begin(), symbols.end(), addr,
        [](uint64_t target, const SymbolInfoItem& symbol) {
            return target < symbol.address;
        });

    if (it == symbols.begin()) {
        return false;
    }

    --it;
    const size_t matchedIndex = static_cast<size_t>(std::distance(symbols.begin(), it));
    outSymbol.address = it->address;
    outSymbol.name = it->name;
    outOffset = addr - it->address;

    {
        std::lock_guard<std::mutex> lock(mutex);
        auto cacheIt = symbolCacheByModuleBase.find(mod.base);
        if (cacheIt != symbolCacheByModuleBase.end() && cacheIt->second.valid) {
            cacheIt->second.lastMatchedIndex = matchedIndex;
        }
    }
    return true;
}

std::string AppContext::ModuleCache::formatWithSymbol(
    uint64_t addr, Mem::IMemService& service) {
    SymbolInfoItem symbol;
    uint64_t symbolOffset = 0;
    if (!tryFindContainingSymbol(
            addr, service, symbol, symbolOffset)) {
        return "";
    }

    char buf[512];
    if (symbolOffset == 0) {
        snprintf(buf, sizeof(buf), "%s", symbol.name.c_str());
    } else {
        snprintf(buf, sizeof(buf), "%s+0x%llX", symbol.name.c_str(), (unsigned long long)symbolOffset);
    }
    return buf;
}

std::string AppContext::ModuleCache::formatAddressWithModuleAndSymbol(
    uint64_t addr, Mem::IMemService& service) {
    std::string moduleText = formatWithModule(addr);
    std::string symbolText = formatWithSymbol(addr, service);
    if (symbolText.empty()) {
        return moduleText;
    }
    return moduleText + " (" + symbolText + ")";
}
