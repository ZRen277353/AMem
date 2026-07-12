#ifdef HAVE_AI_CHAT

#include "Persistence.h"

#include <fstream>
#include <system_error>

namespace AI {

const char* persistenceLoadStatusName(PersistenceLoadStatus status) {
    switch (status) {
        case PersistenceLoadStatus::Loaded:    return "loaded";
        case PersistenceLoadStatus::Missing:   return "missing";
        case PersistenceLoadStatus::Recovered: return "recovered";
        case PersistenceLoadStatus::Invalid:   return "invalid";
        case PersistenceLoadStatus::IoError:   return "io_error";
    }
    return "unknown";
}

JsonDocumentLoadResult loadJsonDocument(
    const std::filesystem::path& filepath) {
    JsonDocumentLoadResult loaded;

    std::error_code ec;
    const bool exists = std::filesystem::exists(filepath, ec);
    if (ec) {
        loaded.result.status = PersistenceLoadStatus::IoError;
        loaded.result.message = "could not inspect file: " + ec.message();
        return loaded;
    }
    if (!exists) {
        loaded.result.status = PersistenceLoadStatus::Missing;
        return loaded;
    }

    std::ifstream input(filepath, std::ios::binary);
    if (!input.is_open()) {
        loaded.result.status = PersistenceLoadStatus::IoError;
        loaded.result.message = "could not open file for reading";
        return loaded;
    }

    try {
        input >> loaded.document;
    } catch (const nlohmann::json::exception& error) {
        loaded.result.status = PersistenceLoadStatus::Invalid;
        loaded.result.message = std::string("invalid JSON: ") + error.what();
        return loaded;
    }
    if (input.bad()) {
        loaded.result.status = PersistenceLoadStatus::IoError;
        loaded.result.message = "I/O failure while reading file";
        return loaded;
    }

    loaded.result.status = PersistenceLoadStatus::Loaded;
    return loaded;
}

bool preserveInvalidFile(const std::filesystem::path& filepath,
                         std::filesystem::path& backupPath,
                         std::string& error) {
    error.clear();
    backupPath.clear();

    std::error_code ec;
    if (!std::filesystem::exists(filepath, ec)) {
        if (ec) {
            error = ec.message();
            return false;
        }
        return true;
    }

    for (unsigned index = 0; index < 1000; ++index) {
        std::filesystem::path candidate = filepath;
        candidate += index == 0
            ? std::string(".corrupt")
            : std::string(".corrupt.") + std::to_string(index);

        ec.clear();
        if (std::filesystem::exists(candidate, ec)) {
            if (ec) {
                error = ec.message();
                return false;
            }
            continue;
        }

        ec.clear();
        std::filesystem::copy_file(filepath, candidate,
                                   std::filesystem::copy_options::none, ec);
        if (!ec) {
            backupPath = std::move(candidate);
            return true;
        }
        error = ec.message();
        return false;
    }

    error = "too many existing corrupt-file backups";
    return false;
}

} // namespace AI

#endif // HAVE_AI_CHAT
