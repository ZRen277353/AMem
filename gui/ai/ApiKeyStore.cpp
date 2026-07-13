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
#include "../../utils/AtomicFileWrite.h"

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
constexpr int kConfigVersion = 3;
constexpr int kMaxConfiguredContextTokens = 2000000;

constexpr const char kBase64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

bool writeConfigsToFile(
    const std::map<std::string, StoredProviderConfig>& configs,
    const std::string& filepath) {
    nlohmann::json root = nlohmann::json::object();
    root["version"] = kConfigVersion;
    nlohmann::json providers = nlohmann::json::object();
    for (const auto& kv : configs) {
        nlohmann::json entry = nlohmann::json::object();
        entry["apiKey"] = kv.second.encryptedApiKey;
        entry["baseUrl"] = kv.second.baseUrl;
        entry["model"] = kv.second.model;
        if (!kv.second.apiVersion.empty()) {
            entry["apiVersion"] = kv.second.apiVersion;
        }
        if (!kv.second.trustedBaseUrl.empty()) {
            entry["trustedBaseUrl"] = kv.second.trustedBaseUrl;
        }
        if (kv.second.contextWindowTokens > 0) {
            entry["contextWindowTokens"] =
                kv.second.contextWindowTokens;
        }
        providers[kv.first] = std::move(entry);
    }
    root["providers"] = std::move(providers);

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

    return utils::installTempFile(tmpPath, targetPath);
}

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
    if (providerName.empty() || config.contextWindowTokens < 0 ||
        config.contextWindowTokens > kMaxConfiguredContextTokens) {
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
    stored.trustedBaseUrl = config.trustedBaseUrl;
    stored.contextWindowTokens = config.contextWindowTokens;
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
        // AC 10.5: flag the entry for the UI so the user can re-enter the key.
        // Do NOT erase the entry: decryption can fail transiently (e.g. the
        // user profile / DPAPI state isn't ready yet at early startup), and
        // dropping it here would let the next saveToFile() delete the key
        // from disk too — turning a transient glitch into permanent loss.
        // The encrypted blob is kept so it round-trips and can be retried.
        if (std::find(decryptionFailures_.begin(),
                      decryptionFailures_.end(),
                      providerName) == decryptionFailures_.end()) {
            decryptionFailures_.push_back(providerName);
        }
        return false;
    }

    outConfig.apiKey = std::move(plaintext);
    outConfig.baseUrl = it->second.baseUrl;
    outConfig.model = it->second.model;
    outConfig.apiVersion = it->second.apiVersion;
    outConfig.trustedBaseUrl = it->second.trustedBaseUrl;
    outConfig.contextWindowTokens = it->second.contextWindowTokens;
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
    // Deliberately leave the store empty. The provider registry supplies
    // non-secret defaults such as endpoint URLs, while API keys must be
    // entered by the user and stored through storeConfig().
}

// --------------------------------------------------------------------------
// File I/O
// --------------------------------------------------------------------------

