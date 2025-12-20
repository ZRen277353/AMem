#include "VersionWindow.h"
#include "../imgui/imgui.h"
#include "../socket/client_singleton.h"
#include "Gui.h"
#include <version.h>  // CMake 生成的版本信息

VersionWindow::VersionWindow()
{
	name = "版本信息";
}

unsigned int VersionWindow::getWindowFlags() const
{
    return ImGuiWindowFlags_NoDocking;
}

void VersionWindow::onDraw()
{
	// ==================== 客户端版本信息 ====================
	ImGui::SeparatorText("客户端版本");
	ImGui::Text("程序版本: %s", PROJECT_VERSION);
	ImGui::Text("协议版本: %s", PROTOCOL_VERSION);
	ImGui::Text("构建日期: %s", BUILD_DATE);
	ImGui::Text("构建时间: %s", BUILD_TIME);
	ImGui::Text("Git 提交: %s", GIT_COMMIT_HASH);
	ImGui::Text("Git 分支: %s", GIT_BRANCH);
	
	ImGui::Spacing();
	ImGui::Separator();
	
	// ==================== 服务端版本信息 ====================
	ImGui::SeparatorText("服务端版本");
	
	if (ImGui::Button("获取服务端版本"))
	{
		ServerVersionInfo info{};
		if (FetchServerVersion(info)) {
			hasData = true;
			version = info.version;
			versionString = info.versionString;
			Gui::log("服务端版本: %d (%s)", version, versionString.c_str());
		} else {
			hasData = false;
			Gui::log("获取服务端版本失败");
		}
	}

	if (hasData) {
		ImGui::Text("版本号: %d", version);
		ImGui::Text("版本字符串: %s", versionString.c_str());
		
		// 简单的版本兼容性提示
		int serverProtocolMinor = version / 100;
		ImGui::Spacing();
		if (serverProtocolMinor < PROTOCOL_VERSION_MINOR) {
			ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), 
				"⚠ 服务端协议版本较旧，部分功能可能不可用");
		} else if (serverProtocolMinor == PROTOCOL_VERSION_MINOR) {
			ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), 
				"✓ 协议版本匹配");
		} else {
			ImGui::TextColored(ImVec4(0.0f, 0.5f, 1.0f, 1.0f), 
				"服务端支持更新的协议版本");
		}
	} else {
		ImGui::TextDisabled("未连接或未获取版本信息");
	}
}