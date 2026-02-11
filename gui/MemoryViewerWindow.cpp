#include "MemoryViewerWindow.h"
#include "AppContext.h"
#include "Gui.h"
#include "DisassemblyHelper.h"
#include "ColorScheme.h"
#include "EventBus.h"
#include "Events.h"
#include "../imgui/imgui.h"
#include "../socket/client_singleton.h"
#include <algorithm>
#include <cstdint>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <fstream>
#include <cmath>
#include <memory>


// 解析地址表达式，支持十六进制加减运算
// 例如: "1000", "1000+200", "5000-100", "ABCD + 10"
bool MemoryViewerWindow::parseAddressExpression(const char* expr, uint64_t& result)
{
    if (!expr || expr[0] == '\0') return false;
    
    std::string s = expr;
    // 去除所有空格
    s.erase(std::remove(s.begin(), s.end(), ' '), s.end());
    
    if (s.empty()) return false;
    
    // 查找+或-运算符（从索引1开始，避免将负号误认为运算符）
    size_t opPos = std::string::npos;
    char op = '\0';
    
    // 从后向前查找最后一个+或-（支持多次运算，从右向左计算）
    for (size_t i = s.length() - 1; i > 0; i--) {
        if (s[i] == '+' || s[i] == '-') {
            opPos = i;
            op = s[i];
            break;
        }
    }
    
    if (opPos == std::string::npos) {
        // 没有运算符，直接解析十六进制数
        char* endPtr = nullptr;
        result = strtoull(s.c_str(), &endPtr, 16);
        return endPtr != s.c_str() && *endPtr == '\0';
    }
    
    // 有运算符，递归解析左右两边
    std::string leftStr = s.substr(0, opPos);
    std::string rightStr = s.substr(opPos + 1);
    
    uint64_t left = 0, right = 0;
    
    // 递归解析左边（支持嵌套表达式）
    if (!parseAddressExpression(leftStr.c_str(), left)) return false;
    
    // 解析右边
    char* endPtr = nullptr;
    right = strtoull(rightStr.c_str(), &endPtr, 16);
    if (endPtr == rightStr.c_str() || *endPtr != '\0') return false;
    
    // 计算结果
    if (op == '+') {
        result = left + right;
    } else if (op == '-') {
        result = left - right;
    }
    
    return true;
}

MemoryViewerWindow::MemoryViewerWindow()
{
    name = "内存查看器";
    loadStructDefinitions();
    
    // 初始化默认地址，避免首次打开时地址为0导致无法显示
    viewAddress = 0;
    pageBaseAddress = 0;
    targetAddress = 0;
    
    // 初始化偏移链输入框
    strcpy(newOffsetBuf, "0");
    
    // 调整buffer大小为一页
    buffer.resize(viewSize);
    // 清空buffer避免显示垃圾数据
    std::fill(buffer.begin(), buffer.end(), 0);
    
    // 初始化反汇编引擎
    disassemblyHelper = std::make_unique<DisassemblyHelper>();
    // 默认使用ARM64架构（Android通常是ARM64）
    if (DisassemblyHelper::isCapstoneAvailable()) {
        disassemblyInitialized = disassemblyHelper->initialize(DisassemblyHelper::Architecture::ARM64);
        if (!disassemblyInitialized) {
            // 如果ARM64失败，尝试ARM32
            disassemblyInitialized = disassemblyHelper->initialize(DisassemblyHelper::Architecture::ARM);
        }
    }
    
    // 初始化反汇编地址输入框
    disassemblyAddressBuf[0] = '\0';
    disassemblyAddress = 0;
    disassemblyBuffer.clear();
    disassemblyBufferValid = false;
    cachedDisassemblyResult = DisassemblyResult();
    scrollToDisassemblyAddress = false;

    // 订阅地址跳转事件
    navSubscriptionId = EventBus::Get().subscribe<NavigateToAddressEvent>(
        [this](const NavigateToAddressEvent& e) {
            jumpToAddress(e.address);
        });
}

MemoryViewerWindow::~MemoryViewerWindow()
{
    EventBus::Get().unsubscribe<NavigateToAddressEvent>(navSubscriptionId);
}

unsigned int MemoryViewerWindow::getWindowFlags() const
{
    return ImGuiWindowFlags_NoDocking;
}

