#include "ProcessListWindow.h"
#include "../imgui/imgui.h"
#include "../socket/client_singleton.h"
#include "Gui.h"

ProcessListWindow::ProcessListWindow()
{
	name = "Process List";
}

unsigned int ProcessListWindow::getWindowFlags() const
{
    return ImGuiWindowFlags_NoDocking;
}

void ProcessListWindow::onDraw()
{
	if (ImGui::Button("Refresh"))
	{
		std::vector<ProcessInfoItem> list;
		if (FetchProcessList(list)) {
			hasData = true;
			processes.clear();
			processes.reserve(list.size());
			for (auto& it : list)
				processes.emplace_back(it.pid, std::move(it.name));
			Gui::log("Fetched %d processes", (int)processes.size());
		} else {
			hasData = false;
			Gui::log("Failed to fetch process list");
		}
	}

	if (hasData) {
		if (ImGui::BeginTable("proc", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY)) {
			ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_WidthFixed, 100.0f);
			ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableHeadersRow();
			for (auto& [pid, name] : processes) {
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::Text("%d", pid);
				ImGui::TableSetColumnIndex(1);
				ImGui::TextUnformatted(name.c_str());
			}
			ImGui::EndTable();
		}
	}
} 