PersistenceLoadResult ApiKeyStore::loadFromFile(const std::string& filepath) {
    std::lock_guard<std::mutex> lock(mutex_);
    JsonDocumentLoadResult document = loadJsonDocument(filepath);
    if (document.result.status == PersistenceLoadStatus::Missing) {
        configs_.clear();
        decryptionFailures_.clear();
        return document.result;
    }
    if (document.result.failed()) {
        return document.result;
    }

    std::map<std::string, StoredProviderConfig> loadedConfigs;
    try {
        const nlohmann::json& root = document.document;
        if (!root.is_object()) {
            return {PersistenceLoadStatus::Invalid,
                    "configuration root must be an object"};
        }
        if (root.contains("version") && !root["version"].is_number_integer()) {
            return {PersistenceLoadStatus::Invalid,
                    "configuration 'version' must be an integer"};
        }

        const auto providersIt = root.find("providers");
        if (providersIt != root.end()) {
            if (!providersIt->is_object()) {
                return {PersistenceLoadStatus::Invalid,
                        "configuration 'providers' must be an object"};
            }

            for (auto it = providersIt->begin(); it != providersIt->end(); ++it) {
                if (it.key().empty() || !it.value().is_object()) {
                    return {PersistenceLoadStatus::Invalid,
                            "provider entries require a non-empty name and object value"};
                }

                StoredProviderConfig stored;
                const auto readString = [&](const char* name,
                                            std::string& destination) -> bool {
                    const auto field = it.value().find(name);
                    if (field == it.value().end()) {
                        return true;
                    }
                    if (!field->is_string()) {
                        return false;
                    }
                    destination = field->get<std::string>();
                    return true;
                };

                if (!readString("apiKey", stored.encryptedApiKey) ||
                    !readString("baseUrl", stored.baseUrl) ||
                    !readString("model", stored.model) ||
                    !readString("apiVersion", stored.apiVersion) ||
                    !readString("trustedBaseUrl", stored.trustedBaseUrl)) {
                    return {PersistenceLoadStatus::Invalid,
                            "provider configuration fields must be strings"};
                }
                const auto contextWindow =
                    it.value().find("contextWindowTokens");
                if (contextWindow != it.value().end()) {
                    if (!contextWindow->is_number_integer()) {
                        return {PersistenceLoadStatus::Invalid,
                                "provider contextWindowTokens must be an integer"};
                    }
                    const long long value = contextWindow->get<long long>();
                    if (value < 0 || value > kMaxConfiguredContextTokens) {
                        return {PersistenceLoadStatus::Invalid,
                                "provider contextWindowTokens is out of range"};
                    }
                    stored.contextWindowTokens = static_cast<int>(value);
                }
                loadedConfigs.emplace(it.key(), std::move(stored));
            }
        }
    } catch (const nlohmann::json::exception& error) {
        return {PersistenceLoadStatus::Invalid,
                std::string("invalid provider configuration: ") + error.what()};
    }

    configs_ = std::move(loadedConfigs);
    decryptionFailures_.clear();
    return {PersistenceLoadStatus::Loaded, {}};
}

bool ApiKeyStore::applyChangesAndSave(
    const std::vector<ProviderConfigChange>& changes,
    const std::string& filepath) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::map<std::string, StoredProviderConfig> candidate = configs_;
    for (const ProviderConfigChange& change : changes) {
        if (change.providerName.empty()) {
            return false;
        }
        if (change.remove) {
            candidate.erase(change.providerName);
            continue;
        }
        if (change.config.contextWindowTokens < 0 ||
            change.config.contextWindowTokens > kMaxConfiguredContextTokens) {
            return false;
        }

        std::string encrypted = encryptWithDPAPI(change.config.apiKey);
        if (encrypted.empty()) {
            return false;
        }
        StoredProviderConfig stored;
        stored.encryptedApiKey = std::move(encrypted);
        stored.baseUrl = change.config.baseUrl;
        stored.model = change.config.model;
        stored.apiVersion = change.config.apiVersion;
        stored.trustedBaseUrl = change.config.trustedBaseUrl;
        stored.contextWindowTokens = change.config.contextWindowTokens;
        candidate[change.providerName] = std::move(stored);
    }

    if (!writeConfigsToFile(candidate, filepath)) {
        return false;
    }

    configs_ = std::move(candidate);
    for (const ProviderConfigChange& change : changes) {
        decryptionFailures_.erase(
            std::remove(decryptionFailures_.begin(),
                        decryptionFailures_.end(),
                        change.providerName),
            decryptionFailures_.end());
    }
    return true;
}

bool ApiKeyStore::saveToFile(const std::string& filepath) {
    std::lock_guard<std::mutex> lock(mutex_);
    return writeConfigsToFile(configs_, filepath);
}

} // namespace AI

#endif // HAVE_AI_CHAT
