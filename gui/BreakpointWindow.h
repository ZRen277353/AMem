#pragma once

#include "Window.h"
#include "../mem/MemTypes.h"
#include "../mem/MemResult.h"
#include <vector>
#include <string>
#include <cstdint>
#include <map>
#include <functional>
#include <memory>

// 前向声明
class DisassemblyHelper;
struct DisassemblyResult;
namespace Mem {
class IMemService;
}

// 断点类型枚举
enum class BreakpointType {
    // EXECUTE = 0,    // 执行断点
    // WRITE = 1,      // 写入断点
    // READ_WRITE = 3  // 读写断点
        HW_BREAKPOINT_EMPTY = 0,
        HW_BREAKPOINT_R = 1,
        HW_BREAKPOINT_W = 2,
        HW_BREAKPOINT_RW = HW_BREAKPOINT_R | HW_BREAKPOINT_W,
        HW_BREAKPOINT_X = 4,
        HW_BREAKPOINT_INVALID = HW_BREAKPOINT_RW | HW_BREAKPOINT_X,
};

// 断点大小枚举
enum class BreakpointSize {
    SIZE_1 = 1,
    SIZE_2 = 2,
    SIZE_4 = 4,
    SIZE_8 = 8
};

// PC地址命中统计结构
struct PCHitStat {
    uint64_t pc_address;
    int hit_count;
    uint64_t first_hit_time;
    uint64_t last_hit_time;
    
    PCHitStat() : pc_address(0), hit_count(0), first_hit_time(0), last_hit_time(0) {}
};

// 断点信息结构
struct BreakpointInfo {
    uint64_t address;
    BreakpointType type;
    BreakpointSize size;
    std::string description;
    bool enabled;
    bool suspended;
    int hitCount;
    std::vector<Mem::BreakpointHit> hitHistory;
    std::map<uint64_t, PCHitStat> pcHitStats;  // PC地址命中统计
    int dataVersion = 0;  // 数据版本号，用于检测变化
    
    BreakpointInfo() : address(0), type(BreakpointType::HW_BREAKPOINT_RW), 
                      size(BreakpointSize::SIZE_1), enabled(false), 
                      suspended(false), hitCount(0) {}
};

class BreakpointWindow : public Window {
public:
    explicit BreakpointWindow(Mem::IMemService& memService);

    void draw() override;
    unsigned int getWindowFlags() const override;

private:
    bool readTargetMemory(
        uint64_t address, uint32_t size,
        std::vector<unsigned char>& bytes,
        Mem::MemoryReadChannel channel = Mem::MemoryReadChannel::Foreground,
        Mem::Error* error = nullptr);
    void resetProcessState();
    void drawBreakpointList();
    void drawBreakpointControls();
    void drawAddBreakpointDialog();
    
    // 辅助方法
    const char* getBreakpointTypeName(BreakpointType type);
    const char* getBreakpointSizeName(BreakpointSize size);
    void refreshBreakpointHitInfo(int index);
    void updatePCHitStatistics(int index);
    void markDetailWindowsForRefresh(int breakpointIndex);
    void refreshAllDetailWindows();
    std::string formatTime(uint64_t timestamp);
    std::string formatTimeDiff(uint64_t start, uint64_t end);
    void addBreakpoint(uint64_t address, BreakpointType type, BreakpointSize size, const std::string& description);
    
    void removeBreakpoint(int index);
    void toggleBreakpoint(int index);
    void suspendBreakpoint(int index);
    void resumeBreakpoint(int index);
    
    // 状态变量
    Mem::IMemService& memService_;
    std::vector<BreakpointInfo> breakpoints;
    uint64_t observedProcessRevision = 0;
    
    // 添加断点对话框状态
    bool showAddBreakpointDialog = false;
    char newBreakpointAddress[32] = "";
    int newBreakpointType = 0;
    int newBreakpointSize = 0;
    char newBreakpointDescription[256] = "";
    
