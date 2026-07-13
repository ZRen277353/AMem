#include "VersionWindow.h"
#include "ColorScheme.h"
#include "../imgui/imgui.h"
#include "../mem/IMemService.h"
#include "Gui.h"
#include <version.h>  // CMake 生成的版本信息

VersionWindow::VersionWindow(Mem::IMemService& memService)
	: memService_(memService)
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
		auto response = memService_.status(memService_.captureContext(false));
		if (response.ok() && response.value().serverVersion) {
			hasData = true;
			version = *response.value().serverVersion;
			versionString = response.value().serverVersionString;
			Gui::log("服务端版本: %d (%s)", version, versionString.c_str());
		} else {
			hasData = false;
			if (response.ok()) {
				Gui::log("服务器未返回版本信息");
			} else {
				Gui::log("获取服务端版本失败 [%s]: %s",
				         Mem::errorCodeName(response.error().code),
				         response.error().message.c_str());
			}
		}
	}

	if (hasData) {
		ImGui::Text("版本号: %d", version);
		ImGui::Text("版本字符串: %s", versionString.c_str());
		
		// 简单的版本兼容性提示
		int serverProtocolMinor = version / 100;
		ImGui::Spacing();
		if (serverProtocolMinor < PROTOCOL_VERSION_MINOR) {
			ImGui::TextColored(ColorScheme::Warning, 
				"⚠ 服务端协议版本较旧，部分功能可能不可用");
		} else if (serverProtocolMinor == PROTOCOL_VERSION_MINOR) {
			ImGui::TextColored(ColorScheme::Success, 
				"✓ 协议版本匹配");
		} else {
			ImGui::TextColored(ColorScheme::InfoBright, 
				"服务端支持更新的协议版本");
		}
	} else {
		ImGui::TextDisabled("未连接或未获取版本信息");
	}
}
