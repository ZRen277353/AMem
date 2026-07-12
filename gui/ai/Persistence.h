#pragma once
#ifdef HAVE_AI_CHAT

#include "../../third_party/nlohmann/json.hpp"

#include <filesystem>
#include <string>

namespace AI {

enum class PersistenceLoadStatus {
    Loaded,
    Missing,
    Recovered,
    Invalid,
    IoError,
};

struct PersistenceLoadResult {
    PersistenceLoadStatus status = PersistenceLoadStatus::Missing;
    std::string message;

    bool usable() const {
        return status == PersistenceLoadStatus::Loaded ||
               status == PersistenceLoadStatus::Missing ||
               status == PersistenceLoadStatus::Recovered;
    }

    bool failed() const {
        return status == PersistenceLoadStatus::Invalid ||
               status == PersistenceLoadStatus::IoError;
    }
};

const char* persistenceLoadStatusName(PersistenceLoadStatus status);

struct JsonDocumentLoadResult {
    PersistenceLoadResult result;
    nlohmann::json document;
};

JsonDocumentLoadResult loadJsonDocument(
    const std::filesystem::path& filepath);

bool preserveInvalidFile(const std::filesystem::path& filepath,
                         std::filesystem::path& backupPath,
                         std::string& error);

} // namespace AI

#endif // HAVE_AI_CHAT
