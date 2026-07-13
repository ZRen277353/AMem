#include "gui/ai/ClaudeProvider.h"
#include "gui/ai/DeepSeekProvider.h"
#include "gui/ai/HttpClient.h"
#include "gui/ai/OpenAIProvider.h"
#include "gui/ai/UIMessageQueue.h"

#include "httplib.h"
#include "nlohmann/json.hpp"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using nlohmann::json;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class TempDirectory {
public:
    TempDirectory() {
        const auto serial = std::chrono::steady_clock::now()
            .time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            ("amem-provider-tls-" + std::to_string(serial));
        std::error_code ec;
        std::filesystem::create_directories(path_, ec);
        expect(!ec, "could not create TLS fixture directory");
    }

    ~TempDirectory() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

class GeneratedCertificate {
public:
    GeneratedCertificate(const std::filesystem::path& directory,
                         const std::string& label)
        : certificatePath_(directory / (label + "-cert.pem")),
          privateKeyPath_(directory / (label + "-key.pem")) {
        generate();
    }

    const std::filesystem::path& certificatePath() const {
        return certificatePath_;
    }

    const std::filesystem::path& privateKeyPath() const {
        return privateKeyPath_;
    }

private:
    static void addExtension(X509* certificate,
                             int nid,
                             const char* value) {
        X509V3_CTX context;
        X509V3_set_ctx_nodb(&context);
        X509V3_set_ctx(&context, certificate, certificate,
                       nullptr, nullptr, 0);
        X509_EXTENSION* extension = X509V3_EXT_conf_nid(
            nullptr, &context, nid, const_cast<char*>(value));
        expect(extension != nullptr, "could not create X509 extension");
        const int added = X509_add_ext(certificate, extension, -1);
        X509_EXTENSION_free(extension);
        expect(added == 1, "could not add X509 extension");
    }

    void generate() {
        EVP_PKEY_CTX* keyContext = EVP_PKEY_CTX_new_id(
            EVP_PKEY_RSA, nullptr);
        expect(keyContext != nullptr, "could not allocate RSA key context");
        EVP_PKEY* rawKey = nullptr;
        const bool keyGenerated =
            EVP_PKEY_keygen_init(keyContext) > 0 &&
            EVP_PKEY_CTX_set_rsa_keygen_bits(keyContext, 2048) > 0 &&
            EVP_PKEY_keygen(keyContext, &rawKey) > 0;
        EVP_PKEY_CTX_free(keyContext);
        expect(keyGenerated && rawKey != nullptr,
               "could not generate TLS fixture key");
        std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(
            rawKey, EVP_PKEY_free);

        std::unique_ptr<X509, decltype(&X509_free)> certificate(
            X509_new(), X509_free);
        expect(certificate != nullptr, "could not allocate TLS certificate");
        expect(X509_set_version(certificate.get(), 2) == 1 &&
                   ASN1_INTEGER_set(
                       X509_get_serialNumber(certificate.get()), 1) == 1 &&
                   X509_gmtime_adj(
                       X509_getm_notBefore(certificate.get()), -60) != nullptr &&
                   X509_gmtime_adj(
                       X509_getm_notAfter(certificate.get()), 3600) != nullptr &&
                   X509_set_pubkey(certificate.get(), key.get()) == 1,
               "could not initialize TLS certificate");

        X509_NAME* subject = X509_get_subject_name(certificate.get());
        expect(subject != nullptr &&
                   X509_NAME_add_entry_by_txt(
                       subject, "CN", MBSTRING_ASC,
                       reinterpret_cast<const unsigned char*>("localhost"),
                       -1, -1, 0) == 1 &&
                   X509_set_issuer_name(certificate.get(), subject) == 1,
               "could not set TLS certificate identity");

        addExtension(certificate.get(), NID_basic_constraints,
                     "critical,CA:TRUE");
        addExtension(certificate.get(), NID_key_usage,
                     "critical,digitalSignature,keyEncipherment,keyCertSign");
        addExtension(certificate.get(), NID_ext_key_usage, "serverAuth");
        addExtension(certificate.get(), NID_subject_alt_name,
                     "IP:127.0.0.1");
        expect(X509_sign(certificate.get(), key.get(), EVP_sha256()) > 0,
               "could not sign TLS fixture certificate");

        FILE* certificateFile = nullptr;
        expect(fopen_s(&certificateFile,
                       certificatePath_.string().c_str(), "wb") == 0 &&
                   certificateFile != nullptr,
               "could not open TLS certificate fixture");
        const int certificateWritten =
            PEM_write_X509(certificateFile, certificate.get());
        std::fclose(certificateFile);
        expect(certificateWritten == 1,
               "could not write TLS certificate fixture");

        FILE* keyFile = nullptr;
        expect(fopen_s(&keyFile,
                       privateKeyPath_.string().c_str(), "wb") == 0 &&
                   keyFile != nullptr,
               "could not open TLS key fixture");
        const int keyWritten = PEM_write_PrivateKey(
            keyFile, key.get(), nullptr, nullptr, 0, nullptr, nullptr);
        std::fclose(keyFile);
        expect(keyWritten == 1, "could not write TLS key fixture");
    }

