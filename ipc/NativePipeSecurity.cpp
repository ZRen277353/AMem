#include "NativePipeSecurity.h"

#include <sddl.h>

#include <vector>

namespace NativeIpc {
namespace {

bool currentUserSidString(std::wstring& sid, std::wstring& error) {
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
        error = L"OpenProcessToken failed: " +
                FormatWindowsError(::GetLastError());
        return false;
    }

    DWORD required = 0;
    ::GetTokenInformation(token, TokenUser, nullptr, 0, &required);
    const DWORD sizeError = ::GetLastError();
    if (required == 0 || sizeError != ERROR_INSUFFICIENT_BUFFER) {
        ::CloseHandle(token);
        error = L"GetTokenInformation(size) failed: " +
                FormatWindowsError(sizeError);
        return false;
    }

    std::vector<unsigned char> buffer(required);
    if (!::GetTokenInformation(token, TokenUser, buffer.data(), required,
                               &required)) {
        const DWORD infoError = ::GetLastError();
        ::CloseHandle(token);
        error = L"GetTokenInformation failed: " +
                FormatWindowsError(infoError);
        return false;
    }
    ::CloseHandle(token);

    const auto* tokenUser =
        reinterpret_cast<const TOKEN_USER*>(buffer.data());
    LPWSTR sidText = nullptr;
    if (!::ConvertSidToStringSidW(tokenUser->User.Sid, &sidText)) {
        error = L"ConvertSidToStringSidW failed: " +
                FormatWindowsError(::GetLastError());
        return false;
    }

    sid.assign(sidText);
    ::LocalFree(sidText);
    return true;
}

} // namespace

std::wstring FormatWindowsError(DWORD errorCode) {
    LPWSTR message = nullptr;
    const DWORD length = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, errorCode, 0, reinterpret_cast<LPWSTR>(&message), 0,
        nullptr);
    if (length == 0 || message == nullptr) {
        return L"Windows error " + std::to_wstring(errorCode);
    }

    std::wstring result(message, length);
    ::LocalFree(message);
    while (!result.empty() &&
           (result.back() == L'\r' || result.back() == L'\n' ||
            result.back() == L' ')) {
        result.pop_back();
    }
    return result;
}

NativePipeSecurity::~NativePipeSecurity() {
    reset();
}

bool NativePipeSecurity::initialize(std::wstring& error) {
    reset();
    error.clear();

    std::wstring userSid;
    if (!currentUserSidString(userSid, error)) {
        return false;
    }

    const std::wstring sddl =
        L"D:P(A;;GRGW;;;SY)(A;;GRGW;;;" + userSid + L")";
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(), SDDL_REVISION_1, &descriptor_, nullptr)) {
        error = L"ConvertStringSecurityDescriptorToSecurityDescriptorW "
                L"failed: " +
                FormatWindowsError(::GetLastError());
        descriptor_ = nullptr;
        return false;
    }

    attributes_.nLength = sizeof(attributes_);
    attributes_.lpSecurityDescriptor = descriptor_;
    attributes_.bInheritHandle = FALSE;
    return true;
}

void NativePipeSecurity::reset() noexcept {
    if (descriptor_ != nullptr) {
        ::LocalFree(descriptor_);
        descriptor_ = nullptr;
    }
    attributes_.lpSecurityDescriptor = nullptr;
}

} // namespace NativeIpc
