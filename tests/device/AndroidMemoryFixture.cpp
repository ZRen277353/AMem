#include <cinttypes>
#include <csignal>
#include <cstdio>
#include <unistd.h>

namespace {

constexpr uint64_t kOriginalMarker = UINT64_C(0x1122334455667788);
constexpr uint32_t kOriginalScanMarker = UINT32_C(0x13572468);

alignas(64) volatile uint64_t g_guardBefore =
    UINT64_C(0xA1A2A3A4A5A6A7A8);
alignas(64) volatile uint64_t g_marker = kOriginalMarker;
alignas(64) volatile uint32_t g_scanMarker = kOriginalScanMarker;
alignas(64) volatile uint64_t g_guardAfter =
    UINT64_C(0xB1B2B3B4B5B6B7B8);

volatile sig_atomic_t g_running = 1;
volatile sig_atomic_t g_dumpRequested = 1;

void handleSignal(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        g_running = 0;
    } else if (signal == SIGUSR1) {
        g_dumpRequested = 1;
    }
}

uint64_t addressOf(const volatile void* value) {
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(value));
}

void printState(const char* event) {
    std::printf(
        "event=%s pid=%d marker_address=0x%016" PRIX64
        " marker=0x%016" PRIX64
        " scan_address=0x%016" PRIX64
        " scan=0x%08" PRIX32
        " guard_before=0x%016" PRIX64
        " guard_after=0x%016" PRIX64 "\n",
        event,
        static_cast<int>(::getpid()),
        addressOf(&g_marker),
        static_cast<uint64_t>(g_marker),
        addressOf(&g_scanMarker),
        static_cast<uint32_t>(g_scanMarker),
        static_cast<uint64_t>(g_guardBefore),
        static_cast<uint64_t>(g_guardAfter));
    std::fflush(stdout);
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);
    std::signal(SIGUSR1, handleSignal);

    printState("started");
    while (g_running != 0) {
        const uint64_t observedMarker = g_marker;
        const uint32_t observedScanMarker = g_scanMarker;
        (void)observedMarker;
        (void)observedScanMarker;

        if (g_dumpRequested != 0) {
            g_dumpRequested = 0;
            printState("snapshot");
        }
        ::sleep(1);
    }

    printState("stopped");
    return (g_guardBefore == UINT64_C(0xA1A2A3A4A5A6A7A8) &&
            g_guardAfter == UINT64_C(0xB1B2B3B4B5B6B7B8))
               ? 0
               : 2;
}
