#pragma once

#include "Window.h"
#include "MemoryTypes.h"
#include <vector>
#include <string>
#include <cstdint>
#include <thread>
#include <mutex>
#include <atomic>
#include <functional>

// 前向声明
class MemoryViewerWindow;

class ScanWindow : public Window {
public:
    ScanWindow();
    ~ScanWindow();  // 析构函数，用于清理线程
    unsigned int getWindowFlags() const override;

    // 扫描参数
    char valueBuf[128] = "100";
    char value2Buf[128] = "200";
    int scanType = 0;
    int valueType = 0;
    bool hexInput = false;
    
    // 内存类型选择
    uint32_t selectedMemoryTypes = MemoryType::All;
    bool showMemoryTypeModal = false;

    // 扫描结果
    std::vector<ScanResultItem> scanResults;
    std::vector<AddressListItem> addressList;
    std::vector<char> selectedScanResults;
    
    // 扫描状态
    bool scanInProgress = false;
    bool scanCancelled = false;
    float scanProgress = 0.0f;
    uint64_t scanMatchCount = 0;
    uint64_t scanTotalBytes = 0;
    uint64_t scanScannedBytes = 0;
    int totalScanResults = 0;
    bool scanCompleted = false;
    bool scanError = false;
    
    // 结果分页
    int resultOffset = 0;
    int resultPageSize = 1000;
    
    // 地址列表自动刷新控制
    float addressListRefreshInterval = 1.0f;  // 地址列表自动刷新间隔（秒）
    float timeSinceAddressListRefresh = 0.0f;
    bool autoRefreshAddressList = false;  // 是否启用自动刷新
    
    // 搜索结果自动刷新控制
    float scanResultsRefreshInterval = 5.0f;  // 搜索结果自动刷新间隔（秒，默认5秒）
    float timeSinceScanResultsRefresh = 0.0f;
    bool autoRefreshScanResults = false;  // 是否启用自动刷新
    
    // 异步刷新状态
    std::atomic<bool> scanResultsRefreshing{false};  // 是否正在刷新
    std::atomic<bool> addressListRefreshing{false};  // 地址列表是否在刷新
    std::thread scanResultsRefreshThread;  // 刷新线程
    std::thread addressListRefreshThread;  // 地址列表刷新线程
    std::thread scanThread;  // 扫描线程
    std::mutex scanResultsMutex;  // 保护scanResults的互斥锁
    std::mutex addressListMutex;  // 保护addressList的互斥锁
    std::mutex scanProgressMutex;  // 保护扫描进度的互斥锁
    
    // 刷新进度
    std::atomic<int> refreshProgress{0};
    std::atomic<int> refreshTotal{0};

    // 进程信息（从主窗口获取）
    int* selectedPid = nullptr;
    std::string* selectedName = nullptr;
    MemoryViewerWindow* memoryViewerWindow = nullptr;
    
    // 回调函数：用于请求打开内存查看器窗口
    std::function<MemoryViewerWindow*()> openMemoryViewerCallback = nullptr;

    void onDraw() override;
    void updateScanProgress(float progress, uint64_t matchCount, uint64_t scannedBytes, uint64_t totalBytes);
    void setProcessInfo(int* pid, std::string* name);
    void setMemoryViewerWindow(MemoryViewerWindow* memViewer);
    void setOpenMemoryViewerCallback(std::function<MemoryViewerWindow*()> callback);

private:
    void drawScanPanel();
    void drawResultsPanel();
    void drawAddressListPanel();
    void drawScanRangeSettings();
    void drawScanProgressBar();
    void drawMemoryTypeSelectionModal();
    
    // 扫描功能
    void performFirstScan();
    void performFirstScanAsync();  // 异步版本
    void performNextScan();
    void performNextScanAsync();  // 异步版本
    void performNewScan();
    void loadScanResults();
    void refreshAddressValues();
    void refreshSingleAddress(int index);  // 刷新单个地址
    bool writeAddressValue(int index, const std::string& value);  // 写入地址值
    void refreshScanResultsValues();  // 刷新搜索结果的值（同步版本）
    void refreshScanResultsValuesAsync();  // 刷新搜索结果的值（异步版本）
    void refreshAddressValuesAsync();  // 刷新地址列表（异步版本）
    
    // 扫描参数验证和处理
    bool validateScanParameters(bool isFirstScan);
    bool prepareValueBytes(std::vector<unsigned char>& valueBytes, bool isFirstScan);
    bool validateValueRange(const std::vector<unsigned char>& value1, const std::vector<unsigned char>& value2);
    
    // 数据类型转换
    std::vector<unsigned char> parseValueInput(const std::string& input, int type);
    std::string formatValueOutput(const std::vector<unsigned char>& data, int type);
    std::string formatValueOutput(const unsigned char* data, int type);
    std::string formatScanResultValue(uint64_t value, int type);
    std::string formatScanResultValueHex(uint64_t value, int type);
    uint32_t getScanTypeFlag();
    uint32_t getValueTypeFlag();
    uint32_t getValueTypeSize();
    const char* getScanTypeName(int scanType) const;
    const char* getValueTypeName(int valueType) const;
    
    // 辅助方法：确保内存查看器窗口存在
    MemoryViewerWindow* ensureMemoryViewerWindow();
}; 