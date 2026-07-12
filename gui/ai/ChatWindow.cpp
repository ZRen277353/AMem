#ifdef HAVE_AI_CHAT

#include "ChatWindow.h"
#include "ToolCallSecurity.h"

#include "ApiKeyStore.h"
#include "AgentMutationAudit.h"
#include "AgentTaskExecutor.h"
#include "AiSettings.h"
#include "DefaultSystemPrompt.h"
#include "HttpClient.h"
#include "ProviderRegistry.h"
#include "SessionManager.h"
#include "ToolExecutor.h"
#include "UIMessageQueue.h"

#include "../ColorScheme.h"
#include "../Gui.h"
#include "../../imgui/imgui.h"
#include "../../mem/SystemMemService.h"
#include "../../third_party/nlohmann/json.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace AI {

namespace {

// Trim leading/trailing ASCII whitespace. Used by sendMessage() to drop
// whitespace-only submissions (AC 9.10).
std::string trimWhitespace(const std::string& s) {
    size_t begin = 0;
    while (begin < s.size() && std::isspace(static_cast<unsigned char>(s[begin]))) {
        ++begin;
    }
    size_t end = s.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return s.substr(begin, end - begin);
}

CancellationToken makeCancellationToken() {
    return std::make_shared<std::atomic<bool>>(false);
}

constexpr size_t kMaxInboundToolCalls = 64;
constexpr size_t kMaxToolCallIdBytes = 256;
constexpr size_t kMaxToolCallNameBytes = 64;
constexpr size_t kMaxToolCallArgumentsBytes = 512 * 1024;

bool persistMutationOutcome(
    const std::string& runId,
    const ToolCall& call,
    const ToolResult& result,
    const Mem::OperationContext& context,
    MutationApproval approval,
    long long durationMs,
    std::string& error) {
    ToolExecutor& tools = ToolExecutor::getInstance();
    const ToolSafety safety = tools.getToolSafety(call.name);
    if (safety != ToolSafety::Write) {
        return true;
    }

    AgentMutationAuditEvent event;
    event.runId = runId;
    event.call = call;
    event.result = result;
    event.context = context;
    event.safety = safety;
    event.targetPolicy = tools.getToolTargetPolicy(call.name);
    event.approval = approval;
    event.durationMs = durationMs;
    return AgentMutationAuditLog::getInstance().append(event, &error);
}

bool startsWithICase(const std::string& s, const char* prefix) {
    const size_t n = std::strlen(prefix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        const unsigned char a = static_cast<unsigned char>(s[i]);
        const unsigned char b = static_cast<unsigned char>(prefix[i]);
        if (std::tolower(a) != std::tolower(b)) return false;
    }
    return true;
}

bool getProviderConfigProblem(const AIProvider* provider, const std::string& model, std::string& out) {
    if (!provider) {
        out = "Configure a provider in Settings before sending";
        return true;
    }

    const ProviderConfig& cfg = provider->getConfig();
    if (cfg.apiKey.empty()) {
        out = "Enter an API key in Settings before sending";
        return true;
    }
    if (cfg.baseUrl.empty() || !startsWithICase(cfg.baseUrl, "https://")) {
        out = "Configure a valid https:// endpoint in Settings before sending";
        return true;
    }
    if (model.empty() && cfg.model.empty()) {
        out = "Enter a model name before sending";
        return true;
    }
    out.clear();
    return false;
}

std::string validateAndNormalizeToolCalls(std::vector<ToolCall>& calls) {
    if (calls.size() > kMaxInboundToolCalls) {
        return "too many tool calls in one response (max " +
               std::to_string(kMaxInboundToolCalls) + ")";
    }

    std::vector<std::string> seenIds;
    seenIds.reserve(calls.size());
    for (size_t i = 0; i < calls.size(); ++i) {
        ToolCall& call = calls[i];
        const std::string label = "tool call #" + std::to_string(i + 1);
        const std::string id = trimWhitespace(call.id);
        if (id.empty()) {
            return label + " is missing a tool_call id";
        }
        if (id.size() > kMaxToolCallIdBytes) {
            return label + " tool_call id is too long";
        }
        if (std::find(seenIds.begin(), seenIds.end(), id) != seenIds.end()) {
            return label + " duplicates tool_call id '" + id + "'";
        }
        seenIds.push_back(id);
        call.id = id;
        call.name = trimWhitespace(call.name);
        if (call.name.empty()) {
            return label + " is missing a tool name";
        }
        if (call.name.size() > kMaxToolCallNameBytes) {
            return label + " tool name is too long";
        }
        if (trimWhitespace(call.arguments).empty()) {
            call.arguments = "{}";
        }
        if (call.arguments.size() > kMaxToolCallArgumentsBytes) {
            return label + " arguments are too large";
        }
        try {
            const nlohmann::json parsed = nlohmann::json::parse(call.arguments);
            if (!parsed.is_object()) {
                return label + " arguments must be a JSON object";
            }
            call.arguments = parsed.dump();
            if (call.arguments.size() > kMaxToolCallArgumentsBytes) {
                return label + " normalized arguments are too large";
            }
        } catch (const nlohmann::json::exception& e) {
            return label + " has invalid JSON arguments: " + e.what();
        }
    }
    return {};
}

// Current wall-clock time as seconds since the Unix epoch. Used as the
// ChatMessage.timestamp value so rendered times reflect real calendar
// time rather than an arbitrary monotonic baseline.
long long nowUnixSeconds() {
    using namespace std::chrono;
    return duration_cast<seconds>(
               system_clock::now().time_since_epoch()).count();
}

// Current steady-clock time in milliseconds. Used for measuring response
// latency. steady_clock is immune to wall-clock adjustments, so the
// delta is accurate even if the system clock drifts during a long
// completion.
long long nowSteadyMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
               steady_clock::now().time_since_epoch()).count();
}

// Format a Unix timestamp as "HH:MM:SS" in the local timezone. Returns
// empty string when ts == 0 (legacy / unknown).
std::string formatLocalTime(long long ts) {
    if (ts == 0) return {};
    std::time_t t = static_cast<std::time_t>(ts);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return std::string(buf);
}

// Format a duration in milliseconds as a human-friendly string:
// "342ms", "2.4s", "1m 3s". Returns empty when ms == 0.
std::string formatDuration(long long ms) {
    if (ms <= 0) return {};
    char buf[32];
    if (ms < 1000) {
        std::snprintf(buf, sizeof(buf), "%lldms", ms);
    } else if (ms < 60 * 1000) {
        const double s = ms / 1000.0;
        std::snprintf(buf, sizeof(buf), "%.1fs", s);
    } else {
        long long totalSec = ms / 1000;
        long long m = totalSec / 60;
        long long s = totalSec % 60;
        std::snprintf(buf, sizeof(buf), "%lldm %llds", m, s);
    }
    return std::string(buf);
}

const char* stateLabel(ChatWindow* /*unused*/, int stateValue) {
    // Internal helper: the State enum is private, so callers pass an int
    // cast of the enum value. Keeps the label logic out of the header.
    switch (stateValue) {
        case 0: return "Idle";
        case 1: return "Waiting";
        case 2: return "Tool Confirmation";
        case 3: return "Tool Executing";
        default: return "?";
    }
}

const char* traceTypeLabel(AgentTraceType type) {
    switch (type) {
        case AgentTraceType::Started:            return "started";
        case AgentTraceType::ModelRequestDispatched:
            return "model";
        case AgentTraceType::CallLimitTruncated: return "limited";
        case AgentTraceType::StepLimitReached:   return "stopped";
        case AgentTraceType::AwaitingApproval:   return "approval";
        case AgentTraceType::Approved:           return "approved";
        case AgentTraceType::Denied:             return "denied";
        case AgentTraceType::AutoApproved:       return "auto-approved";
        case AgentTraceType::ToolStarted:        return "tool";
        case AgentTraceType::ToolSucceeded:      return "ok";
        case AgentTraceType::ToolFailed:         return "failed";
        case AgentTraceType::ToolSkipped:        return "skipped";
        case AgentTraceType::ToolBatchComplete:  return "tools done";
        case AgentTraceType::FollowUpRequested:  return "follow-up";
        case AgentTraceType::Completed:          return "completed";
        case AgentTraceType::Cancelled:          return "cancelled";
        case AgentTraceType::ProviderError:      return "error";
        default:                                 return "event";
    }
}

