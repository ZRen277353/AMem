#pragma once
#ifdef HAVE_AI_CHAT

#include "AIProvider.h"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace AI {

// Meyer's singleton registry for AI providers. Thread-safe: all public methods
// acquire an internal mutex because providers can be queried from both the
// ImGui main thread (UI) and background threads that service
// AIProvider::sendCompletion calls.
class ProviderRegistry {
public:
    static ProviderRegistry& getInstance() {
        static ProviderRegistry instance;
        return instance;
    }

    // Register a provider. The key is taken from provider->getName(). If a
    // provider with the same name is already registered, it is replaced
    // (satisfies Requirement 1.6).
    void registerProvider(std::unique_ptr<AIProvider> provider);

    // Look up a provider by name. Returns nullptr if no provider is
    // registered under that name.
    AIProvider* getProvider(const std::string& name);

    // Return the names of all currently registered providers.
    std::vector<std::string> getProviderNames() const;

    // Instantiate and register the built-in providers (OpenAI, Claude,
    // DeepSeek). Safe to call multiple times: existing entries are replaced.
    void initBuiltinProviders();

private:
    ProviderRegistry() = default;
    ~ProviderRegistry() = default;
    ProviderRegistry(const ProviderRegistry&) = delete;
    ProviderRegistry& operator=(const ProviderRegistry&) = delete;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<AIProvider>> providers_;
};

} // namespace AI

#endif // HAVE_AI_CHAT
