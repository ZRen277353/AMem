#ifdef HAVE_AI_CHAT

#include "AiSettings.h"

#include "../../third_party/nlohmann/json.hpp"
#include "../../utils/AtomicFileWrite.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace AI {

namespace {

constexpr int kConfigVersion = 1;

int clamp(int v, int lo, int hi) {
    return std::max(lo, std::min(hi, v));
}

} // namespace

PersistenceLoadResult AiSettings::loadFromFile(const std::string& filepath) {
    std::lock_guard<std::mutex> lock(mutex_);
    JsonDocumentLoadResult document = loadJsonDocument(filepath);
    if (document.result.status == PersistenceLoadStatus::Missing) {
        lastPath_ = filepath;
        return document.result;
    }
    if (document.result.failed()) {
        return document.result;
    }

    AiSettingsData loaded;
    try {
        const nlohmann::json& root = document.document;
        if (!root.is_object()) {
            return {PersistenceLoadStatus::Invalid,
                    "settings root must be an object"};
        }
        if (root.contains("version") && !root["version"].is_number_integer()) {
            return {PersistenceLoadStatus::Invalid,
                    "settings 'version' must be an integer"};
        }

        const auto readString = [&](const char* name,
                                    std::string& destination) -> bool {
            const auto field = root.find(name);
            if (field == root.end()) return true;
            if (!field->is_string()) return false;
            destination = field->get<std::string>();
            return true;
        };
        const auto readInt = [&](const char* name, int& destination) -> bool {
            const auto field = root.find(name);
            if (field == root.end()) return true;
            if (!field->is_number_integer()) return false;
            destination = field->get<int>();
            return true;
        };
        const auto readBool = [&](const char* name, bool& destination) -> bool {
            const auto field = root.find(name);
            if (field == root.end()) return true;
            if (!field->is_boolean()) return false;
            destination = field->get<bool>();
            return true;
        };

        if (!readString("activeProvider", loaded.activeProvider) ||
            !readInt("executionTimeout", loaded.executionTimeout) ||
            !readInt("maxAgentSteps", loaded.maxAgentSteps) ||
            !readInt("maxToolCallsPerTurn", loaded.maxToolCallsPerTurn) ||
            !readInt("tokenLimit", loaded.tokenLimit) ||
            !readString("systemPrompt", loaded.systemPrompt) ||
            !readBool("autoApproveWrites", loaded.autoApproveWrites)) {
            return {PersistenceLoadStatus::Invalid,
                    "settings fields have invalid types"};
        }

        loaded.executionTimeout = clamp(loaded.executionTimeout, 1, 300);
        loaded.maxAgentSteps = clamp(loaded.maxAgentSteps, 1, 64);
        loaded.maxToolCallsPerTurn =
            clamp(loaded.maxToolCallsPerTurn, 1, 64);
        loaded.tokenLimit = clamp(loaded.tokenLimit, 1000, 1000000);

        const auto proxy = root.find("proxy");
        if (proxy != root.end()) {
            if (!proxy->is_object()) {
                return {PersistenceLoadStatus::Invalid,
                        "settings 'proxy' must be an object"};
            }
            const auto enabled = proxy->find("enabled");
            const auto host = proxy->find("host");
            const auto port = proxy->find("port");
            if ((enabled != proxy->end() && !enabled->is_boolean()) ||
                (host != proxy->end() && !host->is_string()) ||
                (port != proxy->end() && !port->is_number_integer())) {
                return {PersistenceLoadStatus::Invalid,
                        "proxy fields have invalid types"};
            }
            if (enabled != proxy->end()) {
                loaded.proxy.enabled = enabled->get<bool>();
            }
            if (host != proxy->end()) {
                loaded.proxy.host = host->get<std::string>();
            }
            if (port != proxy->end()) {
                loaded.proxy.port = clamp(port->get<int>(), 0, 65535);
            }
        }
    } catch (const nlohmann::json::exception& error) {
        return {PersistenceLoadStatus::Invalid,
                std::string("invalid settings: ") + error.what()};
    }

    data_ = std::move(loaded);
    lastPath_ = filepath;
    return {PersistenceLoadStatus::Loaded, {}};
}

bool AiSettings::saveToFile(const std::string& filepath) {
    if (filepath.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    lastPath_ = filepath;

    nlohmann::json root = nlohmann::json::object();
    root["version"]            = kConfigVersion;
    root["activeProvider"]     = data_.activeProvider;
    root["executionTimeout"]   = data_.executionTimeout;
    root["maxAgentSteps"]      = data_.maxAgentSteps;
    root["maxToolCallsPerTurn"] = data_.maxToolCallsPerTurn;
    root["tokenLimit"]         = data_.tokenLimit;
    root["systemPrompt"]       = data_.systemPrompt;
    root["autoApproveWrites"]  = data_.autoApproveWrites;

    nlohmann::json proxy = nlohmann::json::object();
    proxy["enabled"] = data_.proxy.enabled;
    proxy["host"]    = data_.proxy.host;
    proxy["port"]    = data_.proxy.port;
    root["proxy"]    = std::move(proxy);

    // Atomic write: dump to sibling `.tmp`, then rename over the target.
    std::filesystem::path targetPath(filepath);
    std::filesystem::path tmpPath = targetPath;
    tmpPath += ".tmp";

    {
        std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return false;
        try {
            out << root.dump(2);
        } catch (const nlohmann::json::exception&) {
            return false;
        }
        out.flush();
        if (!out.good()) return false;
    }

    return utils::installTempFile(tmpPath, targetPath);
}

AiSettingsData AiSettings::get() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return data_;
}

void AiSettings::set(const AiSettingsData& data) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        data_ = data;
        data_.executionTimeout = clamp(data_.executionTimeout, 1, 300);
        data_.maxAgentSteps = clamp(data_.maxAgentSteps, 1, 64);
        data_.maxToolCallsPerTurn = clamp(data_.maxToolCallsPerTurn, 1, 64);
        data_.tokenLimit       = clamp(data_.tokenLimit, 1000, 1000000);
        data_.proxy.port       = clamp(data_.proxy.port, 0, 65535);
    }
    persistLastPath();
}

void AiSettings::setActiveProvider(const std::string& name) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        data_.activeProvider = name;
    }
    persistLastPath();
}

void AiSettings::setExecutionTimeout(int seconds) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        data_.executionTimeout = clamp(seconds, 1, 300);
    }
    persistLastPath();
}

void AiSettings::setTokenLimit(int limit) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        data_.tokenLimit = clamp(limit, 1000, 1000000);
    }
    persistLastPath();
}

void AiSettings::setSystemPrompt(const std::string& prompt) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        data_.systemPrompt = prompt;
    }
    persistLastPath();
}

void AiSettings::setProxy(const ProxyConfig& proxy) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        data_.proxy = proxy;
        data_.proxy.port = clamp(data_.proxy.port, 0, 65535);
    }
    persistLastPath();
}

void AiSettings::persistLastPath() {
    std::string filepath;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        filepath = lastPath_;
    }
    if (!filepath.empty()) {
        saveToFile(filepath);
    }
}

PersistenceLoadResult AiSettings::loadOrDefault(const std::string& filepath) {
    PersistenceLoadResult loaded = loadFromFile(filepath);
    if (loaded.status == PersistenceLoadStatus::Missing &&
        !saveToFile(filepath)) {
        return {PersistenceLoadStatus::IoError,
                "could not create missing settings file"};
    }
    return loaded;
}

} // namespace AI

#endif // HAVE_AI_CHAT
