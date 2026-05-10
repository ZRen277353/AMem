#ifdef HAVE_AI_CHAT

#include "ApiKeyStore.h"

// Windows + DPAPI. <windows.h> must be included before <wincrypt.h>.
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>

#include "../../third_party/nlohmann/json.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

// DPAPI lives in crypt32.lib. CMake task 10.1 also links this, but the
// explicit pragma guarantees correct linkage when this TU is built standalone
// (e.g. via a unit test) under MSVC/clang-cl.
#pragma comment(lib, "crypt32.lib")

namespace AI {

namespace {

// Description string attached to the DPAPI blob so tooling (e.g. Windows
// credential inspectors) can identify where the blob came from. Purely
// informational – not required for decryption.
const wchar_t kDpapiDescription[] = L"AMem AI Key";

// Current on-disk format version. Bumped if the schema changes.
constexpr int kConfigVersion = 1;

constexpr const char kBase64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int base64CharToValue(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return static_cast<int>(c - 'A');
    if (c >= 'a' && c <= 'z') return static_cast<int>(c - 'a') + 26;
    if (c >= '0' && c <= '9') return static_cast<int>(c - '0') + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1; // invalid / padding / whitespace
}

} // namespace

// --------------------------------------------------------------------------
// Base64
// --------------------------------------------------------------------------

std::string ApiKeyStore::base64Encode(const std::vector<uint8_t>& data) {
    std::string out;
    const size_t n = data.size();
    if (n == 0) {
        return out;
    }
    out.reserve(((n + 2) / 3) * 4);

    size_t i = 0;
    for (; i + 3 <= n; i += 3) {
        uint32_t triple = (static_cast<uint32_t>(data[i]) << 16) |
                          (static_cast<uint32_t>(data[i + 1]) << 8) |
                          static_cast<uint32_t>(data[i + 2]);
        out.push_back(kBase64Alphabet[(triple >> 18) & 0x3F]);
        out.push_back(kBase64Alphabet[(triple >> 12) & 0x3F]);
        out.push_back(kBase64Alphabet[(triple >> 6) & 0x3F]);
        out.push_back(kBase64Alphabet[triple & 0x3F]);
    }
    if (i < n) {
        uint32_t triple = static_cast<uint32_t>(data[i]) << 16;
        bool hasTwo = (i + 1 < n);
        if (hasTwo) {
            triple |= static_cast<uint32_t>(data[i + 1]) << 8;
        }
        out.push_back(kBase64Alphabet[(triple >> 18) & 0x3F]);
        out.push_back(kBase64Alphabet[(triple >> 12) & 0x3F]);
        out.push_back(hasTwo ? kBase64Alphabet[(triple >> 6) & 0x3F] : '=');
        out.push_back('=');
    }
    return out;
}

std::vector<uint8_t> ApiKeyStore::base64Decode(const std::string& encoded) {
    std::vector<uint8_t> out;
    // Pack values into a 24-bit accumulator, 6 bits at a time; skip padding
    // and any whitespace/newlines that may have been introduced by editors.
    int buffer = 0;
    int bits = 0;
    out.reserve((encoded.size() * 3) / 4);
    for (unsigned char c : encoded) {
        if (c == '=' || c == '\r' || c == '\n' || c == ' ' || c == '\t') {
            continue;
        }
        int value = base64CharToValue(c);
        if (value < 0) {
            // Invalid character – return empty to signal a corrupt blob.
            return {};
        }
        buffer = (buffer << 6) | value;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((buffer >> bits) & 0xFF));
        }
    }
    return out;
}

// --------------------------------------------------------------------------
// DPAPI
// --------------------------------------------------------------------------

std::string ApiKeyStore::encryptWithDPAPI(const std::string& plaintext) {
    DATA_BLOB in{};
    in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plaintext.data()));
    in.cbData = static_cast<DWORD>(plaintext.size());

    DATA_BLOB out{};
    // CRYPTPROTECT_UI_FORBIDDEN – never surface a UI prompt during
    // encryption. We are encrypting on the user's own current session so
    // this flag is safe and avoids a hang if the call is made from a
    // background thread.
    BOOL ok = ::CryptProtectData(&in,
                                 kDpapiDescription,
                                 /*pOptionalEntropy*/ nullptr,
                                 /*pvReserved*/ nullptr,
                                 /*pPromptStruct*/ nullptr,
                                 CRYPTPROTECT_UI_FORBIDDEN,
                                 &out);
    if (!ok) {
        return {};
    }

    std::vector<uint8_t> blob(out.pbData, out.pbData + out.cbData);
    if (out.pbData) {
        ::LocalFree(out.pbData);
    }
    return base64Encode(blob);
}

