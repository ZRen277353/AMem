#include "../gui/AppContext.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace ImGui {
double GetTime() {
    return 0.0;
}
} // namespace ImGui

namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void testTargetMutationPublishesStableSnapshots() {
    auto& state = AppContext::Get();
    state.clearProcessForDisconnect();
    {
        std::lock_guard<std::mutex> lock(state.moduleCache.mutex);
        state.moduleCache.valid = true;
        state.moduleCache.modules = {
            Mem::ModuleInfo{0x1000, 0x2000, 1, 5, "libgame.so"},
        };
        AppContext::ModuleCache::SymbolListCacheEntry symbols;
        symbols.valid = true;
        symbols.symbols.push_back({0x1100, "GameInit"});
        state.moduleCache.symbolCacheByModuleBase.emplace(
            0x1000, std::move(symbols));
    }
    const auto initial = state.snapshotTarget(7);
    expect(!initial.isAttached() && (initial.processRevision & 1u) == 0u,
           "initial target should be detached and stable");

    {
        auto mutation = state.beginTargetMutation(initial, 7);
        expect(mutation.has_value(),
               "matching target should begin a mutation");
        expect((state.processRevision.load(std::memory_order_acquire) & 1u) != 0u,
               "active mutation should publish an odd revision");
        expect(!state.matchesStableTarget(initial, 7),
               "an active mutation must invalidate the old snapshot");
        mutation->publish(42, 420, "com.example.game");
    }

    const auto selected = state.snapshotTarget(7);
    expect(selected.isAttached() && selected.pid == 42 &&
               selected.processHandle == 420 &&
               selected.processRevision == initial.processRevision + 2 &&
               state.getSelectedName() == "com.example.game",
           "published target should become the next stable snapshot");
    expect(state.matchesStableTarget(selected, 7),
           "published snapshot should validate after mutation release");
    {
        std::lock_guard<std::mutex> lock(state.moduleCache.mutex);
        expect(!state.moduleCache.valid &&
                   state.moduleCache.symbolCacheByModuleBase.empty(),
               "target publication must invalidate presentation caches");
    }
}

void testStaleMutationIsRejectedWithoutChangingState() {
    auto& state = AppContext::Get();
    const auto current = state.snapshotTarget(11);
    auto stale = current;
    stale.processRevision += 2;

    auto rejected = state.beginTargetMutation(stale, 11);
    expect(!rejected.has_value(),
           "stale expected target must not begin a mutation");
    expect(state.snapshotTarget(11) == current,
           "rejected mutation must preserve the current target");
}

void testMoveAndDisconnectClearRemainStable() {
    auto& state = AppContext::Get();
    const auto current = state.snapshotTarget(13);
    {
        auto pending = state.beginTargetMutation(current, 13);
        expect(pending.has_value(), "current target should be mutable");
        AppContext::TargetMutation moved(std::move(*pending));
        moved.clear();
    }

    const auto cleared = state.snapshotTarget(13);
    expect(!cleared.isAttached() &&
               cleared.processRevision == current.processRevision + 2 &&
               state.getSelectedName().empty(),
           "moved mutation should clear target exactly once");

    state.clearProcessForDisconnect();
    const auto disconnected = state.snapshotTarget(14);
    expect(!disconnected.isAttached() &&
               (disconnected.processRevision & 1u) == 0u &&
               disconnected.processRevision == cleared.processRevision + 2,
           "disconnect publication should leave an even detached revision");
}

} // namespace

int main() {
    try {
        testTargetMutationPublishesStableSnapshots();
        testStaleMutationIsRejectedWithoutChangingState();
        testMoveAndDisconnectClearRemainStable();
        std::cout << "AppContext state tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "AppContext state tests failed: " << error.what() << '\n';
        return 1;
    }
}
