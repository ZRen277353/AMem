#pragma once

#include "Window.h"
#include <string>
#include <vector>

// 前向声明
class ScanWindow;
class MemoryViewerWindow;
class BreakpointWindow;
class LuaScriptWindow;
class ServerConnectWindow;
class LogWindow;
namespace Mem {
class IMemService;
}

class CEWindow : public Window {
public:
    explicit CEWindow(Mem::IMemService& memService);
    unsigned int getWindowFlags() const override;

protected:
    void onDraw() override;

private:
    Mem::IMemService& memService_;
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
#ifdef HAVE_AI_CHAT
    void openAIChatWindow();
#endif
#ifdef HAVE_NATIVE_IPC
    void openNativeAgentIpcWindow();
#endif

    // 状态变量
    bool openProcessModal = false;
};