    std::filesystem::path certificatePath_;
    std::filesystem::path privateKeyPath_;
};

struct RecordedRequest {
    std::string authorization;
    std::string apiKey;
    std::string apiVersion;
    std::string contentType;
    size_t contentTypeCount = 0;
    std::string body;
};

class LocalTlsProviderServer {
public:
    explicit LocalTlsProviderServer(const GeneratedCertificate& certificate)
        : server_(certificate.certificatePath().string().c_str(),
                  certificate.privateKeyPath().string().c_str()) {
        expect(server_.is_valid(), "local TLS server certificate is invalid");

        server_.Post("/v1/chat/completions",
            [this](const httplib::Request& request,
                   httplib::Response& response) {
                record("openai", request);
                response.set_content(
                    R"({"choices":[{"message":{"role":"assistant","content":"openai-ok","tool_calls":[{"id":"call_openai","type":"function","function":{"name":"status","arguments":"{}"}}]}}]})",
                    "application/json");
            });
        server_.Post("/v1/messages",
            [this](const httplib::Request& request,
                   httplib::Response& response) {
                record("claude", request);
                response.set_content(
                    R"({"content":[{"type":"text","text":"claude-ok"},{"type":"tool_use","id":"call_claude","name":"status","input":{}}],"stop_reason":"tool_use"})",
                    "application/json");
            });
        server_.Post("/chat/completions",
            [this](const httplib::Request& request,
                   httplib::Response& response) {
                record("deepseek", request);
                response.set_content(
                    R"({"choices":[{"message":{"role":"assistant","content":"deepseek-ok","tool_calls":[{"id":"call_deepseek","type":"function","function":{"name":"status","arguments":"{}"}}]}}]})",
                    "application/json");
            });
        server_.Post("/error/chat/completions",
            [this](const httplib::Request& request,
                   httplib::Response& response) {
                record("error", request);
                response.status = 401;
                response.set_content(
                    R"({"error":{"message":"bad token","code":"invalid_api_key"}})",
                    "application/json");
            });
        server_.Post("/dense/chat/completions",
            [this](const httplib::Request& request,
                   httplib::Response& response) {
                record("dense-openai", request);
                response.set_content(denseResponse(), "application/json");
            });
        server_.Post("/dense/messages",
            [this](const httplib::Request& request,
                   httplib::Response& response) {
                record("dense-claude", request);
                response.set_content(denseResponse(), "application/json");
            });

        port_ = server_.bind_to_any_port("127.0.0.1");
        expect(port_ > 0, "could not bind local TLS provider server");
        thread_ = std::thread([this] { server_.listen_after_bind(); });
    }

    ~LocalTlsProviderServer() {
        server_.stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    std::string baseUrl() const {
        return "https://127.0.0.1:" + std::to_string(port_);
    }

    RecordedRequest request(const std::string& name) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = requests_.find(name);
        return found == requests_.end() ? RecordedRequest{} : found->second;
    }

private:
    static const std::string& denseResponse() {
        static const std::string response = [] {
            std::string value = "{\"dense\":[";
            constexpr size_t kArrays = 26u;
            constexpr size_t kItems = 4000u;
            value.reserve(kArrays * (kItems * 2u + 3u) + 16u);
            for (size_t arrayIndex = 0; arrayIndex < kArrays; ++arrayIndex) {
                if (arrayIndex != 0) value.push_back(',');
                value.push_back('[');
                for (size_t item = 0; item < kItems; ++item) {
                    if (item != 0) value.push_back(',');
                    value.push_back('0');
                }
                value.push_back(']');
            }
            value += "]}";
            return value;
        }();
        return response;
    }

    void record(const std::string& name, const httplib::Request& request) {
        RecordedRequest recorded;
        recorded.authorization = request.get_header_value("Authorization");
        recorded.apiKey = request.get_header_value("x-api-key");
        recorded.apiVersion =
            request.get_header_value("anthropic-version");
        recorded.contentType = request.get_header_value("Content-Type");
        recorded.contentTypeCount =
            request.get_header_value_count("Content-Type");
        recorded.body = request.body;
        std::lock_guard<std::mutex> lock(mutex_);
        requests_[name] = std::move(recorded);
    }

