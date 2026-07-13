#include "gui/ai/ProviderTrust.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class FakeProvider final : public AI::AIProvider {
public:
    std::string getName() const override { return "openai"; }
    std::string getDefaultBaseUrl() const override {
        return "https://api.openai.com/v1";
    }
    AI::ProviderCapabilities getCapabilities() const override {
        return {true, true, 128000};
    }
    void configure(const AI::ProviderConfig& config) override {
        config_ = config;
    }
    const AI::ProviderConfig& getConfig() const override { return config_; }
    void sendCompletion(const AI::CompletionRequest&,
                        AI::CancellationToken) override {}

private:
    AI::ProviderConfig config_;
};

void testCanonicalEndpointIdentity() {
    expect(AI::canonicalEndpointForTrust(
               "  HTTPS://API.OPENAI.COM/v1///  ") ==
               "https://api.openai.com/v1",
           "trust identity should normalize scheme/host and trailing slashes");
    expect(AI::canonicalEndpointForTrust(
               "https://example.invalid/CaseSensitivePath") ==
               "https://example.invalid/CaseSensitivePath",
           "trust identity must preserve path case");
}

void testDefaultEndpointIsImplicitlyTrusted() {
    FakeProvider provider;
    AI::ProviderConfig config;
    config.baseUrl = "HTTPS://API.OPENAI.COM/v1/";
    expect(AI::isDefaultProviderEndpoint(provider, config.baseUrl) &&
               AI::isProviderEndpointTrusted(provider, config),
           "the provider-owned default endpoint should not need confirmation");
}

void testCustomEndpointRequiresExactTrust() {
    FakeProvider provider;
    AI::ProviderConfig config;
    config.baseUrl = "https://gateway.example.invalid/v1";
    expect(!AI::isProviderEndpointTrusted(provider, config),
           "a custom endpoint must start untrusted");

    config.trustedBaseUrl = "HTTPS://GATEWAY.EXAMPLE.INVALID/v1/";
    expect(AI::isProviderEndpointTrusted(provider, config),
           "an endpoint-bound confirmation should authorize the same URL");

    config.baseUrl = "https://gateway.example.invalid/v2";
    expect(!AI::isProviderEndpointTrusted(provider, config),
           "changing any path component must invalidate prior trust");
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, void (*)()>> tests = {
        {"canonical endpoint identity", testCanonicalEndpointIdentity},
        {"default endpoint trust", testDefaultEndpointIsImplicitlyTrusted},
        {"custom endpoint trust binding", testCustomEndpointRequiresExactTrust},
    };
    size_t passed = 0;
    for (const auto& test : tests) {
        try {
            test.second();
            ++passed;
            std::cout << "[PASS] " << test.first << '\n';
        } catch (const std::exception& error) {
            std::cerr << "[FAIL] " << test.first << ": "
                      << error.what() << '\n';
            return 1;
        }
    }
    std::cout << passed << " provider trust test groups passed\n";
    return 0;
}