std::string ApiKeyStore::decryptWithDPAPI(const std::string& encrypted, bool& outOk) {
    outOk = false;
    std::vector<uint8_t> blob = base64Decode(encrypted);
    if (blob.empty()) {
        return {};
    }

    DATA_BLOB in{};
    in.pbData = blob.data();
    in.cbData = static_cast<DWORD>(blob.size());

    DATA_BLOB out{};
    LPWSTR description = nullptr;
    BOOL ok = ::CryptUnprotectData(&in,
                                   &description,
                                   /*pOptionalEntropy*/ nullptr,
                                   /*pvReserved*/ nullptr,
                                   /*pPromptStruct*/ nullptr,
                                   CRYPTPROTECT_UI_FORBIDDEN,
                                   &out);
    if (description) {
        ::LocalFree(description);
    }
    if (!ok) {
        return {};
    }

    std::string plaintext(reinterpret_cast<const char*>(out.pbData), out.cbData);
    // The DPAPI blob may contain embedded NULs if the original plaintext
    // did; std::string handles that correctly via the (ptr, len) ctor.
    if (out.pbData) {
        // Zero the buffer before freeing so the plaintext key isn't left
        // lying around in the local heap.
        ::SecureZeroMemory(out.pbData, out.cbData);
        ::LocalFree(out.pbData);
    }
    outOk = true;
    return plaintext;
}

// --------------------------------------------------------------------------
// Config API
// --------------------------------------------------------------------------

bool ApiKeyStore::storeConfigLocked(const std::string& providerName, const ProviderConfig& config) {
    if (providerName.empty()) {
        return false;
    }
    std::string encrypted = encryptWithDPAPI(config.apiKey);
    // An empty apiKey legitimately produces a non-empty DPAPI blob, so an
    // empty encrypted result only happens if CryptProtectData itself failed.
    if (encrypted.empty()) {
        return false;
    }
    StoredProviderConfig stored;
    stored.encryptedApiKey = std::move(encrypted);
    stored.baseUrl = config.baseUrl;
    stored.model = config.model;
    stored.apiVersion = config.apiVersion;
    configs_[providerName] = std::move(stored);
    return true;
}

bool ApiKeyStore::storeConfig(const std::string& providerName, const ProviderConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    return storeConfigLocked(providerName, config);
}

bool ApiKeyStore::loadConfig(const std::string& providerName, ProviderConfig& outConfig) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = configs_.find(providerName);
    if (it == configs_.end()) {
        return false;
    }

    bool ok = false;
    std::string plaintext = decryptWithDPAPI(it->second.encryptedApiKey, ok);
    if (!ok) {
        // AC 10.5: discard the corrupted entry and flag it for the UI so
        // the user can re-enter the key.
        if (std::find(decryptionFailures_.begin(),
                      decryptionFailures_.end(),
                      providerName) == decryptionFailures_.end()) {
            decryptionFailures_.push_back(providerName);
        }
        configs_.erase(it);
        return false;
    }

    outConfig.apiKey = std::move(plaintext);
    outConfig.baseUrl = it->second.baseUrl;
    outConfig.model = it->second.model;
    outConfig.apiVersion = it->second.apiVersion;
    return true;
}

bool ApiKeyStore::hasConfig(const std::string& providerName) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return configs_.find(providerName) != configs_.end();
}

void ApiKeyStore::removeConfig(const std::string& providerName) {
    std::lock_guard<std::mutex> lock(mutex_);
    configs_.erase(providerName);
}

std::vector<std::string> ApiKeyStore::getConfiguredProviders() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> names;
    names.reserve(configs_.size());
    for (const auto& kv : configs_) {
        names.push_back(kv.first);
    }
    return names;
}

std::vector<std::string> ApiKeyStore::getDecryptionFailures() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return decryptionFailures_;
}

void ApiKeyStore::clearDecryptionFailures() {
    std::lock_guard<std::mutex> lock(mutex_);
    decryptionFailures_.clear();
}

