#include "LogWindow.h"
#include "Gui.h"
#include "../imgui/imgui.h"
#include <string>

LogWindow::LogWindow() {
    name = "Logs";
}

void LogWindow::onDraw() {
    if (ImGui::Begin(name.c_str(), &pOpen)) {
        auto& logs = Gui::logs;
        if (logs.empty()) {
            ImGui::TextDisabled("暂无日志");
        } else {
            for (const auto& [msg, dup] : logs) {
                if (dup > 0)
                    ImGui::TextUnformatted((msg + "  (x" + std::to_string(dup + 1) + ")").c_str());
                else
                    ImGui::TextUnformatted(msg.c_str());
            }
        }
    }
    ImGui::End();
}

