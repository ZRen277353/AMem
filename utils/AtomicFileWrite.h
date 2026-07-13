#pragma once

#include <filesystem>
#include <system_error>

namespace utils {

namespace detail {

struct FilesystemAtomicFileOps {
    bool rename(const std::filesystem::path& from,
                const std::filesystem::path& to) {
        std::error_code ec;
        std::filesystem::rename(from, to, ec);
        return !ec;
    }

    bool exists(const std::filesystem::path& path) {
        std::error_code ec;
        const bool present = std::filesystem::exists(path, ec);
        return !ec && present;
    }

    void remove(const std::filesystem::path& path) {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
};

template <typename Operations>
bool installTempFileWithOps(const std::filesystem::path& tmpPath,
                            const std::filesystem::path& targetPath,
                            Operations& operations) {
    if (operations.rename(tmpPath, targetPath)) {
        return true;
    }

    std::filesystem::path bakPath = targetPath;
    bakPath += ".bak";
    operations.remove(bakPath);

    bool movedAside = false;
    if (operations.exists(targetPath)) {
        movedAside = operations.rename(targetPath, bakPath);
    }

    if (!operations.rename(tmpPath, targetPath)) {
        if (movedAside) {
            (void)operations.rename(bakPath, targetPath);
        }
        operations.remove(tmpPath);
        return false;
    }

    if (movedAside) {
        operations.remove(bakPath);
    }
    return true;
}

} // namespace detail

// Install a freshly-written temp file over `targetPath` as atomically as the
// platform allows, WITHOUT ever leaving the caller with neither file.
//
// Fast path: a direct rename (atomic on NTFS when both paths sit on the same
// volume, which is guaranteed when the temp is a sibling of the target). If
// that fails — e.g. the target is held open by another process (AV scanner,
// sync client, a second app instance) — the existing target is first moved
// aside to a `.bak` sibling, the temp is installed, and on any failure the
// original is restored from the backup. The temp is removed on failure so a
// stale `.tmp` is never left behind.
//
// This replaces the earlier "remove(target) then rename(tmp, target)" pattern,
// which could delete the only good copy if the second rename also failed
// (losing e.g. all encrypted API keys or the whole session index).
//
// Returns true only when `targetPath` now holds the new content.
inline bool installTempFile(const std::filesystem::path& tmpPath,
                            const std::filesystem::path& targetPath) {
    detail::FilesystemAtomicFileOps operations;
    return detail::installTempFileWithOps(tmpPath, targetPath, operations);
}

} // namespace utils