ImVec4 traceTypeColor(AgentTraceType type) {
    switch (type) {
        case AgentTraceType::ToolSucceeded:
        case AgentTraceType::ToolBatchComplete:
        case AgentTraceType::Completed:
            return ColorScheme::Success;
        case AgentTraceType::AwaitingApproval:
        case AgentTraceType::CallLimitTruncated:
        case AgentTraceType::AutoApproved:
        case AgentTraceType::FollowUpRequested:
            return ColorScheme::Warning;
        case AgentTraceType::Denied:
        case AgentTraceType::ToolFailed:
        case AgentTraceType::ToolSkipped:
        case AgentTraceType::StepLimitReached:
        case AgentTraceType::Cancelled:
        case AgentTraceType::ProviderError:
            return ColorScheme::Error;
        case AgentTraceType::Started:
        case AgentTraceType::ModelRequestDispatched:
        case AgentTraceType::Approved:
        case AgentTraceType::ToolStarted:
        default:
            return ColorScheme::Info;
    }
}

ImVec4 agentRunStateColor(AgentRunState state) {
    switch (state) {
        case AgentRunState::Completed:
            return ColorScheme::Success;
        case AgentRunState::WaitingApproval:
        case AgentRunState::ExecutingTools:
            return ColorScheme::Warning;
        case AgentRunState::Failed:
        case AgentRunState::Cancelled:
            return ColorScheme::Error;
        case AgentRunState::WaitingModel:
            return ColorScheme::Info;
        case AgentRunState::Idle:
        default:
            return ColorScheme::TextSecondary;
    }
}

void renderSelectableTextBlock(const char* id,
                               const std::string& text,
                               const ImVec4& color) {
    if (text.empty()) {
        return;
    }

    const float width = (std::max)(1.0f, ImGui::GetContentRegionAvail().x);
    const float wrapWidth = width;
    const ImVec2 textSize =
        ImGui::CalcTextSize(text.c_str(), nullptr, false, wrapWidth);
    const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
    const float height = (std::max)(lineHeight, textSize.y + 2.0f);

    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
    std::vector<char> buffer(text.begin(), text.end());
    buffer.push_back('\0');
    ImGui::InputTextMultiline(
        id,
        buffer.data(),
        buffer.size(),
        ImVec2(width, height),
        ImGuiInputTextFlags_ReadOnly |
            ImGuiInputTextFlags_WordWrap |
            ImGuiInputTextFlags_NoHorizontalScroll);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
}

} // namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

ChatWindow::ChatWindow()
    : agentController_(Mem::getSystemMemService()) {
    name = "AI Chat";
    cancelToken_ = makeCancellationToken();

    // Initialise the provider registry and tool registry. Both initBuiltin*
    // methods are idempotent (they replace existing entries) so calling
    // them from every ChatWindow instance is safe if the user closes and
    // reopens the window.
    ProviderRegistry::getInstance().initBuiltinProviders();
    ToolExecutor::getInstance().initBuiltinTools();

    // Load persisted API keys. Missing file is not an error (AC 10.6).
    // Providers supply non-secret endpoint/model defaults, but API keys
    // are never bundled; users must enter their own key in Settings.
    auto& keyStore = ApiKeyStore::getInstance();
    // One-time migration: if the user has an old `ai_config.dat` sitting
    // next to the exe but no `ai_config.json`, rename it in place so the
    // new default filename keeps their existing keys without a rewrite.
    {
        std::error_code mec;
        if (!std::filesystem::exists(kConfigFile, mec) &&
            std::filesystem::exists(kLegacyConfigFile, mec)) {
            std::filesystem::rename(kLegacyConfigFile, kConfigFile, mec);
            if (mec) {
                Gui::log("[AI Chat] migrated legacy config rename failed: %s",
                         mec.message().c_str());
            }
        }
    }
    keyStore.loadFromFile(kConfigFile);
    keyStore.seedDefaultsIfEmpty();
    keyStore.saveToFile(kConfigFile);

    // Load (or create with defaults) the plain-JSON user settings file.
    // This is separate from the DPAPI-encrypted key store so users can
    // hand-edit non-secret fields like the system prompt or proxy.
    auto& aiSettings = AiSettings::getInstance();
    aiSettings.loadOrDefault(kSettingsFile);
    AiSettingsData settingsSnapshot = aiSettings.get();

    // On a fresh install (or if the user cleared the field), seed the
    // AMem-specific default system prompt so the AI already knows what
    // the tool can do. Persisted immediately so subsequent runs and
    // manual edits to ai_settings.json see a concrete baseline rather
    // than an empty string.
    if (settingsSnapshot.systemPrompt.empty()) {
        settingsSnapshot.systemPrompt = kDefaultSystemPrompt;
        aiSettings.setSystemPrompt(settingsSnapshot.systemPrompt);
    }

    // Mirror persisted settings into the live singletons that consume them.
    ToolExecutor::getInstance().setExecutionTimeout(settingsSnapshot.executionTimeout);
    HttpClient::getInstance().setProxy(settingsSnapshot.proxy);
    session_.setTokenLimit(settingsSnapshot.tokenLimit);
    session_.setSystemPrompt(settingsSnapshot.systemPrompt);
    maxAgentSteps_ = settingsSnapshot.maxAgentSteps;
    maxToolCallsPerTurn_ = settingsSnapshot.maxToolCallsPerTurn;
    proxyEnabled_ = settingsSnapshot.proxy.enabled;
    proxyPort_    = settingsSnapshot.proxy.port;
    {
        const size_t n = std::min(settingsSnapshot.proxy.host.size(),
                                  sizeof(proxyHost_) - 1);
        if (n) std::memcpy(proxyHost_, settingsSnapshot.proxy.host.data(), n);
        proxyHost_[n] = '\0';
    }

    // Push every stored provider configuration into its live AIProvider
    // instance so they can start serving requests immediately.
    for (const std::string& providerName : keyStore.getConfiguredProviders()) {
        ProviderConfig cfg;
        if (!keyStore.loadConfig(providerName, cfg)) {
            continue;
        }
        if (AIProvider* provider = ProviderRegistry::getInstance().getProvider(providerName)) {
            if (cfg.baseUrl.empty()) {
                cfg.baseUrl = provider->getDefaultBaseUrl();
            }
            provider->configure(cfg);
        }
    }

    // Default selection: honour the persisted activeProvider setting
    // first, fall back to "openai", then to the first registered provider.
    const std::vector<std::string> providerNames =
        ProviderRegistry::getInstance().getProviderNames();
    auto has = [&](const std::string& n) {
        return std::find(providerNames.begin(), providerNames.end(), n) !=
               providerNames.end();
    };
    if (!settingsSnapshot.activeProvider.empty() &&
        has(settingsSnapshot.activeProvider)) {
        currentProvider_ = settingsSnapshot.activeProvider;
    } else if (has("openai")) {
        currentProvider_ = "openai";
    } else if (!providerNames.empty()) {
        currentProvider_ = providerNames.front();
    }
    if (!currentProvider_.empty()) {
        if (AIProvider* p = ProviderRegistry::getInstance().getProvider(currentProvider_)) {
            currentModel_ = p->getConfig().model;
        }
    }

    // Restore the prior conversation (AC 8.6). The SessionManager owns
    // the on-disk layout under kSessionsDir/; it migrates any legacy
    // ai_session.json into the new structure on first run, then tells us
    // which session id to load here. Brand-new installs get a fresh
    // empty session created on the fly.
    auto& sm = SessionManager::getInstance();
    sm.init(kSessionsDir, kSessionFile);
    activeSessionId_ = sm.activeId();
    if (activeSessionId_.empty()) {
        activeSessionId_ = sm.create();
    }
    const std::string sessionPath = sm.pathFor(activeSessionId_);
    if (!sessionPath.empty()) {
        session_.load(sessionPath);
    }
}

ChatWindow::~ChatWindow() {
    // Abort any in-flight request so the background HTTP worker exits
    // promptly instead of running to completion against a destroyed
    // window. The worker only inspects this flag between chunks, so a
    // brief delay is still possible; acceptable for a UI shutdown path.
    requestActiveRunCancellation();

    // Best-effort persist on shutdown. addMessage() already auto-persists
    // after every append, but saving again here captures any in-memory
    // state changes (e.g. a flushed tail) that predated a final write.
    const std::string path =
        SessionManager::getInstance().pathFor(activeSessionId_);
    if (!path.empty()) {
        session_.save(path);
    }
}

unsigned int ChatWindow::getWindowFlags() const {
    return ImGuiWindowFlags_NoDocking;
}