    httplib::SSLServer server_;
    int port_ = 0;
    std::thread thread_;
    mutable std::mutex mutex_;
    std::map<std::string, RecordedRequest> requests_;
};

AI::CompletionRequest requestTemplate(const std::string& model) {
    AI::CompletionRequest request;
    request.runId = "provider-http-test";
    request.model = model;
    request.stream = false;
    request.maxOutputTokens = 321;
    AI::ChatMessage message;
    message.role = AI::Role::User;
    message.content = "hello";
    request.messages.push_back(std::move(message));
    return request;
}

template <typename Provider>
AI::CompletionResponse complete(Provider& provider,
                                AI::CompletionRequest request) {
    auto promise = std::make_shared<std::promise<AI::CompletionResponse>>();
    std::future<AI::CompletionResponse> future = promise->get_future();
    request.onComplete = [promise](AI::CompletionResponse response) {
        promise->set_value(std::move(response));
    };
    provider.sendCompletion(
        request, std::make_shared<std::atomic<bool>>(false));
    expect(future.wait_for(5s) == std::future_status::ready,
           "provider completion timed out");
    return future.get();
}

void expectToolResponse(const AI::CompletionResponse& response,
                        const std::string& content,
                        const std::string& callId) {
    const bool valid = !response.error &&
               response.message.content == content &&
               response.message.toolCalls.size() == 1 &&
               response.message.toolCalls.front().id == callId &&
               response.message.toolCalls.front().name == "status" &&
               response.message.toolCalls.front().arguments == "{}";
    std::string detail = "provider full response did not preserve content/tool call";
    if (response.error) {
        detail += ": " + response.error.message;
    } else {
        detail += ": content='" + response.message.content +
            "' calls=" +
            std::to_string(response.message.toolCalls.size());
    }
    expect(valid, detail);
}

void testTlsCertificateVerification(
    LocalTlsProviderServer& server,
    const GeneratedCertificate& trusted,
    const GeneratedCertificate& untrusted) {
    AI::HttpClient::getInstance().setCaCertificatePath(
        untrusted.certificatePath().string());
    std::promise<AI::HttpResponse> promise;
    std::future<AI::HttpResponse> future = promise.get_future();
    const uint64_t id = AI::HttpClient::getInstance().postAsync(
        server.baseUrl() + "/v1/chat/completions", {}, "{}", nullptr,
        [&promise](AI::HttpResponse response) {
            promise.set_value(std::move(response));
        },
        std::make_shared<std::atomic<bool>>(false));
    expect(id != 0 && future.wait_for(5s) == std::future_status::ready,
           "TLS trust rejection request did not complete");
    const AI::HttpResponse response = future.get();
    AI::HttpClient::getInstance().setCaCertificatePath(
        trusted.certificatePath().string());
    expect(response.statusCode == 0 && !response.errorMessage.empty(),
           "TLS client accepted a certificate outside the configured trust root");
}

void testOpenAIFullResponse(LocalTlsProviderServer& server) {
    AI::OpenAIProvider provider;
    AI::ProviderConfig config;
    config.apiKey = "openai-test-key";
    config.baseUrl = server.baseUrl() + "/v1";
    config.model = "openai-test-model";
    provider.configure(config);

    const AI::CompletionResponse response = complete(
        provider, requestTemplate(config.model));
    expectToolResponse(response, "openai-ok", "call_openai");

    const RecordedRequest recorded = server.request("openai");
    const json body = json::parse(recorded.body);
    expect(recorded.authorization == "Bearer openai-test-key" &&
               recorded.contentType == "application/json" &&
               recorded.contentTypeCount == 1 &&
               body.at("model") == config.model &&
               body.at("stream") == false &&
               body.at("max_completion_tokens") == 321,
           "OpenAI TLS request contract is incorrect");
}

void testClaudeFullResponse(LocalTlsProviderServer& server) {
    AI::ClaudeProvider provider;
    AI::ProviderConfig config;
    config.apiKey = "claude-test-key";
    config.baseUrl = server.baseUrl() + "/v1";
    config.model = "claude-test-model";
    config.apiVersion = "2023-06-01";
    provider.configure(config);

    const AI::CompletionResponse response = complete(
        provider, requestTemplate(config.model));
    expectToolResponse(response, "claude-ok", "call_claude");

    const RecordedRequest recorded = server.request("claude");
    const json body = json::parse(recorded.body);
    expect(recorded.apiKey == "claude-test-key" &&
               recorded.apiVersion == config.apiVersion &&
               recorded.contentType == "application/json" &&
               recorded.contentTypeCount == 1 &&
               body.at("model") == config.model &&
               body.at("stream") == false &&
               body.at("max_tokens") == 321,
           "Claude TLS request contract is incorrect");
}

