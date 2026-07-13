#pragma once
#ifdef HAVE_AI_CHAT

#include "Persistence.h"

#include "../../third_party/nlohmann/json.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace AI {

enum class ProtectedPersistenceKind : uint8_t {
    Session = 1,
    SessionIndex = 2,
};

struct ProtectedJsonDocumentLoadResult {
    PersistenceLoadResult result;
    nlohmann::json document;
    bool legacyPlaintext = false;
};

// Session persistence is a binary DPAPI envelope, even though legacy file
// names retain their .json extension. Bounded legacy plaintext is parsed into
// temporary state and marked for migration; the owning schema validator must
// call saveProtectedJsonDocument only after full validation succeeds.
ProtectedJsonDocumentLoadResult loadProtectedJsonDocument(
    const std::filesystem::path& filepath,
    ProtectedPersistenceKind kind);

bool saveProtectedJsonDocument(
    const std::filesystem::path& filepath,
    ProtectedPersistenceKind kind,
    const nlohmann::json& document,
    std::string& error);

bool isProtectedPersistenceFile(
    const std::filesystem::path& filepath,
    ProtectedPersistenceKind kind);

} // namespace AI

#endif // HAVE_AI_CHAT
