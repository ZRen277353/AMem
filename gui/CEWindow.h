#pragma once

#include "Window.h"
#include "../socket/client_singleton.h"
#include <string>
#include <vector>

// 前向声明
class ScanWindow;
class MemoryViewerWindow;
class BreakpointWindow;
class LuaScriptWindow;
class ServerConnectWindow;
class LogWindow;

class CEWindow : public Window {
public:
    CEWindow();
    unsigned int getWindowFlags() const override;

protected:
    void onDraw() override;

private:
    void drawMenuBar();
    void drawTopProcessBar();
    void drawSelectedProcessBanner();
    void drawProcessSelectModal();
    
    // 窗口管理方法
    void openScanWindow();
    void openMemoryViewerWindow();
    void openBreakpointWindow();
    void openModulesWindow();
    void openLuaScriptWindow();
    void openServerConnectWindow();
    void openLogWindow();

    // 状态变量
    bool openProcessModal = false;
    int selectedPid = 0;
    std::string selectedName = "";
    
    // 子窗口指针
    ScanWindow* scanWindow = nullptr;
    MemoryViewerWindow* memoryViewerWindow = nullptr;
    BreakpointWindow* breakpointWindow = nullptr;
};