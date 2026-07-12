#pragma once
#ifdef HAVE_AI_CHAT

#include "AIProvider.h"
#include "Persistence.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace AI {

// On-disk form of a provider configuration. The apiKey is kept as a
// DPAPI-encrypted blob, base64-encoded so it can round-trip through JSON
// cleanly. The other fields are plaintext because they are not sensitive.
struct StoredProviderConfig {
    std::string encryptedApiKey; // DPAPI-encrypted, base64 encoded
    std::string baseUrl;
    std::string model;
    std::string apiVersion;
};

// Meyer's singleton that keeps all AI provider configurations on disk in
// `ai_config.dat`. API keys are protected via the Windows DPAPI
// (CryptProtectData / CryptUnprotectData) before being written, so a key
// stored by one Windows user cannot be read back by another. All public
// methods are thread-safe: the same store is read from the ImGui main
// thread (for UI) and from the background HTTP worker threads.
class ApiKeyStore {
public:
    static ApiKeyStore& getInstance() {
        static ApiKeyStore instance;
        return instance;
    }

    // Load / save the entire store as a JSON file. See design.md,
    // "Provider 配置存储格式", for the exact layout. The default filename
    // uses a .json extension so the file is recognisable as user-editable
    // JSON (the API key value is still a DPAPI-encrypted base64 blob).
    PersistenceLoadResult loadFromFile(
        const std::string& filepath = "ai_config.json");
    bool saveToFile(const std::string& filepath = "ai_config.json");

    // Store a provider configuration. The apiKey is encrypted with DPAPI
    // before being placed in the in-memory map. Returns false (without
    // storing anything) if encryption fails.
    bool storeConfig(const std::string& providerName, const ProviderConfig& config);

    // Retrieve a provider configuration. The apiKey in outConfig is the
    // decrypted plaintext. Returns false if the provider is not stored or
    // if decryption fails; on decryption failure the entry is RETAINED (so a
    // transient DPAPI failure can't cause permanent key loss on the next
    // save) and the provider name is added to getDecryptionFailures() so the
    // UI can prompt the user to re-enter the key (per AC 10.5).
    bool loadConfig(const std::string& providerName, ProviderConfig& outConfig);

    bool hasConfig(const std::string& providerName) const;
    void removeConfig(const std::string& providerName);
    std::vector<std::string> getConfiguredProviders() const;

    // Set of provider names whose entries failed decryption on load and
    // should be re-entered by the user (AC 10.5). The UI should consume
    // these and then call clearDecryptionFailures().
    std::vector<std::string> getDecryptionFailures() const;
    void clearDecryptionFailures();

    // Seed non-secret defaults if no configuration has been loaded.
    // Intentionally does not create a provider entry with a bundled API key:
    // users must explicitly enter their own key in the settings panel.
    void seedDefaultsIfEmpty();

private:
    ApiKeyStore() = default;
    ~ApiKeyStore() = default;
    ApiKeyStore(const ApiKeyStore&) = delete;
    ApiKeyStore& operator=(const ApiKeyStore&) = delete;

    // DPAPI helpers. encryptWithDPAPI returns an empty string on failure;
    // decryptWithDPAPI sets outOk=false and returns an empty string on
    // failure so the caller can distinguish an empty plaintext from a
    // broken blob.
    std::string encryptWithDPAPI(const std::string& plaintext);
    std::string decryptWithDPAPI(const std::string& encrypted, bool& outOk);

    // Base64 codec for DPAPI blobs. Implemented inline (no new dependency)
    // using the standard alphabet with '=' padding.
    std::string base64Encode(const std::vector<uint8_t>& data);
    std::vector<uint8_t> base64Decode(const std::string& encoded);

    // Internal un-locked helpers so public methods can reuse them while
    // already holding mutex_.
    bool storeConfigLocked(const std::string& providerName, const ProviderConfig& config);

    mutable std::mutex mutex_;
    std::map<std::string, StoredProviderConfig> configs_;
    std::vector<std::string> decryptionFailures_;
};

} // namespace AI

#endif // HAVE_AI_CHAT
