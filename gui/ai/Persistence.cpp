#ifdef HAVE_AI_CHAT

#include "Persistence.h"

#include "AiLimits.h"
#include "../../utils/BoundedJson.h"

#include <array>
#include <fstream>
#include <system_error>
#include <utility>

namespace AI {

namespace {

const utils::JsonComplexityLimits& persistenceJsonLimits() {
    static const utils::JsonComplexityLimits limits = {
        Limits::kMaxPersistenceFileBytes,
        Limits::kMaxPersistenceJsonDepth,
        Limits::kMaxPersistenceJsonNodes,
        Limits::kMaxPersistenceContainerItems,
        Limits::kMaxPersistenceStringBytes,
        Limits::kMaxPersistenceTotalStringBytes,
    };
    return limits;
}

} // namespace

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

bool validateSerializedJsonComplexity(std::string_view serialized,
                                      std::string& error) {
    return utils::validateJsonComplexity(
        serialized, persistenceJsonLimits(), error);
}

bool parseBoundedJson(std::string_view serialized,
                      nlohmann::json& document,
                      std::string& error) {
    return utils::parseBoundedJson(
        serialized, persistenceJsonLimits(), document, error);
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

    ec.clear();
    const uintmax_t fileBytes = std::filesystem::file_size(filepath, ec);
    if (ec) {
        loaded.result.status = PersistenceLoadStatus::IoError;
        loaded.result.message = "could not inspect file size: " + ec.message();
        return loaded;
    }
    if (fileBytes > Limits::kMaxPersistenceFileBytes) {
        loaded.result.status = PersistenceLoadStatus::Invalid;
        loaded.result.message = "JSON file exceeds 32 MiB limit";
        return loaded;
    }

    std::ifstream input(filepath, std::ios::binary);
    if (!input.is_open()) {
        loaded.result.status = PersistenceLoadStatus::IoError;
        loaded.result.message = "could not open file for reading";
        return loaded;
    }

    std::string serialized;
    serialized.reserve(static_cast<size_t>(fileBytes));
    std::array<char, 64u * 1024u> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        if (count <= 0) {
            break;
        }
        const size_t bytes = static_cast<size_t>(count);
        if (Limits::wouldExceed(serialized.size(), bytes,
                                Limits::kMaxPersistenceFileBytes)) {
            loaded.result.status = PersistenceLoadStatus::Invalid;
            loaded.result.message = "JSON file exceeds 32 MiB limit";
            return loaded;
        }
        serialized.append(buffer.data(), bytes);
    }
    if (input.bad()) {
        loaded.result.status = PersistenceLoadStatus::IoError;
        loaded.result.message = "I/O failure while reading file";
        return loaded;
    }

    std::string parseError;
    if (!parseBoundedJson(serialized, loaded.document, parseError)) {
        loaded.result.status = PersistenceLoadStatus::Invalid;
        loaded.result.message = std::move(parseError);
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
