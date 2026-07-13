#ifdef HAVE_AI_CHAT

#include "ProtectedPersistence.h"

#include "AiLimits.h"
#include "SecureMemory.h"
#include "../../utils/AtomicFileWrite.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string_view>
#include <system_error>
#include <vector>

#pragma comment(lib, "crypt32.lib")

namespace AI {
namespace {

constexpr std::array<unsigned char, 8> kMagic = {
    'A', 'M', 'E', 'M', 'A', 'I', 'P', '1'};
constexpr uint8_t kEnvelopeVersion = 1;
constexpr size_t kHeaderBytes = 16;

std::string_view entropyFor(ProtectedPersistenceKind kind) {
    switch (kind) {
        case ProtectedPersistenceKind::Session:
            return "AMem.NativeAgent.Session.v1";
        case ProtectedPersistenceKind::SessionIndex:
            return "AMem.NativeAgent.SessionIndex.v1";
    }
    return {};
}

const wchar_t* descriptionFor(ProtectedPersistenceKind kind) {
    switch (kind) {
        case ProtectedPersistenceKind::Session:
            return L"AMem AI Session";
        case ProtectedPersistenceKind::SessionIndex:
            return L"AMem AI Session Index";
    }
    return L"AMem AI Protected Data";
}

void appendUint32Le(std::string& output, uint32_t value) {
    output.push_back(static_cast<char>(value & 0xffu));
    output.push_back(static_cast<char>((value >> 8u) & 0xffu));
    output.push_back(static_cast<char>((value >> 16u) & 0xffu));
    output.push_back(static_cast<char>((value >> 24u) & 0xffu));
}

uint32_t readUint32Le(const char* input) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(input);
    return static_cast<uint32_t>(bytes[0]) |
        (static_cast<uint32_t>(bytes[1]) << 8u) |
        (static_cast<uint32_t>(bytes[2]) << 16u) |
        (static_cast<uint32_t>(bytes[3]) << 24u);
}

bool hasMagic(std::string_view data) {
    if (data.size() < kMagic.size()) {
        return false;
    }
    for (size_t index = 0; index < kMagic.size(); ++index) {
        if (static_cast<unsigned char>(data[index]) != kMagic[index]) {
            return false;
        }
    }
    return true;
}

bool validHeader(std::string_view data,
                 ProtectedPersistenceKind kind,
                 uint32_t& ciphertextBytes,
                 std::string& error) {
    ciphertextBytes = 0;
    if (data.size() < kHeaderBytes || !hasMagic(data)) {
        error = "protected persistence header is truncated";
        return false;
    }
    if (static_cast<uint8_t>(data[8]) != kEnvelopeVersion) {
        error = "unsupported protected persistence version";
        return false;
    }
    if (static_cast<uint8_t>(data[9]) != static_cast<uint8_t>(kind)) {
        error = "protected persistence kind does not match the target file";
        return false;
    }
    if (data[10] != '\0' || data[11] != '\0') {
        error = "protected persistence reserved fields are non-zero";
        return false;
    }
    ciphertextBytes = readUint32Le(data.data() + 12);
    if (ciphertextBytes == 0 ||
        static_cast<size_t>(ciphertextBytes) != data.size() - kHeaderBytes) {
        error = "protected persistence payload length is invalid";
        return false;
    }
    return true;
}

bool protectPayload(std::string_view plaintext,
                    ProtectedPersistenceKind kind,
                    std::vector<uint8_t>& ciphertext,
                    std::string& error) {
    ciphertext.clear();
    if (plaintext.size() > Limits::kMaxPersistenceFileBytes ||
        plaintext.size() >
            static_cast<size_t>((std::numeric_limits<DWORD>::max)())) {
        error = "plaintext persistence payload exceeds 32 MiB limit";
        return false;
    }

    DATA_BLOB input{};
    input.pbData = reinterpret_cast<BYTE*>(
        const_cast<char*>(plaintext.data()));
    input.cbData = static_cast<DWORD>(plaintext.size());

    const std::string_view entropyText = entropyFor(kind);
    DATA_BLOB entropy{};
    entropy.pbData = reinterpret_cast<BYTE*>(
        const_cast<char*>(entropyText.data()));
    entropy.cbData = static_cast<DWORD>(entropyText.size());

    DATA_BLOB output{};
    if (!::CryptProtectData(&input,
                            descriptionFor(kind),
                            &entropy,
                            nullptr,
                            nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN,
                            &output)) {
        error = "DPAPI protection failed (error " +
            std::to_string(::GetLastError()) + ")";
        return false;
    }

    if (output.cbData == 0 ||
        output.cbData > Limits::kMaxProtectedPersistenceFileBytes -
            kHeaderBytes) {
        if (output.pbData != nullptr) {
            ::LocalFree(output.pbData);
        }
        error = "protected persistence payload exceeds file limit";
        return false;
    }

    ciphertext.assign(output.pbData, output.pbData + output.cbData);
    ::LocalFree(output.pbData);
    return true;
}

bool unprotectPayload(std::string_view ciphertext,
                      ProtectedPersistenceKind kind,
                      std::string& plaintext,
                      std::string& error) {
    plaintext.clear();
    if (ciphertext.empty() ||
        ciphertext.size() >
            static_cast<size_t>((std::numeric_limits<DWORD>::max)())) {
        error = "protected persistence payload length is invalid";
        return false;
    }

    DATA_BLOB input{};
    input.pbData = reinterpret_cast<BYTE*>(
        const_cast<char*>(ciphertext.data()));
    input.cbData = static_cast<DWORD>(ciphertext.size());

    const std::string_view entropyText = entropyFor(kind);
    DATA_BLOB entropy{};
    entropy.pbData = reinterpret_cast<BYTE*>(
        const_cast<char*>(entropyText.data()));
    entropy.cbData = static_cast<DWORD>(entropyText.size());

    DATA_BLOB output{};
    LPWSTR description = nullptr;
    const BOOL ok = ::CryptUnprotectData(&input,
                                         &description,
                                         &entropy,
                                         nullptr,
                                         nullptr,
                                         CRYPTPROTECT_UI_FORBIDDEN,
                                         &output);
    if (description != nullptr) {
        ::LocalFree(description);
    }
    if (!ok) {
        error = "DPAPI unprotection failed (error " +
            std::to_string(::GetLastError()) + ")";
        return false;
    }

    if (output.cbData == 0 || output.pbData == nullptr) {
        if (output.pbData != nullptr) {
            ::LocalFree(output.pbData);
        }
        error = "decrypted persistence payload is empty";
        return false;
    }
    if (output.cbData > Limits::kMaxPersistenceFileBytes) {
        if (output.pbData != nullptr) {
            ::SecureZeroMemory(output.pbData, output.cbData);
            ::LocalFree(output.pbData);
        }
        error = "decrypted persistence payload exceeds 32 MiB limit";
        return false;
    }

    plaintext.assign(reinterpret_cast<const char*>(output.pbData),
                     output.cbData);
    if (output.pbData != nullptr) {
        ::SecureZeroMemory(output.pbData, output.cbData);
        ::LocalFree(output.pbData);
    }
    return true;
}

bool readBoundedFile(const std::filesystem::path& filepath,
                     std::string& bytes,
                     PersistenceLoadResult& result) {
    bytes.clear();
    std::error_code ec;
    const bool exists = std::filesystem::exists(filepath, ec);
    if (ec) {
        result = {PersistenceLoadStatus::IoError,
                  "could not inspect file: " + ec.message()};
        return false;
    }
    if (!exists) {
        result = {PersistenceLoadStatus::Missing, {}};
        return false;
    }

    const uintmax_t fileBytes = std::filesystem::file_size(filepath, ec);
    if (ec) {
        result = {PersistenceLoadStatus::IoError,
                  "could not inspect file size: " + ec.message()};
        return false;
    }
    if (fileBytes > Limits::kMaxProtectedPersistenceFileBytes) {
        result = {PersistenceLoadStatus::Invalid,
                  "protected persistence file exceeds 33 MiB limit"};
        return false;
    }

    std::ifstream input(filepath, std::ios::binary);
    if (!input.is_open()) {
        result = {PersistenceLoadStatus::IoError,
                  "could not open file for reading"};
        return false;
    }
    bytes.resize(static_cast<size_t>(fileBytes));
    if (!bytes.empty()) {
        input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    if (input.bad() || static_cast<size_t>(input.gcount()) != bytes.size()) {
        bytes.clear();
        result = {PersistenceLoadStatus::IoError,
                  "I/O failure while reading file"};
        return false;
    }
    return true;
}

bool writeProtectedPayload(const std::filesystem::path& filepath,
                           ProtectedPersistenceKind kind,
                           std::string_view plaintext,
                           std::string& error) {
    std::vector<uint8_t> ciphertext;
    if (!protectPayload(plaintext, kind, ciphertext, error)) {
        return false;
    }
    if (ciphertext.size() >
        static_cast<size_t>((std::numeric_limits<uint32_t>::max)())) {
        error = "protected persistence payload length overflows header";
        return false;
    }

    std::string envelope;
    envelope.reserve(kHeaderBytes + ciphertext.size());
    envelope.append(reinterpret_cast<const char*>(kMagic.data()),
                    kMagic.size());
    envelope.push_back(static_cast<char>(kEnvelopeVersion));
    envelope.push_back(static_cast<char>(kind));
    envelope.push_back('\0');
    envelope.push_back('\0');
    appendUint32Le(envelope, static_cast<uint32_t>(ciphertext.size()));
    envelope.append(reinterpret_cast<const char*>(ciphertext.data()),
                    ciphertext.size());

    std::error_code ec;
    if (filepath.has_parent_path()) {
        std::filesystem::create_directories(filepath.parent_path(), ec);
    }

    std::filesystem::path temporary = filepath;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output.is_open()) {
            error = "could not open protected persistence temp file";
            return false;
        }
        output.write(envelope.data(),
                     static_cast<std::streamsize>(envelope.size()));
        output.flush();
        if (!output.good()) {
            output.close();
            std::filesystem::remove(temporary, ec);
            error = "could not write protected persistence temp file";
            return false;
        }
    }

