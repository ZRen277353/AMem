#pragma once

#include <windows.h>

#include <string>

namespace NativeIpc {

std::wstring FormatWindowsError(DWORD errorCode);

class NativePipeSecurity final {
public:
    NativePipeSecurity() = default;
    ~NativePipeSecurity();

    NativePipeSecurity(const NativePipeSecurity&) = delete;
    NativePipeSecurity& operator=(const NativePipeSecurity&) = delete;

    bool initialize(std::wstring& error);
    void reset() noexcept;

    SECURITY_ATTRIBUTES* attributes() noexcept { return &attributes_; }
    PSECURITY_DESCRIPTOR descriptor() const noexcept { return descriptor_; }

private:
    PSECURITY_DESCRIPTOR descriptor_ = nullptr;
    SECURITY_ATTRIBUTES attributes_{sizeof(SECURITY_ATTRIBUTES), nullptr,
                                    FALSE};
};

} // namespace NativeIpc
