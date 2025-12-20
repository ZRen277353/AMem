#pragma once

#include "Window.h"
#include "../PointerScan/PointerScanner.hpp"
#include "../PointerScan/formatter.h"
#include <vector>
#include <string>
#include <memory>
#include <thread>
#include <functional>

// 前向声明
class MemoryViewerWindow;

class PointerChainWindow : public Window {
public:
    PointerChainWindow();
    ~PointerChainWindow();

    void onDraw() override;
    unsigned int getWindowFlags() const override;
    void setProcessInfo(int* pid, std::string* name);
    void setMemoryViewerWindow(MemoryViewerWindow* memViewer);
    void setOpenMemoryViewerCallback(std::function<MemoryViewerWindow*()> callback);

private:
    // 扫描参数
    char targetAddressBuf[32] = "0";
    int maxDepth = 5;
    int maxOffset = 500;
    uint32_t maxLimit = 9999999;
    bool limitResults = true;
    bool useExistingPointers = false; // 是否使用已有的指针数据
    
    // 扫描器和格式化器
    std::shared_ptr<memchainer::PointerScanner> scanner;
    std::unique_ptr<memchainer::PointerFormatter> formatter;
    
    // 潜在指针数据状态
    bool pointersLoaded = false;       // 是否已加载潜在指针
    uint32_t loadedPointerCount = 0;   // 已加载的指针数量
    bool pointerLoadInProgress = false; // 指针加载中
    std::thread pointerLoadThread;     // 指针加载线程
    
    // 指针加载进度
    uint32_t pointerLoadCurrentRegion = 0;  // 当前区域索引
    uint32_t pointerLoadTotalRegions = 0;   // 总区域数
    float pointerLoadProgress = 0.0f;       // 加载进度 (0.0 - 1.0)
    
    // 扫描状态
    bool scanInProgress = false;
    bool scanCompleted = false;
    bool scanCancelled = false;
    
    // 扫描进度信息
    memchainer::PointerScanner::ScanProgressInfo scanProgressInfo;
    
    // 扫描线程
    std::thread scanThread;
    
    // 指针链结果
    std::vector<std::list<memchainer::PointerChainNode>> chains;
    
    // 结果显示
    int selectedChainIndex = -1;
    int displayOffset = 0;
    int displayLimit = 100;
    
    // 潜在指针展示相关
    bool showPotentialPointers = false;  // 是否显示潜在指针面板
    int pointerDisplayOffset = 0;        // 指针显示偏移
    int pointerDisplayLimit = 100;       // 指针显示数量限制
    int selectedPointerIndex = -1;       // 选中的指针索引
    bool sortByRefCount = true;          // 是否按引用次数排序
    int minRefCountFilter = 0;           // 最小引用次数过滤
    int minOffsetCountFilter = 0;        // 最小偏移量数量过滤
    std::vector<memchainer::PointerAllData*> sortedPointers;  // 排序后的指针列表
    
    // 导出选项
    char exportFileBuf[256] = "pointer_chains.txt";
    
    // 进程信息
    int* selectedPid = nullptr;
    std::string* selectedName = nullptr;
    MemoryViewerWindow* memoryViewerWindow = nullptr;
    
    // 回调函数：用于请求打开内存查看器窗口
    std::function<MemoryViewerWindow*()> openMemoryViewerCallback = nullptr;
    
    // UI 绘制方法
    void drawScanPanel();
    void drawProgressPanel();
    void drawResultsPanel();
    void drawChainDetail(const std::list<memchainer::PointerChainNode>& chain);
    void drawPotentialPointersPanel();  // 新增：潜在指针展示面板
    void drawPointerDetail(memchainer::PointerAllData* pointer);  // 新增：指针详情展示
    
    // 潜在指针加载功能
    void startLoadPointers();
    void cancelLoadPointers();
    void performLoadPointers();
    void updateLoadProgress(uint32_t currentRegion, uint32_t totalRegions, float progress);
    
    // 扫描功能
    void startScan();
    void cancelScan();
    void performScan(uint64_t targetAddress);
    void updateProgress(const memchainer::PointerScanner::ScanProgressInfo& info);
    
    // 导出功能
    void exportToFile();
    
    // 工具方法
    std::string formatAddress(uint64_t address);
    std::string formatOffset(int64_t offset);
    std::string formatChainString(const std::list<memchainer::PointerChainNode>& chain);
    
    // 辅助方法：确保内存查看器窗口存在
    MemoryViewerWindow* ensureMemoryViewerWindow();
    
    // 新增：更新排序后的指针列表
    void updateSortedPointers();
}; 