void testDeepSeekFullResponse(LocalTlsProviderServer& server) {
    AI::DeepSeekProvider provider;
    AI::ProviderConfig config;
    config.apiKey = "deepseek-test-key";
    config.baseUrl = server.baseUrl();
    config.model = "deepseek-test-model";
    provider.configure(config);

    const AI::CompletionResponse response = complete(
        provider, requestTemplate(config.model));
    expectToolResponse(response, "deepseek-ok", "call_deepseek");

    const RecordedRequest recorded = server.request("deepseek");
    const json body = json::parse(recorded.body);
    expect(recorded.authorization == "Bearer deepseek-test-key" &&
               recorded.contentType == "application/json" &&
               recorded.contentTypeCount == 1 &&
               body.at("model") == config.model &&
               body.at("stream") == false &&
               body.at("max_tokens") == 321,
           "DeepSeek TLS request contract is incorrect");
}

void testProviderHttpError(LocalTlsProviderServer& server) {
    AI::OpenAIProvider provider;
    AI::ProviderConfig config;
    config.apiKey = "rejected-key";
    config.baseUrl = server.baseUrl() + "/error";
    config.model = "error-model";
    provider.configure(config);

    const AI::CompletionResponse response = complete(
        provider, requestTemplate(config.model));
    expect(response.error.category == AI::ErrorCategory::Authentication &&
               response.error.httpStatusCode == 401 &&
               response.error.providerErrorCode == "invalid_api_key" &&
               response.error.message.find("bad token") != std::string::npos,
           "provider HTTP error contract lost status/code/message");
}

void expectComplexityRejected(const AI::CompletionResponse& response,
                              const std::string& provider) {
    expect(response.error.category == AI::ErrorCategory::InvalidResponse &&
               response.error.message.find("node limit") !=
                   std::string::npos &&
               response.message.content.empty() &&
               response.message.toolCalls.empty(),
           provider +
               " accepted or partially committed an over-complex full response: " +
               response.error.message);
}

void testProviderFullResponseComplexity(LocalTlsProviderServer& server) {
    AI::ProviderConfig config;
    config.apiKey = "complexity-key";
    config.baseUrl = server.baseUrl() + "/dense";
    config.model = "complexity-model";

    AI::OpenAIProvider openai;
    openai.configure(config);
    expectComplexityRejected(
        complete(openai, requestTemplate(config.model)), "OpenAI");

    AI::ClaudeProvider claude;
    claude.configure(config);
    expectComplexityRejected(
        complete(claude, requestTemplate(config.model)), "Claude");

    AI::DeepSeekProvider deepseek;
    deepseek.configure(config);
    expectComplexityRejected(
        complete(deepseek, requestTemplate(config.model)), "DeepSeek");
}

void drainUiQueue() {
    AI::UIMessage message;
    while (AI::UIMessageQueue::getInstance().tryPop(message)) {
    }
}

} // namespace

int main() {
    TempDirectory temporary;
    GeneratedCertificate certificate(temporary.path(), "trusted");
    GeneratedCertificate untrustedCertificate(temporary.path(), "untrusted");
    LocalTlsProviderServer server(certificate);

    AI::HttpClient& client = AI::HttpClient::getInstance();
    client.setConnectionTimeout(2);
    client.setResponseTimeout(5);
    client.setCaCertificatePath(certificate.certificatePath().string());

    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"OpenAI TLS full response",
         [&] { testOpenAIFullResponse(server); }},
        {"Claude TLS full response",
         [&] { testClaudeFullResponse(server); }},
        {"DeepSeek TLS full response",
         [&] { testDeepSeekFullResponse(server); }},
        {"provider HTTP error mapping",
         [&] { testProviderHttpError(server); }},
        {"provider full-response complexity",
         [&] { testProviderFullResponseComplexity(server); }},
        {"TLS trust verification",
         [&] {
             testTlsCertificateVerification(
                 server, certificate, untrustedCertificate);
         }},
    };

    size_t passed = 0;
    for (const auto& test : tests) {
        try {
            test.second();
            drainUiQueue();
            ++passed;
            std::cout << "[PASS] " << test.first << '\n';
        } catch (const std::exception& error) {
            std::cerr << "[FAIL] " << test.first << ": "
                      << error.what() << '\n';
            (void)client.shutdown();
            return 1;
        }
    }

    client.setCaCertificatePath({});
    expect(client.shutdown(), "HTTP integration workers did not drain");
    std::cout << passed << " provider HTTP/TLS integration groups passed\n";
    return 0;
}
