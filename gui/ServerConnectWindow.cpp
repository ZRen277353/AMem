#include "ServerConnectWindow.h"
#include "ColorScheme.h"
#include "../imgui/imgui.h"
#include "../mem/IMemService.h"
#include "Gui.h"
#include "version.h"
#include "ConfigManager.h"

namespace {
bool isValidPort(int port)
{
	return port > 0 && port <= 65533;
}

const char* getMemTypeName(const std::string (&names)[5], int memType)
{
	return (memType >= 0 && memType < 5) ? names[memType].c_str() : "Unknown";
}
} // namespace

ServerConnectWindow::ServerConnectWindow(Mem::IMemService& memService)
	: memService_(memService)
{
	name = "服务器连接";
	std::snprintf(hostBuf, sizeof(hostBuf), "%s", "127.0.0.1");
	port = 52736;
	autoReconnect = false;
	status = "空闲";
	
	// 初始化新增成员变量
	currentMemType = 0;
	//std::snprintf(cardKeyBuf, sizeof(cardKeyBuf), "%s", "");
	driverStatus = "未初始化";
	memTypeNames[0] = "空";
	memTypeNames[1] = "IO";
	memTypeNames[2] = "系统调用";
	memTypeNames[3] = "内核";
	memTypeNames[4] = "系统钩子";
	
	// 加载配置
	loadConfig();
	
	updateMemType();
}

void ServerConnectWindow::updateStatus(bool ok, const char* action)
{
	if (ok) {
		auto response = memService_.status(memService_.captureContext(true));
		if (response.ok() && response.value().serverVersion) {
			const auto& server = response.value();
			status = std::string(action) + ": 已连接到 " + hostBuf + ":" + std::to_string(port);
			Gui::log("%s", status.c_str());
			Gui::log("服务器版本: %d", *server.serverVersion);
			Gui::log("服务器版本字符串: %s", server.serverVersionString.c_str());
			status = status  +"\n" + " (版本: " + server.serverVersionString + ")";
			
			// 连接成功后更新MemType
			updateMemType();
		} else {
			status = std::string(action) + ": 失败 -> 未知服务器";
			if (response.ok()) {
				Gui::log("获取服务器版本失败，服务端可能不兼容");
			} else {
				Gui::log("获取服务器状态失败 [%s]: %s",
				         Mem::errorCodeName(response.error().code),
				         response.error().message.c_str());
			}
		}
	} else {
		status = std::string(action) + ": 失败";
		Gui::log("%s", status.c_str());
	}
}

void ServerConnectWindow::updateMemType()
{
	if (isConnected()) {
		auto response = memService_.status(memService_.captureContext(true));
		if (response.ok() && response.value().architectureType) {
			currentMemType = *response.value().architectureType;
			Gui::log("当前内存类型: %s (%d)", getMemTypeName(memTypeNames, currentMemType), currentMemType);
		} else {
			if (!response.ok()) {
				Gui::log("获取内存类型失败 [%s]: %s",
				         Mem::errorCodeName(response.error().code),
				         response.error().message.c_str());
			} else {
				Gui::log("服务器未返回内存类型");
			}
			currentMemType = 0;
		}
	}
}

bool ServerConnectWindow::isConnected() const
{
	return memService_.connectionSnapshot().connected;
}

void ServerConnectWindow::initializeDriver()
{
	std::string cardKey = std::string(cardKeyBuf);
	std::string kernelVersion = std::string(1, KernelVersionBuf);
	
	if (cardKey.empty()) {
		driverStatus = "初始化失败: 卡密不能为空";
		Gui::log("驱动初始化失败: 卡密不能为空");
		return;
	}
	
	Gui::log("正在初始化驱动");
	cardKey = cardKey +"-"+ kernelVersion;
	Mem::DriverInitializeRequest request;
	request.card = cardKey;
	auto response = memService_.initializeDriver(
		memService_.captureContext(false), request);
	if (response.ok()) {
		driverStatus = "初始化成功: " + response.value().message;
		Gui::log("驱动初始化成功: %s", response.value().message.c_str());
		// 初始化成功后更新MemType
		updateMemType();
	} else {
		driverStatus = "初始化失败: " + response.error().message;
		Gui::log("驱动初始化失败 [%s]: %s",
		         Mem::errorCodeName(response.error().code),
		         response.error().message.c_str());
	}
}