void ChatWindow::draw() {
    if (!pOpen) {
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    const bool oldMoveFromTitleBarOnly = io.ConfigWindowsMoveFromTitleBarOnly;
    io.ConfigWindowsMoveFromTitleBarOnly = true;

    ImGuiWindowFlags flags = static_cast<ImGuiWindowFlags>(getWindowFlags());
    if (shouldBringToFront) {
        ImGui::SetNextWindowFocus();
        shouldBringToFront = false;
    }

    if (ImGui::Begin(name.c_str(), &pOpen, flags)) {
        onDraw();
    }
    ImGui::End();

    io.ConfigWindowsMoveFromTitleBarOnly = oldMoveFromTitleBarOnly;
}

// ---------------------------------------------------------------------------
// Per-frame drawing
// ---------------------------------------------------------------------------

void ChatWindow::onDraw() {
    // Drain the producer-to-consumer queue first so any tokens that arrived
    // since the last frame are visible in the render performed below.
    pollMessages();

    drawToolbar();
    drawSessionControls();
    drawAgentActivityPanel();
    ImGui::Separator();
    drawMessageArea();
    drawInputArea();

    // Modal + settings popups draw last so they render on top of the main
    // window contents.
    drawToolConfirmationModal();
    if (showSettings_) {
        drawSettingsPanel();
    }
    if (showMutationAudit_) {
        drawMutationAuditPanel();
    }

    // Session management popups.
    if (showDeleteConfirm_) {
        ImGui::OpenPopup("##delete_session");
        showDeleteConfirm_ = false;
    }
    if (ImGui::BeginPopupModal("##delete_session", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        // Look up the title so the user sees what they're about to remove.
        std::string title = deleteConfirmId_;
        for (const auto& s : SessionManager::getInstance().list()) {
            if (s.id == deleteConfirmId_) { title = s.title; break; }
        }
        ImGui::Text("Delete session \"%s\"?", title.c_str());
        ImGui::Separator();
        ImGui::TextDisabled("This cannot be undone.");
        ImGui::Separator();
        if (ImGui::Button("Delete", ImVec2(120, 32))) {
            deleteSession(deleteConfirmId_);
            deleteConfirmId_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 32))) {
            deleteConfirmId_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (showRenameDialog_) {
        ImGui::OpenPopup("##rename_session");
        showRenameDialog_ = false;
    }
    if (ImGui::BeginPopupModal("##rename_session", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Rename session");
        ImGui::SetNextItemWidth(320.0f);
        ImGui::InputText("##rename_input", renameBuf_, sizeof(renameBuf_));
        ImGui::Separator();
        if (ImGui::Button("OK", ImVec2(120, 32))) {
            std::string newTitle = renameBuf_;
            if (!newTitle.empty() && !renameTargetId_.empty()) {
                SessionManager::getInstance().rename(renameTargetId_, newTitle);
            }
            renameTargetId_.clear();
            renameBuf_[0] = '\0';
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 32))) {
            renameTargetId_.clear();
            renameBuf_[0] = '\0';
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void ChatWindow::drawSessionControls() {
    auto& sm = SessionManager::getInstance();
    const auto sessions = sm.list();

    // Find the active session's title for the combo preview.
    std::string activeTitle = "(no session)";
    for (const auto& s : sessions) {
        if (s.id == activeSessionId_) { activeTitle = s.title; break; }
    }

    ImGui::SetNextItemWidth(260.0f);
    // Cap the popup size so a long title list doesn't make the dropdown
    // absurdly wide or tall. Width stays equal to the combo itself;
    // height is bounded so we fall back to a scroll bar instead.
    ImGui::SetNextWindowSizeConstraints(ImVec2(260.0f, 0.0f),
                                        ImVec2(260.0f, 320.0f));
    if (ImGui::BeginCombo("##session_picker", activeTitle.c_str())) {
        for (const auto& s : sessions) {
            const bool selected = (s.id == activeSessionId_);

            // Truncate very long titles so each row stays on one line.
            // The id suffix (after "##") keeps the label unique even
            // when two sessions share the same displayed prefix.
            constexpr size_t kMaxTitleChars = 34;
            std::string display = s.title;
            if (display.size() > kMaxTitleChars) {
                display.resize(kMaxTitleChars);
                display += "...";
            }

            // Inline the message count into the label; no SameLine(),
            // which was previously pushing the cursor past the popup's
            // content region and forcing it to grow horizontally.
            char label[192];
            std::snprintf(label, sizeof(label), "%s  (%d)##%s",
                          display.c_str(), s.messageCount, s.id.c_str());
            if (ImGui::Selectable(label, selected)) {
                if (s.id != activeSessionId_) {
                    switchToSession(s.id);
                }
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    if (ImGui::Button("New")) {
        createNewSession();
    }
    ImGui::SameLine();
    if (ImGui::Button("Rename")) {
        if (!activeSessionId_.empty()) {
            // Pre-fill the input with the current title.
            for (const auto& s : sessions) {
                if (s.id == activeSessionId_) {
                    const size_t n = std::min(s.title.size(),
                                              sizeof(renameBuf_) - 1);
                    if (n) std::memcpy(renameBuf_, s.title.data(), n);
                    renameBuf_[n] = '\0';
                    renameTargetId_ = s.id;
                    showRenameDialog_ = true;
                    break;
                }
            }
        }
    }
    ImGui::SameLine();
    // Deleting the only session leaves the user with nothing to chat
    // in; we handle that inside deleteSession() by auto-creating a
    // fresh one. Still disable the button when there is no active id.
    const bool canDelete = !activeSessionId_.empty();
    if (!canDelete) ImGui::BeginDisabled();
    if (ImGui::Button("Delete")) {
        deleteConfirmId_ = activeSessionId_;
        showDeleteConfirm_ = true;
    }
    if (!canDelete) ImGui::EndDisabled();
}

void ChatWindow::switchToSession(const std::string& id) {
    if (id.empty() || id == activeSessionId_) return;

    // Cancel any in-flight request so a late completion can't land on
    // the newly-loaded session and corrupt it.
    requestActiveRunCancellation();
    clearActiveRunContext();
    streamingContent_.clear();
    agentController_.resetForNewRun();
    requestStartMs_ = 0;
    state_ = State::Idle;

    // Persist whatever's currently in-memory to the old session's file
    // before we clobber the in-memory state.
    auto& sm = SessionManager::getInstance();
    if (!activeSessionId_.empty()) {
        const std::string oldPath = sm.pathFor(activeSessionId_);
        if (!oldPath.empty()) session_.save(oldPath);
    }

    activeSessionId_ = id;
    sm.setActiveId(id);

    // Replace the in-memory conversation with the selected session's
    // contents. Use resetInMemory() rather than clearHistory() so the
    // previous session's persisted file is NOT deleted as a side effect
    // of this switch. load() will rebind sessionFilePath_ to the new id.
    session_.resetInMemory();
    const std::string path = sm.pathFor(id);
    if (!path.empty()) session_.load(path);
}

void ChatWindow::createNewSession() {
    auto& sm = SessionManager::getInstance();
    // Persist current session before switching.
    if (!activeSessionId_.empty()) {
        const std::string oldPath = sm.pathFor(activeSessionId_);
        if (!oldPath.empty()) session_.save(oldPath);
    }

    const std::string id = sm.create();
    activeSessionId_ = id;

    // Clear in-memory state and bind session_ to the new path without
    // touching the previous session's file on disk.
    requestActiveRunCancellation();
    clearActiveRunContext();
    streamingContent_.clear();
    agentController_.resetForNewRun();
    requestStartMs_ = 0;
    state_ = State::Idle;
    session_.resetInMemory();
    const std::string path = sm.pathFor(id);
    session_.setSessionFilePath(path);
    // load() is unnecessary since the file doesn't exist yet; the path
    // binding alone ensures the next addMessage() persists to the right
    // location.
}

void ChatWindow::deleteSession(const std::string& id) {
    if (id.empty()) return;
    auto& sm = SessionManager::getInstance();
    const bool wasActive = (id == activeSessionId_);
    sm.remove(id);

    if (wasActive) {
        // Clear in-memory state before switching; if sessions remain the
        // manager already picked a new active id, otherwise create one.
        requestActiveRunCancellation();
        clearActiveRunContext();
        streamingContent_.clear();
        agentController_.resetForNewRun();
        requestStartMs_ = 0;
        state_ = State::Idle;
        session_.resetInMemory();

        std::string next = sm.activeId();
        if (next.empty()) {
            next = sm.create();
        }
        activeSessionId_ = next;
        const std::string nextPath = sm.pathFor(next);
        session_.setSessionFilePath(nextPath);
        session_.load(nextPath);
    }
}

void ChatWindow::touchActiveSession() {
    if (activeSessionId_.empty()) return;
    const auto& msgs = session_.getMessages();
    // First user message: used to auto-derive the session title on the
    // first send in a fresh chat.
    std::string firstUser;
    for (const auto& m : msgs) {
        if (m.role == Role::User) { firstUser = m.content; break; }
    }
    SessionManager::getInstance().touch(activeSessionId_,
                                        static_cast<int>(msgs.size()),
                                        firstUser);
}

void ChatWindow::drawToolbar() {
    auto& registry = ProviderRegistry::getInstance();
    const std::vector<std::string> providerNames = registry.getProviderNames();
    const bool runInProgress = state_ != State::Idle;

    // Provider selector.
    ImGui::SetNextItemWidth(160.0f);
    const char* preview =
        currentProvider_.empty() ? "<no provider>" : currentProvider_.c_str();
    if (runInProgress) {
        ImGui::BeginDisabled();
    }
    if (ImGui::BeginCombo("##chat_provider", preview)) {
        for (const auto& p : providerNames) {
            const bool selected = (p == currentProvider_);
            if (ImGui::Selectable(p.c_str(), selected)) {
                currentProvider_ = p;
                if (AIProvider* provider = registry.getProvider(p)) {
                    currentModel_ = provider->getConfig().model;
                }
                // Persist the user's selection so reopens remember it.
                AiSettings::getInstance().setActiveProvider(p);
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    if (runInProgress) {
        ImGui::EndDisabled();
    }

    ImGui::SameLine();

    // Model field: free-form text so the user can pick models not in the
    // built-in list without needing a settings-panel round-trip.
    ImGui::SetNextItemWidth(220.0f);
    char modelBuf[256] = {};
    {
        // Copy safely into a fixed-size buffer for ImGui::InputText. Avoid
        // std::strncpy because MSVC's CRT flags it deprecated; a manual
        // bounded copy sidesteps the warning.
        const size_t n = std::min(currentModel_.size(), sizeof(modelBuf) - 1);
        if (n) std::memcpy(modelBuf, currentModel_.data(), n);
        modelBuf[n] = '\0';
    }
    if (runInProgress) {
        ImGui::BeginDisabled();
    }
    if (ImGui::InputText("##chat_model", modelBuf, sizeof(modelBuf))) {
        currentModel_ = modelBuf;
        // Mirror the change into the live provider config so the next
        // sendMessage() picks it up.
        if (!currentProvider_.empty()) {
            if (AIProvider* provider = registry.getProvider(currentProvider_)) {
                ProviderConfig cfg = provider->getConfig();
                cfg.model = currentModel_;
                provider->configure(cfg);
            }
        }
    }
    if (runInProgress) {
        ImGui::EndDisabled();
    }

    ImGui::SameLine();
    if (ImGui::Button("Settings")) {
        showSettings_ = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Audit")) {
        showMutationAudit_ = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        clearHistory();
    }

    ImGui::SameLine();
    ImGui::TextDisabled("[%s]", stateLabel(this, static_cast<int>(state_)));

    // Live token usage. `estimateTokenCount()` is the same heuristic the
    // session uses when deciding whether to truncate. Showing it here
    // gives users an at-a-glance warning when they're approaching the
    // configured ceiling so they can wipe history or raise the limit.
    const int usedTokens = session_.estimateTokenCount();
    const int tokenLimit = session_.getTokenLimit();
    ImGui::SameLine();
    // Pick a colour based on fill level so the indicator doubles as a
    // subtle pressure gauge: green under 70 %, amber up to 90 %, red
    // beyond that (or if we've already crossed the limit).
    const float frac = tokenLimit > 0
                           ? static_cast<float>(usedTokens) / static_cast<float>(tokenLimit)
                           : 0.0f;
    ImVec4 tokenColor = ColorScheme::TextSecondary;
    if (frac >= 1.0f)       tokenColor = ColorScheme::ErrorBright;
    else if (frac >= 0.9f)  tokenColor = ColorScheme::Error;
    else if (frac >= 0.7f)  tokenColor = ColorScheme::Warning;
    else                    tokenColor = ColorScheme::Success;
    ImGui::PushStyleColor(ImGuiCol_Text, tokenColor);
    ImGui::Text("tokens: %d / %d (%.0f%%)", usedTokens, tokenLimit, frac * 100.0f);
    ImGui::PopStyleColor();

    // YOLO indicator. Shows up only when auto-approve is on so the user
    // can't forget they enabled it; bright red to match the warning in
    // the settings panel.
    if (AiSettings::getInstance().get().autoApproveWrites) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ColorScheme::ErrorBright);
        ImGui::TextUnformatted("[YOLO: writes auto-approved]");
        ImGui::PopStyleColor();
    }
}

void ChatWindow::drawAgentActivityPanel() {
    const AgentRunSnapshot snapshot = agentController_.snapshot();
    const auto& trace = snapshot.trace;
    if (trace.empty() && snapshot.state == AgentRunState::Idle) {
        return;
    }

    const AgentTraceEvent* latest = trace.empty() ? nullptr : &trace.back();
    ImGui::Spacing();
    ImGui::TextUnformatted("Agent");
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, agentRunStateColor(snapshot.state));
    ImGui::TextUnformatted(agentRunStateLabel(snapshot.state));
    ImGui::PopStyleColor();

    if (snapshot.modelTurns > 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("models %d", snapshot.modelTurns);
    }
    if (snapshot.toolSteps > 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("tools %d/%d", snapshot.toolSteps, maxAgentSteps_);
    }
    if (!snapshot.id.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", snapshot.id.c_str());
    }
    if (snapshot.pendingApproval) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ColorScheme::Warning);
        ImGui::Text("approval: %s", snapshot.pendingApproval->name.c_str());
        ImGui::PopStyleColor();
    } else if (latest && !latest->detail.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", latest->detail.c_str());
    }

    if (ImGui::BeginChild("##agent_activity", ImVec2(0.0f, 76.0f), true,
        ImGuiWindowFlags_HorizontalScrollbar)) {
        const size_t visible =
            trace.size() > 6 ? trace.size() - 6 : 0;
        for (size_t i = visible; i < trace.size(); ++i) {
            const AgentTraceEvent& ev = trace[i];
            const std::string ts = formatLocalTime(ev.timestamp);
            ImGui::TextDisabled("%s", ts.empty() ? "--:--:--" : ts.c_str());
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, traceTypeColor(ev.type));
            ImGui::TextUnformatted(traceTypeLabel(ev.type));
            ImGui::PopStyleColor();

            if (ev.step > 0) {
                ImGui::SameLine();
                ImGui::TextDisabled("s%d", ev.step);
            }
            if (ev.total > 0) {
                ImGui::SameLine();
                ImGui::TextDisabled("%d/%d", ev.index, ev.total);
            }
            if (!ev.tool.empty()) {
                ImGui::SameLine();
                ImGui::TextUnformatted(ev.tool.c_str());
            }
            if (ev.durationMs > 0) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", formatDuration(ev.durationMs).c_str());
            }
            if (!ev.detail.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", ev.detail.c_str());
            }
        }
    }
    ImGui::EndChild();
}

void ChatWindow::drawMutationAuditPanel() {
    ImGui::SetNextWindowSize(ImVec2(900.0f, 360.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Mutation Audit", &showMutationAudit_)) {
        ImGui::End();
        return;
    }

    AgentMutationAuditLog& audit = AgentMutationAuditLog::getInstance();
    const std::vector<AgentMutationAuditEntry> entries = audit.recent();
    ImGui::TextDisabled("%s", audit.filepath().c_str());
    ImGui::Separator();

    if (entries.empty()) {
        ImGui::TextDisabled("No mutation outcomes recorded");
        ImGui::End();
        return;
    }

    const ImGuiTableFlags flags =
        ImGuiTableFlags_BordersInnerV |
        ImGuiTableFlags_RowBg |
        ImGuiTableFlags_Resizable |
        ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("##mutation_audit", 8, flags,
                          ImVec2(0.0f, 0.0f))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed,
                                78.0f);
        ImGui::TableSetupColumn("Tool", ImGuiTableColumnFlags_WidthStretch,
                                1.2f);
        ImGui::TableSetupColumn("Effect", ImGuiTableColumnFlags_WidthStretch,
                                1.0f);
        ImGui::TableSetupColumn("Approval", ImGuiTableColumnFlags_WidthFixed,
                                92.0f);
        ImGui::TableSetupColumn("Target", ImGuiTableColumnFlags_WidthStretch,
                                1.0f);
        ImGui::TableSetupColumn("Completion",
                                ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("Duration",
                                ImGuiTableColumnFlags_WidthFixed, 72.0f);
        ImGui::TableSetupColumn("Error", ImGuiTableColumnFlags_WidthStretch,
                                1.4f);
        ImGui::TableHeadersRow();

        for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
            const AgentMutationAuditEntry& entry = *it;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            const std::string timestamp =
                formatLocalTime(entry.timestampMs / 1000);
            ImGui::TextUnformatted(
                timestamp.empty() ? "--:--:--" : timestamp.c_str());

            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(entry.tool.c_str());

            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(entry.effect.c_str());
            if (!entry.resourceDomain.empty() && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Resource: %s",
                                  entry.resourceDomain.c_str());
            }

            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(entry.approval.c_str());

            ImGui::TableSetColumnIndex(4);
            if (entry.target) {
                ImGui::Text("PID %d / r%llu",
                            entry.target->pid,
                            static_cast<unsigned long long>(
                                entry.target->processRevision));
            } else {
                ImGui::Text("gen %llu",
                            static_cast<unsigned long long>(
                                entry.connectionGeneration));
            }

            ImGui::TableSetColumnIndex(5);
            const bool uncertain =
                entry.completion == "completion_unknown" ||
                entry.completion == "cancel_requested" ||
                entry.completion == "completed_after_cancel_request" ||
                entry.completion == "completed_after_deadline";
            ImGui::PushStyleColor(
                ImGuiCol_Text,
                uncertain ? ColorScheme::Warning
                          : (entry.success ? ColorScheme::Success
                                           : ColorScheme::Error));
            ImGui::TextUnformatted(entry.completion.c_str());
            ImGui::PopStyleColor();

            ImGui::TableSetColumnIndex(6);
            ImGui::TextUnformatted(formatDuration(entry.durationMs).c_str());

            ImGui::TableSetColumnIndex(7);
            ImGui::TextUnformatted(entry.error.c_str());
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

void ChatWindow::drawInputArea() {
    AIProvider* activeProvider = nullptr;
    if (!currentProvider_.empty()) {
        activeProvider = ProviderRegistry::getInstance().getProvider(currentProvider_);
    }
    std::string configProblem;
    const bool hasProvider = activeProvider != nullptr;
    const bool providerReady =
        hasProvider && !getProviderConfigProblem(activeProvider, currentModel_, configProblem);

    // Show a warning line above the input when the user can't send yet.
    if (!providerReady) {
        ImGui::PushStyleColor(ImGuiCol_Text, ColorScheme::Warning);
        ImGui::TextUnformatted(configProblem.empty()
                                   ? "Configure a provider in Settings before sending"
                                   : configProblem.c_str());
        ImGui::PopStyleColor();
    }

    const ImVec2 inputSize(-100.0f, 80.0f);
    // Re-focus the composer on the frame right after a send so the user
    // can continue typing without clicking back in. Must be called
    // *before* the widget draws, per ImGui's SetKeyboardFocusHere API.
    if (refocusInput_) {
        ImGui::SetKeyboardFocusHere();
        refocusInput_ = false;
    }
    // Append inputGeneration_ to the widget id so that bumping the
    // counter in sendMessage() forces ImGui to treat this as a brand-
    // new widget. InputTextMultiline keeps an internal cached copy of
    // the buffer as long as the same widget id is active. Without a
    // fresh id, a plain memset on inputBuf_ is immediately overwritten
    // by that cache on the next frame, causing the "sent text stays in
    // the input box" bug.
    char inputId[48];
    std::snprintf(inputId, sizeof(inputId), "##chat_input_%u", inputGeneration_);
    ImGui::InputTextMultiline(inputId, inputBuf_, sizeof(inputBuf_), inputSize);
    const bool inputFocused = ImGui::IsItemFocused();

    ImGui::SameLine();
    ImGui::BeginGroup();

    // Send is enabled only when we have a provider and aren't already
    // waiting on one.
    const bool canSend = providerReady && state_ == State::Idle;
    if (!canSend) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Send", ImVec2(80.0f, 36.0f))) {
        sendMessage();
    }
    if (!canSend) {
        ImGui::EndDisabled();
    }

    // Stop cancels model streaming, a pending tool approval, or an in-flight
    // tool run. Once a tool has started we can't recall a side effect that
    // already reached the device, but the user still gets an escape hatch: the
    // run returns to Idle and the owned task receives a cancellation request.
    // A command already sent to the device may still complete; its late UI
    // result is isolated by runId while the worker remains joinable/tracked.
    if (state_ == State::WaitingResponse ||
        state_ == State::ToolConfirmation ||
        state_ == State::ToolExecuting) {
        if (ImGui::Button("Stop", ImVec2(80.0f, 36.0f))) {
            cancelRequest();
        }
    }
    ImGui::EndGroup();

    // Enter-to-submit: Enter without Shift sends, Shift+Enter inserts a
    // newline per the usual chat convention (AC 9.3).
    if (inputFocused && canSend) {
        const bool enterPressed =
            ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
            ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
        const bool shiftHeld =
            ImGui::IsKeyDown(ImGuiKey_LeftShift) ||
            ImGui::IsKeyDown(ImGuiKey_RightShift);
        if (enterPressed && !shiftHeld) {
            sendMessage();
        }
    }
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

void ChatWindow::sendMessage() {
    // Ignore empty / whitespace-only submissions (AC 9.10).
    std::string text = trimWhitespace(std::string(inputBuf_));
    if (text.empty()) {
        return;
    }

    // Refuse if no provider is configured (AC 9.9). The Send button is
    // usually already disabled in this case, but the keyboard shortcut
    // bypass means we double-check here.
    if (currentProvider_.empty()) {
        return;
    }
    AIProvider* provider =
        ProviderRegistry::getInstance().getProvider(currentProvider_);
    if (!provider) {
        return;
    }
    std::string configProblem;
    if (getProviderConfigProblem(provider, currentModel_, configProblem)) {
        ChatMessage errMsg;
        errMsg.role = Role::System;
        errMsg.content = std::string("[error] ") + configProblem;
        errMsg.timestamp = nowUnixSeconds();
        session_.addMessage(std::move(errMsg));
        touchActiveSession();
        showSettings_ = true;
        state_ = State::Idle;
        return;
    }

    // Append the user message, clear the composer, reset streaming state.
    ChatMessage userMsg;
    userMsg.role = Role::User;
    userMsg.content = std::move(text);
    userMsg.timestamp = nowUnixSeconds();
    session_.addMessage(userMsg);
    touchActiveSession();

    std::memset(inputBuf_, 0, sizeof(inputBuf_));
    // Bump the input widget generation so drawInputArea() renders with
    // a fresh ImGui id. InputTextMultiline caches an internal copy of
    // the buffer while its id is the active widget; without an id
    // change, that cache clobbers the memset above and the sent text
    // re-appears on the next frame.
    ++inputGeneration_;
    refocusInput_ = true;
    streamingContent_.clear();
    agentController_.resetForNewRun();
    activeRunProvider_ = currentProvider_;
    activeRunModel_ =
        currentModel_.empty() ? provider->getConfig().model : currentModel_;
    activeDispatchRunId_.clear();

    if (!dispatchAgentRequest(session_.getMessagesForRequest(),
                              "provider unavailable before dispatch")) {
        requestStartMs_ = 0;
        clearActiveRunContext();
        state_ = State::Idle;
    }
}

void ChatWindow::cancelRequest() {
    const bool mutationMayStillComplete =
        activeToolIsMutation_ && !activeToolRunId_.empty();
    requestActiveRunCancellation();
    activeDispatchRunId_.clear();

    // Preserve any partial streaming content as an assistant message so the
    // user doesn't lose what they've already seen (AC 12.7), then append a
    // system notice explaining the interruption (AC 9.6).
    if (!streamingContent_.empty()) {
        ChatMessage partial;
        partial.role = Role::Assistant;
        partial.content = streamingContent_;
        partial.timestamp = nowUnixSeconds();
        if (requestStartMs_ != 0) {
            partial.durationMs = nowSteadyMs() - requestStartMs_;
        }
        session_.addMessage(std::move(partial));
        touchActiveSession();
        streamingContent_.clear();
    }

    ChatMessage notice;
    notice.role = Role::System;
    notice.timestamp = nowUnixSeconds();
    notice.content = mutationMayStillComplete
        ? "[cancel requested] Active mutation may still complete; final status is recorded in Audit."
        : "[cancelled]";
    session_.addMessage(std::move(notice));
    touchActiveSession();

    requestStartMs_ = 0;
    agentController_.addTraceEvent(AgentTraceType::Cancelled, "request cancelled");
    agentController_.finishCancelled();
    clearActiveRunContext();
    state_ = State::Idle;
}

void ChatWindow::requestActiveRunCancellation() {
    if (cancelToken_) {
        cancelToken_->store(true, std::memory_order_release);
    }
    AgentTaskExecutor::getInstance().cancelRun(agentController_.runId());
}

void ChatWindow::clearActiveRunContext() {
    activeRunProvider_.clear();
    activeRunModel_.clear();
    activeDispatchRunId_.clear();
    activeToolRunId_.clear();
    activeToolIsMutation_ = false;
}

void ChatWindow::clearHistory() {
    // Cancel any in-flight request first so its completion callback
    // doesn't re-add a stale assistant message after the clear.
    requestActiveRunCancellation();
    clearActiveRunContext();
    streamingContent_.clear();
    agentController_.resetForNewRun();
    requestStartMs_ = 0;
    state_ = State::Idle;
    session_.clearHistory();
    touchActiveSession();
}

// ---------------------------------------------------------------------------
// Message pump
// ---------------------------------------------------------------------------

void ChatWindow::pollMessages() {
    UIMessage msg;
    while (UIMessageQueue::getInstance().tryPop(msg)) {
        if (!msg.auditPersisted && !msg.auditError.empty()) {
            Gui::log("[AI Chat] mutation audit persistence failed: %s",
                     msg.auditError.c_str());
        }
        if (!msg.runId.empty()) {
            const bool isToolResult = msg.type == UIMessageType::ToolResult;
            const std::string& expectedRunId =
                isToolResult ? activeToolRunId_ : activeDispatchRunId_;
            if (expectedRunId.empty() || msg.runId != expectedRunId) {
                continue;
            }
        }
        switch (msg.type) {
            case UIMessageType::Token: {
                streamingContent_.append(msg.data);
                break;
            }
            case UIMessageType::Completion: {
                const CompletionResponse& resp = msg.response;

                if (resp.error) {
                    // Categorised error handling (task 9.1, AC 12.1-12.8).
                    displayErrorForCategory(resp.error);
                    break;
                }

                // Success path: commit the assistant message. Prefer the
                // response's own content field. It's authoritative, but
                // fall back to the streamed accumulator when providers
                // return an empty content on the final completion (some
                // streaming endpoints do this).
                ChatMessage asstMsg = resp.message;
                if (asstMsg.content.empty() && !streamingContent_.empty()) {
                    asstMsg.content = streamingContent_;
                }
                asstMsg.role = Role::Assistant;
                asstMsg.timestamp = nowUnixSeconds();
                if (requestStartMs_ != 0) {
                    asstMsg.durationMs = nowSteadyMs() - requestStartMs_;
                }
                streamingContent_.clear();

                std::vector<ToolCall> calls = asstMsg.toolCalls;
                const std::string toolCallError = validateAndNormalizeToolCalls(calls);
                if (!toolCallError.empty()) {
                    if (!asstMsg.content.empty()) {
                        asstMsg.toolCalls.clear();
                        session_.addMessage(std::move(asstMsg));
                    }

                    ChatMessage errMsg;
                    errMsg.role = Role::System;
                    errMsg.content = "[error] Invalid tool call from AI: " + toolCallError;
                    errMsg.timestamp = nowUnixSeconds();
                    session_.addMessage(std::move(errMsg));
                    touchActiveSession();
                    Gui::log("[AI Chat] invalid tool call: %s", toolCallError.c_str());
                    streamingContent_.clear();
                    requestStartMs_ = 0;
                    activeDispatchRunId_.clear();
                    agentController_.addTraceEvent(AgentTraceType::ProviderError,
                                                   toolCallError);
                    agentController_.finishFailed();
                    clearActiveRunContext();
                    state_ = State::Idle;
                    break;
                }
                const bool hasToolCalls = !calls.empty();
                for (auto& call : calls) {
                    applyToolCallRedaction(call);
                }
                std::vector<ToolCall> persistedCalls = calls;
                const int maxCallsForHistory = std::clamp(maxToolCallsPerTurn_, 1, 64);
                if (static_cast<int>(persistedCalls.size()) > maxCallsForHistory) {
                    persistedCalls.resize(static_cast<size_t>(maxCallsForHistory));
                }
                asstMsg.toolCalls = std::move(persistedCalls);
                session_.addMessage(std::move(asstMsg));
                touchActiveSession();
                activeDispatchRunId_.clear();

                if (hasToolCalls) {
                    // Keep requestStartMs_ running; the follow-up
                    // completion after tool execution continues the same
                    // logical "AI turn" from the user's perspective.
                    processToolCalls(calls);
                } else {
                    requestStartMs_ = 0;
                    agentController_.addTraceEvent(
                        AgentTraceType::Completed,
                        "assistant response completed");
                    agentController_.finishCompleted();
                    clearActiveRunContext();
                    state_ = State::Idle;
                }
                break;
            }
            case UIMessageType::Error: {
                if (msg.response.error) {
                    if (streamingContent_.empty() && !msg.response.message.content.empty()) {
                        streamingContent_ = msg.response.message.content;
                    }
                    displayErrorForCategory(msg.response.error);
                    break;
                }

                ChatMessage errMsg;
                errMsg.role = Role::System;
                errMsg.content = std::string("[error] ") + msg.data;
                errMsg.timestamp = nowUnixSeconds();
                session_.addMessage(std::move(errMsg));
                touchActiveSession();
                Gui::log("[AI Chat] error: %s", msg.data.c_str());
                streamingContent_.clear();
                requestStartMs_ = 0;
                agentController_.addTraceEvent(AgentTraceType::ProviderError, msg.data);
                agentController_.finishFailed();
                clearActiveRunContext();
                state_ = State::Idle;
                break;
            }
            case UIMessageType::ToolResult: {
                activeToolRunId_.clear();
                activeToolIsMutation_ = false;
                handleAgentOutcome(agentController_.completeToolExecution(
                    msg.toolCall,
                    msg.toolResult,
                    msg.durationMs,
                    makeAgentConfig()));
                break;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Error routing: task 9.1 (Requirement 12)
// ---------------------------------------------------------------------------

void ChatWindow::displayErrorForCategory(const ProviderError& err) {
    // Preserve any partial streaming content as an assistant message before
    // the error notice so the user keeps what they already saw (AC 12.7).
    if (!streamingContent_.empty()) {
        ChatMessage partial;
        partial.role = Role::Assistant;
        partial.content = streamingContent_;
        partial.timestamp = nowUnixSeconds();
        if (requestStartMs_ != 0) {
            partial.durationMs = nowSteadyMs() - requestStartMs_;
        }
        session_.addMessage(std::move(partial));
        touchActiveSession();
    }

    // Format a category-specific user-facing message. The "[error]" prefix
    // is recognised by renderMessage() and rendered in ColorScheme::Error.
    std::string text;
    switch (err.category) {
        case ErrorCategory::RateLimit:
            text = "[error] Rate limit exceeded. Please wait a moment before retrying.";
            break;
        case ErrorCategory::Authentication:
            text = "[error] Authentication failed. Your API key appears invalid. "
                   "Open Settings to update it.";
            // Offer the settings panel directly (AC 12.2).
            showSettings_ = true;
            break;
        case ErrorCategory::Network:
            text = std::string("[error] Network connection failed: ") + err.message;
            break;
        case ErrorCategory::Timeout:
            text = std::string("[error] Request timed out: ") + err.message;
            break;
        case ErrorCategory::InvalidResponse:
            text = std::string("[error] Invalid response from AI: ") + err.message;
            break;
        case ErrorCategory::Cancelled:
            text = "[error] Request cancelled";
            break;
        case ErrorCategory::Unknown:
        case ErrorCategory::None:
        default: {
            // HTTP 5xx or any unrecognised non-2xx status gets a status-code
            // carrying message (AC 12.3). For everything else, surface the
            // raw provider message.
            if (err.httpStatusCode >= 500 && err.httpStatusCode < 600) {
                text = "[error] Server error (HTTP " +
                       std::to_string(err.httpStatusCode) + "): " + err.message;
            } else if (err.httpStatusCode != 0 &&
                       (err.httpStatusCode < 200 || err.httpStatusCode >= 300)) {
                text = "[error] Unexpected HTTP status " +
                       std::to_string(err.httpStatusCode) + ": " + err.message;
            } else {
                text = std::string("[error] ") + err.message;
            }
            break;
        }
    }

    ChatMessage errMsg;
    errMsg.role = Role::System;
    errMsg.content = std::move(text);
    errMsg.timestamp = nowUnixSeconds();
    session_.addMessage(std::move(errMsg));
    touchActiveSession();

    // Log every error (AC 12.8).
    Gui::log("[AI Chat] error (category=%d): %s",
             static_cast<int>(err.category), err.message.c_str());
    agentController_.addTraceEvent(AgentTraceType::ProviderError, err.message);

    // Restore idle state so the input field re-enables and the loading
    // indicator disappears (AC 12.6).
    streamingContent_.clear();
    requestStartMs_ = 0;
    agentController_.finishFailed();
    clearActiveRunContext();
    state_ = State::Idle;
}

// ---------------------------------------------------------------------------
// Message rendering: task 8.2
// ---------------------------------------------------------------------------

void ChatWindow::drawMessageArea() {
    // Reserve room at the bottom for the input area + toolbar padding.
    ImGui::BeginChild("##chat_messages", ImVec2(0.0f, -100.0f), true,
                      ImGuiWindowFlags_HorizontalScrollbar);

    // Track whether the user has scrolled away from the bottom. Once
    // they do, we stop force-scrolling on new content until they return
    // to the bottom themselves (AC 9.8).
    const float scrollY = ImGui::GetScrollY();
    const float maxScrollY = ImGui::GetScrollMaxY();
    scrollY_ = scrollY;
    maxScrollY_ = maxScrollY;
    if (maxScrollY > 0.0f && scrollY < maxScrollY - 10.0f) {
        userScrolledUp_ = true;
    } else {
        userScrolledUp_ = false;
    }

    // Cap the visible history at 200 messages (AC 9.2). Older messages
    // stay in session_ (which keeps up to 1000) for persistence and for
    // the next request payload; we just don't render them all every frame.
    const auto& msgs = session_.getMessages();
    constexpr size_t kMaxVisible = 200;
    const size_t renderStart =
        msgs.size() > kMaxVisible ? msgs.size() - kMaxVisible : 0;
    for (size_t i = renderStart; i < msgs.size(); ++i) {
        renderMessage(msgs[i], static_cast<int>(i));
        // Separator between messages for readability (AC 9.2).
        if (i + 1 < msgs.size()) {
            ImGui::Separator();
        }
    }
    if (!streamingContent_.empty()) {
        if (!msgs.empty()) {
            ImGui::Separator();
        }
        renderStreamingMessage();
    }
    if (state_ == State::WaitingResponse) {
        renderLoadingIndicator();
    }

    // Auto-scroll to bottom when new content arrived this frame AND the
    // user hasn't manually scrolled up. "New content" means either the
    // message count grew or the streaming buffer extended.
    const bool grewMessages = msgs.size() != lastRenderedMessageCount_;
    const bool grewStreaming = streamingContent_.size() != lastStreamingContentLen_;
    if (!userScrolledUp_ && (grewMessages || grewStreaming)) {
        ImGui::SetScrollHereY(1.0f);
    }
    lastRenderedMessageCount_ = msgs.size();
    lastStreamingContentLen_ = streamingContent_.size();

    ImGui::EndChild();
}

void ChatWindow::renderMessage(const ChatMessage& msg, int index) {
    ImGui::PushID(index);
    const char* roleStr = "";
    ImVec4 color = ColorScheme::TextPrimary;

    // Tool-role messages that carry an error payload should render in the
    // error color so denials / failures stand out (AC 12.5). Same for
    // system messages that start with the "[error]" marker we use for
    // error notices surfaced in pollMessages().
    const bool isErrorToolResult =
        msg.role == Role::Tool &&
        msg.content.find("\"error\"") != std::string::npos;
    const bool isErrorSystem =
        msg.role == Role::System &&
        msg.content.rfind("[error]", 0) == 0;

    switch (msg.role) {
        case Role::User:
            roleStr = "You";
            color = ColorScheme::Info;
            break;
        case Role::Assistant:
            roleStr = "AI";
            color = ColorScheme::TextPrimary;
            break;
        case Role::Tool:
            roleStr = "[Tool]";
            color = isErrorToolResult ? ColorScheme::Error
                                      : ColorScheme::TextSecondary;
            break;
        case Role::System:
            roleStr = "[sys]";
            color = isErrorSystem ? ColorScheme::Error : ColorScheme::Warning;
            break;
    }

    // Header line: role, timestamp, and duration for AI messages.
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(roleStr);
    ImGui::PopStyleColor();

    const std::string ts = formatLocalTime(msg.timestamp);
    if (!ts.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("· %s", ts.c_str());
    }
    if (msg.role == Role::Assistant && msg.durationMs > 0) {
        const std::string dur = formatDuration(msg.durationMs);
        if (!dur.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("· took %s", dur.c_str());
        }
    }

    // Body.
    renderSelectableTextBlock("##message_body", msg.content, color);

    for (const auto& tc : msg.toolCalls) {
        ImGui::PushStyleColor(ImGuiCol_Text, ColorScheme::TextSecondary);
        ImGui::BulletText("tool_call: %s(%s)", tc.name.c_str(),
                          toolCallArgumentsForDisplay(tc).c_str());
        ImGui::PopStyleColor();
    }
    ImGui::PopID();
}

void ChatWindow::renderStreamingMessage() {
    // Header: "AI · HH:MM:SS · streaming for Xs"
    ImGui::PushStyleColor(ImGuiCol_Text, ColorScheme::TextPrimary);
    ImGui::TextUnformatted("AI");
    ImGui::PopStyleColor();

    const std::string ts = formatLocalTime(nowUnixSeconds());
    if (!ts.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("· %s", ts.c_str());
    }
    if (requestStartMs_ != 0) {
        const long long elapsed = nowSteadyMs() - requestStartMs_;
        const std::string dur = formatDuration(elapsed);
        if (!dur.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("· streaming for %s", dur.c_str());
        }
    }

    renderSelectableTextBlock("##streaming_body",
                              streamingContent_,
                              ColorScheme::TextPrimary);
}

void ChatWindow::renderLoadingIndicator() {
    // Animated cycling dots at roughly 500ms per step (AC 9.5). ImGui
    // typically runs at ~60 FPS, so advancing every 30 frames puts us at
    // 4 states per two seconds ("", ".", "..", "...").
    static const char* dots[] = {"", ".", "..", "..."};
    // When a request is in flight, include a live elapsed counter so the
    // user can see the request isn't stuck.
    if (requestStartMs_ != 0) {
        const long long elapsed = nowSteadyMs() - requestStartMs_;
        const std::string dur = formatDuration(elapsed);
        ImGui::TextDisabled("generating%s  (%s)",
                            dots[(animFrame_ / 30) % 4],
                            dur.empty() ? "0ms" : dur.c_str());
    } else {
        ImGui::TextDisabled("generating%s", dots[(animFrame_ / 30) % 4]);
    }
    ++animFrame_;
}

// ---------------------------------------------------------------------------
// Tool-confirmation + tool-execution flow: task 8.3
// ---------------------------------------------------------------------------

void ChatWindow::drawToolConfirmationModal() {
    if (state_ != State::ToolConfirmation) {
        return;
    }
    ImGui::OpenPopup("##tool_confirm");
    if (ImGui::BeginPopupModal("##tool_confirm", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        const ToolCall* pending = agentController_.pendingApproval();
        if (pending) {
            ImGui::Text("AI wants to execute tool: %s", pending->name.c_str());
            const AgentRunSnapshot approvalSnapshot = agentController_.snapshot();
            const Mem::OperationContext& expected =
                approvalSnapshot.context.operation;
            ImGui::TextDisabled(
                "Expected connection generation: %llu",
                static_cast<unsigned long long>(
                    expected.connectionGeneration));
            if (expected.target) {
                ImGui::TextDisabled(
                    "Expected target: PID %d, revision %llu",
                    expected.target->pid,
                    static_cast<unsigned long long>(
                        expected.target->processRevision));
            }
            ImGui::Separator();

            // Pretty-print the arguments JSON when possible so the user can
            // actually read what they're approving (AC 6.3). Fall back to
            // the raw string if the arguments aren't valid JSON, which can
            // happen for malformed tool calls we still want to surface.
            const std::string& displayArguments =
                toolCallArgumentsForDisplay(*pending);
            std::string prettyArgs = displayArguments;
            try {
                if (!displayArguments.empty()) {
                    const auto parsed = nlohmann::json::parse(displayArguments);
                    prettyArgs = parsed.dump(2);
                }
            } catch (const nlohmann::json::exception&) {
                // keep raw
            }
            ImGui::TextUnformatted("Arguments:");
            ImGui::PushStyleColor(ImGuiCol_Text, ColorScheme::TextSecondary);
            ImGui::TextWrapped("%s", prettyArgs.c_str());
            ImGui::PopStyleColor();

            ImGui::Separator();
            if (ImGui::Button("Approve")) {
                ImGui::CloseCurrentPopup();
                state_ = State::ToolExecuting;
                handleAgentOutcome(agentController_.approvePendingTool(makeAgentConfig()));
            }
            ImGui::SameLine();
            if (ImGui::Button("Deny")) {
                ToolResult deniedResult;
                deniedResult.success = false;
                deniedResult.errorMessage = "tool execution denied by user";
                deniedResult.resultJson =
                    R"({"success":false,"error":{"code":"permission_denied","message":"tool execution denied by user","retryable":false},"completion":"rejected_before_start"})";
                deniedResult.completion =
                    ToolCompletionState::RejectedBeforeStart;
                std::string auditError;
                if (!persistMutationOutcome(
                        agentController_.runId(),
                        *pending,
                        deniedResult,
                        agentController_.operationContext(),
                        MutationApproval::Denied,
                        0,
                        auditError)) {
                    Gui::log(
                        "[AI Chat] mutation audit persistence failed: %s",
                        auditError.c_str());
                }
                ImGui::CloseCurrentPopup();
                state_ = State::ToolExecuting;
                handleAgentOutcome(agentController_.denyPendingTool(makeAgentConfig()));
            }
        } else {
            agentController_.addTraceEvent(AgentTraceType::ProviderError,
                                           "pending tool confirmation disappeared");
            agentController_.finishFailed();
            clearActiveRunContext();
            // Pending call went away; recover gracefully.
            state_ = State::Idle;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void ChatWindow::processToolCalls(const std::vector<ToolCall>& calls) {
    state_ = State::ToolExecuting;
    handleAgentOutcome(agentController_.beginToolCalls(calls, makeAgentConfig()));
}

void ChatWindow::startToolExecution(const ToolCall& call) {
    activeToolRunId_ = agentController_.runId();
    activeToolIsMutation_ =
        ToolExecutor::getInstance().getToolSafety(call.name) ==
        ToolSafety::Write;
    const std::string runId = activeToolRunId_;
    const Mem::OperationContext operationContext =
        agentController_.operationContext();

    AgentToolTask task;
    task.runId = runId;
    task.call = call;
    task.context = operationContext;
    MutationApproval approval = MutationApproval::NotRequired;
    if (ToolExecutor::getInstance().getToolSafety(call.name) ==
        ToolSafety::Write) {
        approval =
            AiSettings::getInstance().get().autoApproveWrites
                ? MutationApproval::AutoApproved
                : MutationApproval::Approved;
        task.approval = approval;
    }
    const bool queued = AgentTaskExecutor::getInstance().enqueue(
        std::move(task), [](AgentToolTaskOutcome outcome) {
        UIMessage msg;
        msg.type = UIMessageType::ToolResult;
        msg.runId = std::move(outcome.runId);
        msg.toolCall = std::move(outcome.call);
        msg.toolResult = std::move(outcome.result);
        msg.durationMs = outcome.durationMs;
        msg.auditPersisted = outcome.auditPersisted;
        msg.auditError = std::move(outcome.auditError);
        UIMessageQueue::getInstance().push(std::move(msg));
    });

    if (!queued) {
        UIMessage msg;
        msg.type = UIMessageType::ToolResult;
        msg.runId = runId;
        msg.toolCall = call;
        msg.toolResult.success = false;
        msg.toolResult.errorMessage =
            "Tool task queue is shutting down or full";
        msg.toolResult.resultJson =
            R"({"success":false,"error":{"code":"internal_error","message":"tool task queue is shutting down or full","retryable":false},"completion":"cancelled_before_start"})";
        msg.toolResult.completion =
            ToolCompletionState::CancelledBeforeStart;
        msg.auditPersisted = persistMutationOutcome(
            runId,
            call,
            msg.toolResult,
            operationContext,
            approval,
            0,
            msg.auditError);
        UIMessageQueue::getInstance().push(std::move(msg));
    }
}

void ChatWindow::handleAgentOutcome(AgentController::ToolOutcome outcome) {
    for (const std::string& line : outcome.logs) {
        Gui::log("%s", line.c_str());
    }
    for (auto& msg : outcome.messages) {
        session_.addMessage(std::move(msg));
        touchActiveSession();
    }

    switch (outcome.kind) {
        case AgentController::ToolOutcomeKind::NeedsConfirmation:
            if (outcome.pendingToolCall) {
                state_ = State::ToolConfirmation;
            } else {
                agentController_.finishFailed();
                clearActiveRunContext();
                state_ = State::Idle;
            }
            break;
        case AgentController::ToolOutcomeKind::NeedsExecution:
            if (outcome.toolCallToExecute) {
                startToolExecution(*outcome.toolCallToExecute);
            } else {
                agentController_.finishFailed();
                clearActiveRunContext();
                state_ = State::Idle;
            }
            break;
        case AgentController::ToolOutcomeKind::ReadyForFollowUp:
            sendFollowUpAfterTools();
            break;
        case AgentController::ToolOutcomeKind::Stopped:
        case AgentController::ToolOutcomeKind::Idle:
        default:
            requestStartMs_ = 0;
            agentController_.finishFailed();
            clearActiveRunContext();
            state_ = State::Idle;
            break;
    }
}

void ChatWindow::sendFollowUpAfterTools() {
    if (dispatchAgentRequest(session_.getMessagesForRequest(),
                             "active provider unavailable for tool follow-up")) {
        agentController_.addTraceEvent(AgentTraceType::FollowUpRequested,
                                       "tool results sent back to model");
        return;
    }

    requestStartMs_ = 0;
    agentController_.finishFailed();
    clearActiveRunContext();
    state_ = State::Idle;
}

bool ChatWindow::dispatchAgentRequest(const std::vector<ChatMessage>& messages,
                                      const char* failureDetail) {
    const std::string providerName =
        activeRunProvider_.empty() ? currentProvider_ : activeRunProvider_;
    const std::string modelName =
        activeRunModel_.empty() ? currentModel_ : activeRunModel_;

    AgentController::ModelRequest request;
    request.providerName = providerName;
    request.modelOverride = modelName;
    request.failureDetail = failureDetail ? failureDetail : "";
    request.messages = messages;
    request.stream = true;

    streamingContent_.clear();
    activeToolRunId_.clear();
    if (cancelToken_) {
        cancelToken_->store(true);
    }
    cancelToken_ = makeCancellationToken();
    requestStartMs_ = nowSteadyMs();
    AgentController::DispatchResult result =
        agentController_.dispatchModelRequest(request, cancelToken_);

    if (result.dispatched) {
        activeDispatchRunId_ = agentController_.runId();
        state_ = State::WaitingResponse;
        return true;
    }
    activeDispatchRunId_.clear();

    const std::string detail =
        !result.error.empty()
            ? result.error
            : (failureDetail && *failureDetail ? std::string(failureDetail)
                                               : std::string("agent dispatch failed"));
    Gui::log("[AI Chat] agent dispatch failed: %s", detail.c_str());

    ChatMessage errMsg;
    errMsg.role = Role::System;
    errMsg.content = std::string("[error] ") + detail;
    errMsg.timestamp = nowUnixSeconds();
    session_.addMessage(std::move(errMsg));
    touchActiveSession();
    return false;
}

AgentController::ToolConfig ChatWindow::makeAgentConfig() const {
    AgentController::ToolConfig cfg;
    cfg.maxAgentSteps = maxAgentSteps_;
    cfg.maxToolCallsPerTurn = maxToolCallsPerTurn_;
    cfg.autoApproveWrites = AiSettings::getInstance().get().autoApproveWrites;
    return cfg;
}

// drawSettingsPanel / drawProviderSettings / validateSettings are defined in ChatWindowSettings.cpp (task 8.4)

} // namespace AI

#endif // HAVE_AI_CHAT