    // 全局设置
    bool autoRefreshHitInfo = true;
    float refreshInterval = 1.0f;
    
    // 弹窗管理
    struct BreakpointDetailWindow {
        bool isOpen = false;
        int breakpointIndex = -1;
        int selectedHitIndex = -1;
        uint64_t selectedPCAddress = 0;  // 选中的PC地址，用于显示寄存器信息
        bool showPCStatistics = true;
        bool showRegisterInfo = true;
        bool showHitHistoryDetails = true;
        bool showDisassembly = true;  // 显示反汇编
        char pcFilterBuffer[64] = "";
        std::string windowTitle;
        
        // 缓存和优化
        std::vector<std::pair<uint64_t, PCHitStat*>> cachedSortedStats;
        std::string lastFilterStr = "";
        bool needsStatRefresh = true;
        bool needsHitRefresh = true;
        float lastHitRefreshTime = 0.0f;
        int lastDataVersion = 0;  // 用于检测数据变化
        
        // 显示选项
        bool showAdvancedInfo = false;
        bool compactMode = false;
        int maxDisplayedHits = 1000;  // 限制显示的命中记录数量
        
        // 排序选项
        int sortBy = 0;  // 0:命中次数, 1:PC地址, 2:首次命中时间, 3:最后命中时间
        bool sortDescending = true;
        int lastSortBy = -1;  // 缓存上次的排序方式
        bool lastSortDescending = true;
        
        // 反汇编缓存
        struct DisassemblyCache {
            uint64_t cachedPCAddress = 0;  // 缓存的PC地址
            int cachedBeforeCount = 0;     // 缓存的前置指令数
            int cachedAfterCount = 0;      // 缓存的后置指令数
            std::vector<unsigned char> cachedMemoryData;  // 缓存的内存数据
            std::shared_ptr<DisassemblyResult> cachedResult;  // 缓存的反汇编结果
            bool isValid = false;  // 缓存是否有效
        };
        DisassemblyCache disasmCache;
        bool autoRefreshDisasm = false;  // 自动刷新反汇编
        float disasmRefreshInterval = 2.0f;  // 反汇编刷新间隔（秒）
        float lastDisasmRefreshTime = 0.0f;  // 上次反汇编刷新时间

        int disasmBeforeCount = 4;
        int disasmAfterCount = 4;
        int fpDisplayMode = 0;  // 0: hex, 1: double, 2: float, 3: half, 4: i64, 5: i32
        int fpRegGroup = 0;     // 0: all, 1: args, 2: saved, 3: temp
        bool fpFilterNonZero = false;
        bool fpHighlightSpecial = true;
        
        BreakpointDetailWindow() = default;
        BreakpointDetailWindow(int bpIndex, const std::string& title) 
            : breakpointIndex(bpIndex), windowTitle(title), isOpen(true) {}
    };
    
    std::vector<BreakpointDetailWindow> detailWindows;
    
    // 弹窗相关方法
    void openBreakpointDetailWindow(int breakpointIndex);
    void drawBreakpointDetailWindow(BreakpointDetailWindow& detailWindow);
    void drawPCHitStatisticsInWindow(BreakpointDetailWindow& detailWindow);
    void drawDetailedHitInfoInWindow(BreakpointDetailWindow& detailWindow);
    void drawRegisterInfoInWindow(const Mem::BreakpointHit& hit, BreakpointDetailWindow& detailWindow);
    void drawDisassemblyInWindow(uint64_t address, const uint8_t* code, size_t codeSize);
    void drawDisassemblyForPC(uint64_t pcAddress, int beforeCount, int afterCount, BreakpointDetailWindow& detailWindow);
    void closeBreakpointDetailWindow(int windowIndex);
    
    // 反汇编助手
    std::unique_ptr<DisassemblyHelper> disassemblyHelper;
    bool disassemblyInitialized = false;
};