void ServerConnectWindow::drawConnectionControls() {
  // ==================== 客户端版本信息 ====================
  ImGui::SeparatorText("客户端版本");
  ImGui::Text("程序版本: %s", PROJECT_VERSION);
  ImGui::Text("协议版本: %s", PROTOCOL_VERSION);
  ImGui::Text("构建日期: %s  构建时间: %s", BUILD_DATE, BUILD_TIME);
  ImGui::Text("Git 提交: %s  分支: %s", GIT_COMMIT_HASH, GIT_BRANCH);
  ImGui::Spacing();
  ImGui::Separator();

  ImGui::InputText("主机", hostBuf, IM_ARRAYSIZE(hostBuf));
  ImGui::InputInt("端口", &port);
  ImGui::Checkbox("自动重连", &autoReconnect);
  ImGui::Text("状态: %s", status.c_str());

  if (!isConnected()) {
    if (ImGui::Button("连接")) {
      if (!isValidPort(port)) {
        status = "连接: 失败 -> invalid port";
        Gui::log("连接失败: invalid port %d", port);
        return;
      }
      Mem::ConnectRequest request;
      request.host = hostBuf;
      request.port = static_cast<uint16_t>(port);
      auto response = memService_.connect(
          memService_.captureContext(false), request);
      updateStatus(response.ok(), "连接");
      if (!response.ok()) {
        Gui::log("连接失败 [%s]: %s",
                 Mem::errorCodeName(response.error().code),
                 response.error().message.c_str());
      }
    }
  } else {
    if (ImGui::Button("断开连接")) {
      auto response = memService_.disconnect(
          memService_.captureContext(false));
      if (response.ok()) {
        status = "断开连接: 已断开";
        currentMemType = 0;
        driverStatus = "未初始化";
        Gui::log("服务器连接已断开");
      } else {
        status = "断开连接: 失败";
        Gui::log("断开连接失败 [%s]: %s",
                 Mem::errorCodeName(response.error().code),
                 response.error().message.c_str());
      }
    }
  }

  if (autoReconnect && !isConnected()) {
    if (!isValidPort(port)) {
      status = "自动重连: 失败 -> invalid port";
      return;
    }
    Mem::ConnectRequest request;
    request.host = hostBuf;
    request.port = static_cast<uint16_t>(port);
    auto response = memService_.connect(
        memService_.captureContext(false), request);
    if (response.ok())
      updateStatus(true, "自动重连");
  }
}

void ServerConnectWindow::drawDriverControls() {
  ImGui::Separator();
  ImGui::Text("驱动控制");

  // 显示当前内存类型
  ImGui::Text("当前内存类型: %s", getMemTypeName(memTypeNames, currentMemType));

  ImGui::SameLine();
  if (ImGui::Button("刷新类型")) {
    updateMemType();
  }

  // 卡密输入和驱动初始化
  bool cardKeyChanged = false;
  if (ImGui::InputText("卡密", cardKeyBuf, IM_ARRAYSIZE(cardKeyBuf))) {
    cardKeyChanged = true;
  }
  // 列表显示5 6
  const char *kernelVersionList[] = {"5系", "6系"};
  int currentKernelVersion = (KernelVersionBuf == '5') ? 0 : 1;
  ImGui::PushItemWidth(100);
  bool kernelVersionChanged = false;
  if (ImGui::Combo("##kernelVersion", &currentKernelVersion, kernelVersionList,
                   IM_ARRAYSIZE(kernelVersionList))) {
    KernelVersionBuf = currentKernelVersion == 0 ? '5' : '6';
    kernelVersionChanged = true;
  }
  ImGui::PopItemWidth();
  
  // 如果配置改变，保存配置
  if (cardKeyChanged || kernelVersionChanged) {
    saveConfig();
  }

  if (isConnected()) {
    if (ImGui::Button("初始化驱动")) {
      initializeDriver();
    }
  } else {
    ImGui::BeginDisabled();
    ImGui::Button("初始化驱动");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextColored(ColorScheme::Warning, "(需要先连接服务器)");
  }

  ImGui::Text("驱动状态: %s", driverStatus.c_str());
}

void ServerConnectWindow::loadConfig()
{
	auto& config = ConfigManager::getInstance();
	config.loadConfig("config.ini");
	
	// 加载卡密
	std::string cardKey = config.getString("cardKey", "1142192691366763");
	std::snprintf(cardKeyBuf, sizeof(cardKeyBuf), "%s", cardKey.c_str());
	
	// 加载内核版本
	KernelVersionBuf = config.getChar("kernelVersion", '6');
	
	Gui::log("配置已加载: 卡密=%s, 内核版本=%c", cardKeyBuf, KernelVersionBuf);
}

void ServerConnectWindow::saveConfig()
{
	auto& config = ConfigManager::getInstance();
	
	// 保存卡密
	config.setString("cardKey", std::string(cardKeyBuf));
	
	// 保存内核版本
	config.setChar("kernelVersion", KernelVersionBuf);
	
	// 保存到文件
	if (config.saveConfig("config.ini")) {
		Gui::log("配置已保存: 卡密=%s, 内核版本=%c", cardKeyBuf, KernelVersionBuf);
	} else {
		Gui::log("配置保存失败: 无法写入 config.ini");
	}
}

void ServerConnectWindow::onDraw()
{
	drawConnectionControls();
	drawDriverControls();
}
