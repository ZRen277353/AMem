#include "ProcessListWindow.h"
#include "../imgui/imgui.h"
#include "../mem/IMemService.h"
#include "Gui.h"

ProcessListWindow::ProcessListWindow(Mem::IMemService& memService)
	: memService_(memService)
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
		std::vector<Mem::ProcessInfo> list;
		const Mem::OperationContext context = memService_.captureContext(false);
		bool success = true;
		size_t offset = 0;
		while (success) {
			Mem::ProcessListRequest request;
			request.offset = offset;
			request.limit = Mem::kMaxProcessPageSize;
			auto response = memService_.listProcesses(context, request);
			if (!response.ok()) {
				Gui::log("Failed to fetch process list [%s]: %s",
				         Mem::errorCodeName(response.error().code),
				         response.error().message.c_str());
				success = false;
				break;
			}
			const auto& page = response.value();
			list.insert(list.end(), page.items.begin(), page.items.end());
			if (!page.nextOffset) break;
			offset = *page.nextOffset;
		}
		if (success) {
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
