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

bool AiSettings::loadFromFile(const std::string& filepath) {
    std::lock_guard<std::mutex> lock(mutex_);
    lastPath_ = filepath;

    std::error_code ec;
    if (!std::filesystem::exists(filepath, ec)) {
        // Missing file is not an error; caller gets defaults.
        return false;
    }

    std::ifstream in(filepath, std::ios::binary);
    if (!in.is_open()) {
        return false;
    }

    nlohmann::json root;
    try {
        in >> root;
    } catch (const nlohmann::json::exception&) {
        return false;
    }
    if (!root.is_object()) {
        return false;
    }

    AiSettingsData loaded;
    loaded.activeProvider     = root.value("activeProvider", loaded.activeProvider);
    loaded.executionTimeout   = clamp(root.value("executionTimeout", loaded.executionTimeout), 1, 300);
    loaded.maxAgentSteps      = clamp(root.value("maxAgentSteps", loaded.maxAgentSteps), 1, 64);
    loaded.maxToolCallsPerTurn = clamp(root.value("maxToolCallsPerTurn", loaded.maxToolCallsPerTurn), 1, 64);
    loaded.tokenLimit         = clamp(root.value("tokenLimit", loaded.tokenLimit), 1000, 1000000);
    loaded.systemPrompt       = root.value("systemPrompt", loaded.systemPrompt);
    loaded.autoApproveWrites  = root.value("autoApproveWrites", loaded.autoApproveWrites);

    if (root.contains("proxy") && root["proxy"].is_object()) {
        const auto& p = root["proxy"];
        loaded.proxy.enabled = p.value("enabled", false);
        loaded.proxy.host    = p.value("host", std::string{});
        loaded.proxy.port    = clamp(p.value("port", 0), 0, 65535);
    }

    data_ = std::move(loaded);
    return true;
}

bool AiSettings::saveToFile(const std::string& filepath) {
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
    saveToFile(lastPath_);
}

void AiSettings::setActiveProvider(const std::string& name) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        data_.activeProvider = name;
    }
    saveToFile(lastPath_);
}

void AiSettings::setExecutionTimeout(int seconds) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        data_.executionTimeout = clamp(seconds, 1, 300);
    }
    saveToFile(lastPath_);
}

void AiSettings::setTokenLimit(int limit) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        data_.tokenLimit = clamp(limit, 1000, 1000000);
    }
    saveToFile(lastPath_);
}

void AiSettings::setSystemPrompt(const std::string& prompt) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        data_.systemPrompt = prompt;
    }
    saveToFile(lastPath_);
}

void AiSettings::setProxy(const ProxyConfig& proxy) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        data_.proxy = proxy;
        data_.proxy.port = clamp(data_.proxy.port, 0, 65535);
    }
    saveToFile(lastPath_);
}

bool AiSettings::loadOrDefault(const std::string& filepath) {
    const bool loaded = loadFromFile(filepath);
    if (!loaded) {
        // Persist defaults so the user sees an editable file on first run.
        saveToFile(filepath);
    }
    return loaded;
}

} // namespace AI

#endif // HAVE_AI_CHAT