void MemoryViewerWindow::jumpToAddress(uint64_t address)
{
    // 保存目标地址
    targetAddress = address;
    
    // 计算页首地址（4KB对齐）
    pageBaseAddress = (address / pageSize) * pageSize;
    
    // 设置视图地址为页首
    viewAddress = pageBaseAddress;
    
    // 读取整页数据
    viewSize = pageSize;  // 确保读取一整页
    buffer.resize(viewSize);
    
    if (ReadProcessMemoryBytes(viewAddress, (uint32_t)viewSize, buffer)) {
        // 读取成功
        Gui::log("内存查看器已跳转到地址: 0x%llX (页首: 0x%llX, 页内偏移: 0x%llX)", 
                 address, pageBaseAddress, address - pageBaseAddress);
    } else {
        // 读取失败，清空buffer
        std::fill(buffer.begin(), buffer.end(), 0);
        Gui::log("读取内存失败: 0x%llX", address);
    }
    
    // 添加到历史记录
    addToHistory(address);
    
    // 标记需要滚动到目标地址
    scrollToTarget = true;
    
    pOpen = true;  // 确保窗口打开
    shouldBringToFront = true;  // 标记需要置于前台
}

void MemoryViewerWindow::addToHistory(uint64_t address)
{
    // 如果不是在历史记录末尾，删除后面的记录
    if (historyIndex >= 0 && historyIndex < (int)addressHistory.size() - 1) {
        addressHistory.erase(addressHistory.begin() + historyIndex + 1, addressHistory.end());
    }
    
    // 添加新地址
    addressHistory.push_back(address);
    historyIndex = (int)addressHistory.size() - 1;
    
    // 限制历史记录大小
    if (addressHistory.size() > 50) {
        addressHistory.erase(addressHistory.begin());
        historyIndex--;
    }
}

void MemoryViewerWindow::refreshMemory()
{
    if (AppContext::Get().hasProcess() && viewAddress != 0) {
        // 确保buffer大小正确
        buffer.resize(viewSize);
        
        // 使用调试端口进行自动刷新，避免阻塞主端口
        if (!ReadProcessMemoryBytes(viewAddress, (uint32_t)viewSize, buffer, PORT_DEBUG)) {
            // 读取失败时，清空buffer避免显示错误数据
            std::fill(buffer.begin(), buffer.end(), 0);
            Gui::log("刷新内存失败: 0x%llX", viewAddress);
        }
    } else {
        // 没有进程或地址无效时，清空buffer
        buffer.resize(viewSize);
        std::fill(buffer.begin(), buffer.end(), 0);
    }
}

void MemoryViewerWindow::writeMemoryByte(uint64_t address, unsigned char value)
{
    // 检查进程是否附加
    if (!AppContext::Get().hasProcess()) {
        Gui::log("错误：未附加进程");
        return;
    }
    
    std::vector<unsigned char> data = { value };
    if (WriteProcessMemoryBytes(address, 1, data)) {
        Gui::log("成功写入内存: 0x%llX = 0x%02X", address, value);
        // 写入成功后刷新内存显示
        refreshMemory();
    } else {
        Gui::log("错误：写入内存失败 - 地址 0x%llX", address);
    }
}

void MemoryViewerWindow::onDraw()
{
    if (!pOpen) return;

    if (ImGui::Begin(name.c_str(), &pOpen, ImGuiWindowFlags_None))
    {
        if (AppContext::Get().hasProcess()) {
            ImGui::TextColored(ColorScheme::SuccessBright, "已附加: %s (PID %d)",
                AppContext::Get().selectedName.c_str(), AppContext::Get().selectedPid.load());
        } else {
            ImGui::TextDisabled("未附加进程");
        }
        ImGui::Separator();

        // 添加标签页
        if (ImGui::BeginTabBar("MemoryViewerTabs"))
        {
            if (ImGui::BeginTabItem("内存查看器"))
            {
                drawMemoryViewerPanel();
                ImGui::EndTabItem();
            }
            
            if (ImGui::BeginTabItem("地址列表"))
            {
                drawAddressList();
                ImGui::EndTabItem();
            }
            
            if (ImGui::BeginTabItem("数据结构分析"))
            {
                drawStructAnalyzerPanel();
                ImGui::EndTabItem();
            }
            
            if (ImGui::BeginTabItem("反汇编查看器"))
            {
                drawDisassemblyPanel();
                ImGui::EndTabItem();
            }
            
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}