    if (!utils::installTempFile(temporary, filepath)) {
        error = "could not atomically install protected persistence file";
        return false;
    }
    return true;
}

} // namespace

ProtectedJsonDocumentLoadResult loadProtectedJsonDocument(
    const std::filesystem::path& filepath,
    ProtectedPersistenceKind kind) {
    ProtectedJsonDocumentLoadResult loaded;
    std::string fileBytes;
    if (!readBoundedFile(filepath, fileBytes, loaded.result)) {
        return loaded;
    }

    std::string plaintext;
    std::string error;
    if (hasMagic(fileBytes)) {
        uint32_t ciphertextBytes = 0;
        if (!validHeader(fileBytes, kind, ciphertextBytes, error)) {
            loaded.result = {PersistenceLoadStatus::Invalid,
                             std::move(error)};
            return loaded;
        }
        const std::string_view ciphertext(
            fileBytes.data() + kHeaderBytes, ciphertextBytes);
        if (!unprotectPayload(ciphertext, kind, plaintext, error)) {
            loaded.result = {PersistenceLoadStatus::Invalid,
                             std::move(error)};
            return loaded;
        }
    } else {
        if (fileBytes.size() > Limits::kMaxPersistenceFileBytes) {
            loaded.result = {PersistenceLoadStatus::Invalid,
                             "legacy plaintext persistence exceeds 32 MiB limit"};
            return loaded;
        }
        plaintext = std::move(fileBytes);
        loaded.legacyPlaintext = true;
    }

    std::string parseError;
    if (!parseBoundedJson(plaintext, loaded.document, parseError)) {
        secureClearString(plaintext);
        loaded.result = {
            PersistenceLoadStatus::Invalid,
            std::move(parseError)};
        return loaded;
    }
    secureClearString(plaintext);

    loaded.result.status = PersistenceLoadStatus::Loaded;
    return loaded;
}

