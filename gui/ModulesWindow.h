#pragma once

#include "Window.h"
#include "../socket/client_singleton.h"
#include <vector>
#include <string>
#include <tuple>
#include <functional>

// 前向声明
class MemoryViewerWindow;

class ModulesWindow : public Window {
public:
	ModulesWindow();
	void onDraw() override;
	unsigned int getWindowFlags() const override;

	void triggerAutoRefresh() { autoRefreshOnce = true; }
	void setMemoryViewerWindow(MemoryViewerWindow* memViewer) { memoryViewerWindow = memViewer; }
	void setOpenMemoryViewerCallback(std::function<MemoryViewerWindow*()> callback) { openMemoryViewerCallback = callback; }

private:
	void drawFilterModal();
	bool passesFilter(const ModuleInfoItem& module) const;
	const char* getModuleTypeName(int type) const;
	MemoryViewerWindow* ensureMemoryViewerWindow();

private:
	bool hasData = false;
	bool autoRefreshOnce = false;
	std::vector<ModuleInfoItem> modules;
	MemoryViewerWindow* memoryViewerWindow = nullptr;
	std::function<MemoryViewerWindow*()> openMemoryViewerCallback = nullptr;
	
	// 过滤功能
	char nameFilter[256] = "";
	uint32_t selectedModuleTypes = static_cast<uint32_t>(-1); // 默认全选
	uint32_t selectedProtectionFlags = static_cast<uint32_t>(-1); // 默认全选
	bool showFilterModal = false;
}; 