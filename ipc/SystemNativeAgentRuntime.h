#pragma once

namespace NativeIpc {

class NativeAgentRuntime;

// Lazily constructs the product runtime only when the explicit GUI control is
// opened. Shutdown does not create it if native IPC was never used.
NativeAgentRuntime &GetSystemNativeAgentRuntime();
void ShutdownSystemNativeAgentRuntime();

} // namespace NativeIpc