bool saveProtectedJsonDocument(
    const std::filesystem::path& filepath,
    ProtectedPersistenceKind kind,
    const nlohmann::json& document,
    std::string& error) {
    error.clear();
    std::string serialized;
    try {
        serialized = document.dump(2);
    } catch (const nlohmann::json::exception& jsonError) {
        error = std::string("could not serialize protected JSON: ") +
            jsonError.what();
        return false;
    }
    if (serialized.size() > Limits::kMaxPersistenceFileBytes) {
        secureClearString(serialized);
        error = "serialized JSON exceeds 32 MiB limit";
        return false;
    }
    if (!validateSerializedJsonComplexity(serialized, error)) {
        secureClearString(serialized);
        return false;
    }

    const bool saved = writeProtectedPayload(
        filepath, kind, serialized, error);
    secureClearString(serialized);
    return saved;
}

bool isProtectedPersistenceFile(
    const std::filesystem::path& filepath,
    ProtectedPersistenceKind kind) {
    std::string bytes;
    PersistenceLoadResult result;
    if (!readBoundedFile(filepath, bytes, result)) {
        return false;
    }
    uint32_t ciphertextBytes = 0;
    std::string error;
    return validHeader(bytes, kind, ciphertextBytes, error) &&
        ciphertextBytes != 0;
}

} // namespace AI

#endif // HAVE_AI_CHAT