void ApiKeyStore::seedDefaultsIfEmpty() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!configs_.empty()) {
        return;
    }
    // USER-REQUESTED default – a working OpenAI-compatible configuration
    // so a fresh install can start chatting without first walking through
    // the settings panel. The user can override any field via the UI.
    ProviderConfig seed;
    seed.apiKey = "sk-mnVq3ahItPaRyRGbtFEdIZTH4JBGFeS0vgf3Y6k1POCB5Kpb";
    seed.baseUrl = "https://ai.ikik.net/v1";
    seed.model = "gpt-5.4";
    seed.apiVersion = "";
    storeConfigLocked("openai", seed);
}

// --------------------------------------------------------------------------
// File I/O
// --------------------------------------------------------------------------

bool ApiKeyStore::loadFromFile(const std::string& filepath) {
    std::lock_guard<std::mutex> lock(mutex_);
    configs_.clear();

    std::error_code ec;
    if (!std::filesystem::exists(filepath, ec)) {
        // AC 10.6: a missing file is not an error. Start with an empty
        // store so the caller (e.g. ChatWindow) can seed defaults.
        return true;
    }

    std::ifstream in(filepath, std::ios::binary);
    if (!in.is_open()) {
        return false;
    }

    nlohmann::json root;
    try {
        in >> root;
    } catch (const nlohmann::json::exception&) {
        // Malformed JSON – leave configs_ empty and report failure so the
        // UI can surface an error instead of silently clobbering the file.
        return false;
    }

    if (!root.is_object()) {
        return false;
    }

    auto providersIt = root.find("providers");
    if (providersIt == root.end() || !providersIt->is_object()) {
        // No providers section is legal – treat as empty store.
        return true;
    }

    for (auto it = providersIt->begin(); it != providersIt->end(); ++it) {
        const auto& entry = it.value();
        if (!entry.is_object()) {
            continue;
        }
        StoredProviderConfig stored;
        stored.encryptedApiKey = entry.value("apiKey", std::string{});
        stored.baseUrl = entry.value("baseUrl", std::string{});
        stored.model = entry.value("model", std::string{});
        stored.apiVersion = entry.value("apiVersion", std::string{});
        configs_[it.key()] = std::move(stored);
    }

    return true;
}

bool ApiKeyStore::saveToFile(const std::string& filepath) {
    std::lock_guard<std::mutex> lock(mutex_);

    nlohmann::json root = nlohmann::json::object();
    root["version"] = kConfigVersion;
    nlohmann::json providers = nlohmann::json::object();
    for (const auto& kv : configs_) {
        nlohmann::json entry = nlohmann::json::object();
        entry["apiKey"] = kv.second.encryptedApiKey;
        entry["baseUrl"] = kv.second.baseUrl;
        entry["model"] = kv.second.model;
        // Only emit apiVersion when it carries information so the on-disk
        // file matches the example in design.md for providers that don't
        // use it (OpenAI/DeepSeek).
        if (!kv.second.apiVersion.empty()) {
            entry["apiVersion"] = kv.second.apiVersion;
        }
        providers[kv.first] = std::move(entry);
    }
    root["providers"] = std::move(providers);

    // Atomic write: dump to a sibling temp file in the same directory, then
    // rename over the target. std::filesystem::rename is atomic on NTFS
    // when both paths sit on the same volume, which is what we guarantee
    // by keeping the temp next to the target.
    std::error_code ec;
    std::filesystem::path targetPath(filepath);
    std::filesystem::path tmpPath = targetPath;
    tmpPath += ".tmp";

    {
        std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            return false;
        }
        try {
            out << root.dump(2);
        } catch (const nlohmann::json::exception&) {
            return false;
        }
        out.flush();
        if (!out.good()) {
            return false;
        }
    }

    std::filesystem::rename(tmpPath, targetPath, ec);
    if (ec) {
        // rename can fail if the target is held open; fall back to
        // remove-then-rename which is still better than leaving the
        // half-written .tmp around.
        std::filesystem::remove(targetPath, ec);
        ec.clear();
        std::filesystem::rename(tmpPath, targetPath, ec);
        if (ec) {
            std::filesystem::remove(tmpPath, ec); // best-effort cleanup
            return false;
        }
    }

    return true;
}

} // namespace AI

#endif // HAVE_AI_CHAT
