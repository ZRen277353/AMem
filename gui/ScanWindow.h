#pragma once

#include "Window.h"
#include "MemoryTypes.h"
#include "../utils/ScopedThread.h"
#include <vector>
#include <string>
#include <cstdint>
#include <mutex>
#include <atomic>
#include <functional>

class ScanWindow : public Window {
public:
    ScanWindow();
    ~ScanWindow();
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
    float addressListRefreshInterval = 1.0f;
    float timeSinceAddressListRefresh = 0.0f;
    bool autoRefreshAddressList = false;

    // 搜索结果自动刷新控制
    float scanResultsRefreshInterval = 5.0f;
    float timeSinceScanResultsRefresh = 0.0f;
    bool autoRefreshScanResults = false;

    // 异步刷新状态
    ScopedThread scanResultsRefreshThread;
    ScopedThread addressListRefreshThread;
    ScopedThread scanThread;
    std::mutex scanResultsMutex;
    std::mutex addressListMutex;
    std::mutex scanProgressMutex;

    // 刷新进度
    std::atomic<int> refreshProgress{0};
    std::atomic<int> refreshTotal{0};

    void onDraw() override;
    void updateScanProgress(float progress, uint64_t matchCount, uint64_t scannedBytes, uint64_t totalBytes);

private:
    void drawScanPanel();
    void drawResultsPanel();
    void drawAddressListPanel();
    void drawScanRangeSettings();
    void drawScanProgressBar();
    void drawMemoryTypeSelectionModal();

    // 扫描功能
    void performFirstScan();
    void performFirstScanAsync();
    void performNextScan();
    void performNextScanAsync();
    void performNewScan();
    void loadScanResults();
    void refreshAddressValues();
    void refreshSingleAddress(int index);
    bool writeAddressValue(int index, const std::string& value);
    void refreshScanResultsValues();
    void refreshScanResultsValuesAsync();
    void refreshAddressValuesAsync();

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
}; 