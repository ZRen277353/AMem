#ifdef HAVE_AI_CHAT

#include "ProviderRegistry.h"

// The built-in provider classes (OpenAIProvider, ClaudeProvider,
// DeepSeekProvider) are implemented in later tasks (3.1 - 3.3). To keep the
// registry compilable while those files are still being added, the
// includes below are guarded by __has_include. Each provider TU, once it
// exists, should #include its own header directly; initBuiltinProviders()
// then picks it up automatically without further edits here.
#if __has_include("OpenAIProvider.h")
#  include "OpenAIProvider.h"
#  define AI_HAS_OPENAI_PROVIDER 1
#endif
#if __has_include("ClaudeProvider.h")
#  include "ClaudeProvider.h"
#  define AI_HAS_CLAUDE_PROVIDER 1
#endif
#if __has_include("DeepSeekProvider.h")
#  include "DeepSeekProvider.h"
#  define AI_HAS_DEEPSEEK_PROVIDER 1
#endif

#include <utility>

namespace AI {

void ProviderRegistry::registerProvider(std::unique_ptr<AIProvider> provider) {
    if (!provider) {
        return;
    }
    // Capture the key before moving the unique_ptr into the map.
    const std::string name = provider->getName();
    if (name.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    // operator[] + move assignment replaces any existing entry under the same
    // key, which satisfies the "replaces if exists" semantics of Req 1.6.
    providers_[name] = std::move(provider);
}

AIProvider* ProviderRegistry::getProvider(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = providers_.find(name);
    if (it == providers_.end()) {
        return nullptr;
    }
    return it->second.get();
}

std::vector<std::string> ProviderRegistry::getProviderNames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> names;
    names.reserve(providers_.size());
    for (const auto& kv : providers_) {
        names.push_back(kv.first);
    }
    return names;
}

void ProviderRegistry::initBuiltinProviders() {
#ifdef AI_HAS_OPENAI_PROVIDER
    {
        auto provider = std::make_unique<OpenAIProvider>();
        ProviderConfig cfg;
        provider->configure(cfg);
        registerProvider(std::move(provider));
    }
#endif
#ifdef AI_HAS_CLAUDE_PROVIDER
    {
        auto provider = std::make_unique<ClaudeProvider>();
        ProviderConfig cfg;
        provider->configure(cfg);
        registerProvider(std::move(provider));
    }
#endif
#ifdef AI_HAS_DEEPSEEK_PROVIDER
    {
        auto provider = std::make_unique<DeepSeekProvider>();
        ProviderConfig cfg;
        provider->configure(cfg);
        registerProvider(std::move(provider));
    }
#endif
}

} // namespace AI

#endif // HAVE_AI_CHAT
